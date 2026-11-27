// =============================================================================
// net/action_sync.cpp -- online players by ACTIONS, not buttons (see the .h).
//
// The fight tree (decompiled 2026-11-27; notes online-coop.md s.11):
//   tree instance: +0x10 actor, **(+0x1C) node array (node N = *(**(tree+0x1C)
//     + 4N), sub_820C0738), +0x48 current STATE node, +0x90 exit the decision
//     picked this frame (-1 none), +0x9C flags byte (bit 0x80 = skip the
//     decision: sub_820C1170 tests it before calling sub_820C10A0)
//   node: +0x1C u16 type (0 state; 3/4/5 exits), +0x1E u16 number
//     (= the Crash.bfig / <titan>.bfig listing number)
//   sub_820C1170 (f1 dt, r3 tree)  = per-frame update: decision -> +0x90
//   sub_820C1578 (r3 tree, r4 node, r5 exit) = CHANGE STATE (the only way in;
//     wrapped in players/more_players.cpp, which calls us)
//   sub_820C0DF8 (r3 tree, r4 node) = "go to node" = what scripts' messages use
//     to force a state (CCharacterFightTreeBehaviour slot 5, message type 1)
// Probe results (offline, save 2, no buttons): forcing 429 AttackHeavyAWindup
// played the whole heavy attack (lunge, cooldown, back to Ground); 109 Jump =
// a real jump through the physics; health is never touched by a state.
// =============================================================================
#include "action_sync.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "../players/more_players.h"
#include "mask_sync.h"
#include "state_sync.h"
#include "session_sync.h"
#include "world_sync.h"

REXCVAR_DEFINE_BOOL(online_action_sync, true, "CrashMoM",
                    "Online co-op: other players' copies obey their owner's fight-tree states (actions) "
                    "instead of deciding from replayed buttons");

extern "C" REX_FUNC(__imp__sub_820C1170);  // tree per-frame update (original)
extern "C" REX_FUNC(__imp__sub_820C1578);  // change state (original; our wrapper is in more_players.cpp)
extern "C" REX_FUNC(sub_820C1578);        // ... through that wrapper (it lets our own change through)
extern "C" REX_FUNC(sub_820C1BB8);        // (below)
extern "C" REX_FUNC(__imp__sub_820C0ED8);  // variant picker (original)
extern "C" REX_FUNC(__imp__sub_820C1BB8);  // change state of a master tree + its slave (original)
extern "C" REX_FUNC(__imp__sub_820C1210);  // set a tree's link mode (r3 tree, r4 mode, r5 other tree)

namespace tree_probe {  // debug/tree_probe.cpp (console `tree`)
void BeforeUpdate(PPCContext& ctx, uint8_t* base, uint32_t tree);
void AfterUpdate(uint32_t tree);
}  // namespace tree_probe

namespace net::action_sync {
namespace {

constexpr int kPlayers = 4;
constexpr uint32_t kTreeActor = 0x10, kTreeNodes = 0x1C, kTreeState = 0x48, kTreeExit = 0x90,
                   kTreeFlags = 0x9C;
constexpr uint8_t kNoDecision = 0x80;
constexpr uint32_t kNodeNumber = 0x1E;
constexpr uint32_t kTreeVariant = 0x4C, kTreeVariant2 = 0x50;
// Link mode (+0x94; set by sub_820C1210): 2 = MASTER, its slave tree at +0x98
// (a ridden titan's tree carries the rider's); 3 = a SLAVE (its own update is
// skipped, sub_820C1170): it follows its master and is never driven by us.
constexpr uint32_t kTreeMode = 0x94;
constexpr uint32_t kModeSlave = 3;
// A state message older than this when its copy could first take it was meant
// for another moment (e.g. on foot before the ride): skipped, not applied.
constexpr auto kTooOld = std::chrono::milliseconds(2000);

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
int NodeNumber(uint32_t node) {
  if (!node || !guest_memory::Readable(node, 0x20)) return -1;
  const uint8_t* p = Guest(node + kNodeNumber);
  return int(p[0] << 8 | p[1]);
}

struct Wanted {        // the newest state an owner told us about
  bool valid = false;
  uint32_t seq = 0;    // owner's counter (re-entries of one state differ)
  int node = 0;
  int variant = 0;     // tree +0x4C node the owner's state picked (0 none)
  int exit = 0;        // exit node the owner took into it (0 none)
  int variant2 = 0;    // tree +0x50 (the slave's variant)
  bool with_slave = false;  // owner changed it through sub_820C1BB8
  uint8_t body = 0;    // 0 character, 1 titan
  std::chrono::steady_clock::time_point received;
};

std::mutex g_mutex;
// One slot per player AND body: a rider and the titan they ride are two trees
// with their own states (one shared slot made them overwrite each other).
std::array<std::array<Wanted, 2>, kPlayers> g_wanted{};
std::array<std::array<uint32_t, 2>, kPlayers> g_applied{};  // seq last put on each copy
uint32_t g_local_seq = 0;
std::vector<std::vector<uint8_t>> g_outgoing;
bool g_forcing = false;  // game thread: our own state change is running
uint32_t g_forcing_tree = 0;
int g_forcing_variant = 0;   // the variant picker's answers for that tree meanwhile
int g_forcing_variant2 = 0;
int g_inside_slave_change = 0;  // owner: inside sub_820C1BB8 (its inner sub_820C1578 isn't sent alone)
uint32_t g_refused = 0;   // copy state changes refused (for the trace)
// ENEMIES (host -> guests, online_enemy_states): newest state per host net id,
// and the sequence last put on each local copy (by actor).
std::unordered_map<uint32_t, Wanted> g_enemy_wanted;
std::unordered_map<uint32_t, uint32_t> g_enemy_applied;
uint32_t g_enemy_seq = 0;
// RIDER LINKS (2026-11-27 playtest crash): jacking links the rider's tree to
// the titan's (titan = master +0x94 2, slave at +0x98; rider = slave mode 3);
// the game unlinks them when the titan's tree runs out of states (sub_820C20A8
// -> sub_820C2138), which a forced copy never does. Seen per frame: slave
// tree -> its master, and since when it has been a slave.
std::unordered_map<uint32_t, uint32_t> g_master_of;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_slave_since;
// OWN DECISIONS for a while (pocketing): until then the copy is not driven.
std::array<std::chrono::steady_clock::time_point, kPlayers> g_free_until{};
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_enemy_free_until;  // actor -> until

bool SeqNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

// Which player's body is `actor`, and is it their titan?
int PlayerOfActor(uint32_t actor, uint8_t& body) {
  if (!actor) return -1;
  for (int p = 0; p < kPlayers; ++p) {
    if (actor == more_players::TitanOfPlayer(p)) { body = 1; return p; }
    if (actor == more_players::CharacterOfPlayer(p)) { body = 0; return p; }
  }
  return -1;
}

int PlayerOfTree(uint32_t tree, uint8_t& body) {
  if (!tree || !guest_memory::Readable(tree, 0xA0)) return -1;
  return PlayerOfActor(Read32(tree + kTreeActor), body);
}

// Do we drive player p's copy right now (this tree being its body `body`)?
// Needs a message for that same body (a titan tree's numbers mean nothing to a
// Crash tree) and state_sync's "on foot on both PCs, same area, no cutscene".
bool IsSlave(uint32_t tree) { return Read32(tree + kTreeMode) == kModeSlave; }

bool Free(int p) { return std::chrono::steady_clock::now() < g_free_until[p]; }

bool Driving(int p, uint8_t body) {
  if (!REXCVAR_GET(online_action_sync) || !net::Active() || p == net::LocalSlot()) return false;
  if (Free(p)) return false;
  {
    std::lock_guard lock(g_mutex);
    if (!g_wanted[p][body & 1].valid) return false;
  }
  return net::PuppetOnFoot(p);
}

bool Trace() { return net::DebugTraceOn(); }

// Guest: is this tree an enemy copy we drive (host net id in `net_id`)? Its
// actor is synced (bound to a host entity), not a player's body, and the host
// has told us a state for it; play is on, no loading / cutscene grace.
bool EnemyDriving(uint32_t tree, uint32_t& net_id) {
  if (!net::Active() || net::IsHost() || !net::world_sync::EnemyStates()) return false;
  if (!more_players::InPlay() || more_players::Loading() || net::session_sync::InTransitionOrCutsceneGrace()) {
    return false;
  }
  if (!tree || !guest_memory::Readable(tree, 0xA0)) return false;
  const uint32_t actor = Read32(tree + kTreeActor);
  uint8_t body = 0;
  if (!actor || PlayerOfActor(actor, body) >= 0) return false;
  {
    std::lock_guard lock(g_mutex);
    const auto f = g_enemy_free_until.find(actor);
    if (f != g_enemy_free_until.end()) {
      if (std::chrono::steady_clock::now() < f->second) return false;  // being jacked here
      g_enemy_free_until.erase(f);
    }
  }
  net_id = net::world_sync::NetIdOfActor(actor);
  if (!net_id) return false;
  std::lock_guard lock(g_mutex);
  return g_enemy_wanted.count(net_id) != 0;
}

// The state, variant, exit and second variant of `tree` into msg[at..at+7].
void PutState(std::vector<uint8_t>& msg, size_t at, uint32_t tree, int node, uint32_t exit) {
  const int variant = std::max(0, NodeNumber(Read32(tree + kTreeVariant)));
  const int exit_number = std::max(0, NodeNumber(exit));
  const int variant2 = std::max(0, NodeNumber(Read32(tree + kTreeVariant2)));
  const int v[4] = {node, variant, exit_number, variant2};
  for (int i = 0; i < 4; ++i) {
    msg[at + 2 * i] = uint8_t(v[i]);
    msg[at + 2 * i + 1] = uint8_t(v[i] >> 8);
  }
}

// Put a copy's tree into an owner's state, the game's own way (the owner's
// exit + variants answered to the picker); `who` names it in the trace.
void ForceState(PPCContext& ctx, uint8_t* base, uint32_t tree, const Wanted& w, const std::string& who) {
  const uint32_t nodes = Read32(Read32(tree + kTreeNodes));
  const uint32_t node = Read32(nodes + 4 * uint32_t(w.node));
  const uint32_t exit = w.exit > 0 ? Read32(nodes + 4 * uint32_t(w.exit)) : 0;
  if (NodeNumber(node) != w.node || (w.exit > 0 && NodeNumber(exit) != w.exit)) {
    REXLOG_WARN("Online: {} copy has no node {} / exit {} (state from its owner skipped)", who, w.node, w.exit);
    return;
  }
  // A LINKED change (the owner's tree had its rider/slave tree: a sub-tree node,
  // type 6, or a master change) needs this copy's link too: the game reads the
  // slave tree (+0x98) without a check. Here it may not be linked (yet / any
  // more): skipped, the next state comes. (2026-11-27 guest crash: read of
  // 0x48 in sub_820C1BB8 from our forced change.)
  const uint16_t type = uint16_t(Guest(node + 0x1C)[0] << 8 | Guest(node + 0x1C)[1]);
  const uint32_t slave = Read32(tree + 0x98);
  if ((w.with_slave || type == 6) && (!slave || !guest_memory::Readable(slave, 0xA0))) {
    if (Trace()) REXLOG_INFO("OnlineTrace: {} linked state {} skipped (no rider linked here)", who, w.node);
    return;
  }
  const PPCContext saved = ctx;
  ctx.r3.u64 = tree;
  ctx.r4.u64 = node;
  ctx.r5.u64 = exit;
  g_forcing = true;
  g_forcing_tree = tree;
  g_forcing_variant = w.variant;
  g_forcing_variant2 = w.variant2;
  if (w.with_slave) {
    sub_820C1BB8(ctx, base);  // the owner's way: master + slave together
  } else {
    sub_820C1578(ctx, base);
  }
  g_forcing = false;
  g_forcing_tree = 0;
  ctx = saved;
  if (Trace()) {
    REXLOG_INFO("OnlineTrace: {} copy put in state {} variant {} exit {} (#{}), now {} variant {}", who, w.node,
                w.variant, w.exit, w.seq, NodeNumber(Read32(tree + kTreeState)),
                NodeNumber(Read32(tree + kTreeVariant)));
  }
}

}  // namespace

void OnStateEntered(uint32_t tree, uint32_t exit, bool with_slave) {
  if (!REXCVAR_GET(online_action_sync) || !net::Active() || g_forcing) return;
  if (g_inside_slave_change && !with_slave) return;  // the master change sends once, after
  if (IsSlave(tree)) return;  // follows its master: the master's state is sent
  const int local = net::LocalSlot();
  if (local < 0 || !more_players::InPlay()) return;
  uint8_t body = 0;
  const int owner = PlayerOfTree(tree, body);
  if (owner < 0) {
    // HOST: a synced enemy's state goes to the guests (online_enemy_states).
    if (!net::IsHost() || !net::world_sync::EnemyStates() || !guest_memory::Readable(tree, 0xA0)) return;
    const uint32_t net_id = net::world_sync::NetIdOfActor(Read32(tree + kTreeActor));
    const int enode = NodeNumber(Read32(tree + kTreeState));
    if (!net_id || enode <= 0) return;
    std::vector<uint8_t> msg(kEnemyStateSize, 0);
    msg[0] = kMsgEnemyState;
    for (int i = 0; i < 4; ++i) msg[1 + i] = uint8_t(net_id >> (8 * i));
    msg[17] = with_slave ? 1 : 0;
    PutState(msg, 9, tree, enode, exit);
    std::lock_guard lock(g_mutex);
    const uint32_t seq = ++g_enemy_seq;
    for (int i = 0; i < 4; ++i) msg[5 + i] = uint8_t(seq >> (8 * i));
    g_outgoing.push_back(std::move(msg));
    if (Trace()) REXLOG_INFO("OnlineTrace: enemy {} state {} sent (#{})", net_id, enode, seq);
    return;
  }
  if (owner != local) return;
  const int node = NodeNumber(Read32(tree + kTreeState));
  if (node <= 0) return;
  std::vector<uint8_t> msg(kTreeStateSize, 0);
  msg[0] = kMsgTreeState;
  msg[1] = uint8_t(local);
  msg[2] = body;
  msg[3] = with_slave ? 1 : 0;
  std::lock_guard lock(g_mutex);
  const uint32_t seq = ++g_local_seq;
  for (int i = 0; i < 4; ++i) msg[4 + i] = uint8_t(seq >> (8 * i));
  const int variant = std::max(0, NodeNumber(Read32(tree + kTreeVariant)));
  const int exit_number = std::max(0, NodeNumber(exit));
  msg[8] = uint8_t(node);
  msg[9] = uint8_t(node >> 8);
  msg[10] = uint8_t(variant);
  msg[11] = uint8_t(variant >> 8);
  msg[12] = uint8_t(exit_number);
  msg[13] = uint8_t(exit_number >> 8);
  const int variant2 = std::max(0, NodeNumber(Read32(tree + kTreeVariant2)));
  msg[14] = uint8_t(variant2);
  msg[15] = uint8_t(variant2 >> 8);
  g_outgoing.push_back(std::move(msg));
  if (Trace()) {
    REXLOG_INFO("OnlineTrace: P{} {} state {} variant {} exit {} sent (#{})", local + 1,
                body ? "titan" : "body", node, variant, exit_number, seq);
  }
}

bool BlockStateChange(uint32_t tree) {
  if (g_forcing) return false;
  uint8_t body = 0;
  const int p = PlayerOfTree(tree, body);
  if (p < 0) {
    // Enemy copies: their DECISIONS are off (the update below), but changes
    // that come from messages are let through: a player's JACK links the
    // titan's tree to the rider's by message (sub_8212EF40 type 1), and the
    // first version refused it here -> on the other PC the titan was never
    // jacked and session_sync's repair spawned a second one to mount (the
    // "fake titan", 2026-11-27 playtest). A local hit's stagger may show for a
    // moment; the host's next state follows.
    return false;
  }
  if (IsSlave(tree) || !Driving(p, body)) return false;
  if (Read32(tree + kTreeState) == 0) return false;  // the tree's own start-up: let it set up
  ++g_refused;
  if (Trace() && (g_refused & 63) == 1) {
    REXLOG_INFO("OnlineTrace: P{} copy's own state change refused ({} so far)", p + 1, g_refused);
  }
  return true;
}

void FreeEnemyCopy(uint32_t actor, int ms) {
  if (!actor) return;
  std::lock_guard lock(g_mutex);
  g_enemy_free_until[actor] = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

void AllowOwnDecisions(int p, int ms) {
  if (p < 0 || p >= kPlayers) return;
  std::lock_guard lock(g_mutex);
  // Extends, never shortens: a titan press's hold (1.2 s) must outlast the
  // shorter window its answer opens a frame later (titan_sync.h).
  g_free_until[p] = std::max(g_free_until[p], std::chrono::steady_clock::now() + std::chrono::milliseconds(ms));
}

void QueueMessage(std::vector<uint8_t> msg) {
  std::lock_guard lock(g_mutex);
  g_outgoing.push_back(std::move(msg));
}

std::vector<std::vector<uint8_t>> TakeOutgoing() {
  std::lock_guard lock(g_mutex);
  std::vector<std::vector<uint8_t>> out;
  out.swap(g_outgoing);
  return out;
}

void OnReceive(const uint8_t* data, size_t size) {
  if (size < kTreeStateSize) return;
  const int p = data[1];
  if (p >= kPlayers) return;
  uint32_t seq = 0;
  for (int i = 0; i < 4; ++i) seq |= uint32_t(data[4 + i]) << (8 * i);
  Wanted in;
  in.valid = true;
  in.seq = seq;
  in.node = data[8] | data[9] << 8;
  in.variant = data[10] | data[11] << 8;
  in.exit = data[12] | data[13] << 8;
  in.body = data[2];
  in.with_slave = data[3] == 1;
  in.variant2 = data[14] | data[15] << 8;
  in.received = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  Wanted& w = g_wanted[p][in.body & 1];
  if (w.valid && !SeqNewer(seq, w.seq)) return;  // resent / out of order
  w = in;
}

void OnReceiveEnemy(const uint8_t* data, size_t size) {
  if (size < kEnemyStateSize) return;
  uint32_t net_id = 0, seq = 0;
  for (int i = 0; i < 4; ++i) {
    net_id |= uint32_t(data[1 + i]) << (8 * i);
    seq |= uint32_t(data[5 + i]) << (8 * i);
  }
  Wanted in;
  in.valid = true;
  in.seq = seq;
  in.node = data[9] | data[10] << 8;
  in.variant = data[11] | data[12] << 8;
  in.exit = data[13] | data[14] << 8;
  in.variant2 = data[15] | data[16] << 8;
  in.with_slave = data[17] == 1;
  in.received = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  Wanted& w = g_enemy_wanted[net_id];
  if (w.valid && !SeqNewer(seq, w.seq)) return;
  w = in;
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_wanted = {};
  g_applied = {};
  g_enemy_wanted.clear();
  g_enemy_applied.clear();
  g_master_of.clear();
  g_enemy_free_until.clear();
  g_slave_since.clear();
  g_free_until = {};
  g_outgoing.clear();
}

}  // namespace net::action_sync

// Every fight tree's per-frame update. For a copy we drive: put it into the
// owner's newest state first (a re-entry too: newer sequence), then run the
// update with its decision switched off, so it never leaves that state by
// itself. Everything else runs as before.
extern "C" REX_FUNC(sub_820C1170) {
  using namespace net::action_sync;
  const uint32_t tree = ctx.r3.u32;
  tree_probe::BeforeUpdate(ctx, base, tree);
  uint8_t body = 0;
  const int p = PlayerOfTree(tree, body);
  // Links seen this frame (see RIDER LINKS).
  const auto now = std::chrono::steady_clock::now();
  if (guest_memory::Readable(tree, 0xA0)) {
    if (Read32(tree + kTreeMode) == 2 && Read32(tree + 0x98)) g_master_of[Read32(tree + 0x98)] = tree;
    if (IsSlave(tree)) {
      g_slave_since.emplace(tree, now);
    } else {
      g_slave_since.erase(tree);
    }
  }
  if (p >= 0 && Free(p)) {
    // Own decisions for a moment (an accepted pocket press): what the owner
    // did meanwhile is not replayed afterwards (the copy did it itself).
    std::lock_guard lock(g_mutex);
    g_applied[p][body & 1] = g_wanted[p][body & 1].seq;
  }
  // A copy's rider still linked to its titan here, while its OWNER has sent a
  // state for the rider since (the owner sends a rider's states only when it
  // is free) = the link is over there: unlink here the game's way, then drive.
  if (p >= 0 && body == 0 && IsSlave(tree) && Driving(p, 0)) {
    Wanted w;
    {
      std::lock_guard lock(g_mutex);
      w = g_wanted[p][0];
    }
    const auto since = g_slave_since.find(tree);
    if (w.valid && w.seq != g_applied[p][0] && since != g_slave_since.end() && w.received > since->second) {
      const auto m = g_master_of.find(tree);
      const PPCContext saved = ctx;
      if (m != g_master_of.end() && guest_memory::Readable(m->second, 0xA0) &&
          Read32(m->second + 0x98) == tree) {
        ctx.r3.u64 = m->second; ctx.r4.u64 = 0; ctx.r5.u64 = 0;
        __imp__sub_820C1210(ctx, base);
      }
      ctx.r3.u64 = tree; ctx.r4.u64 = 0; ctx.r5.u64 = 0;
      __imp__sub_820C1210(ctx, base);
      ctx = saved;
      g_slave_since.erase(tree);
      if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy's rider unlinked from its titan (owner's is free)", p + 1);
    }
  }
  bool drive = p >= 0 && !IsSlave(tree) && Driving(p, body);
  if (p >= 0 && !drive && body == 0 && p != net::LocalSlot() && net::Active() && Trace()) {
    // why a remote player's Crash copy is NOT driven (test trace, 2026-11-27)
    static std::array<int, kPlayers> last_reason{-1, -1, -1, -1};
    bool wanted = false;
    {
      std::lock_guard lock(g_mutex);
      wanted = g_wanted[p][0].valid;
    }
    const int reason = Free(p) ? 1 : IsSlave(tree) ? 2 : !wanted ? 3 : !net::PuppetOnFoot(p) ? 4 : 5;
    static std::array<bool, kPlayers> was_driven{};
    if (reason != last_reason[p] || was_driven[p]) {
      was_driven[p] = false;
      last_reason[p] = reason;
      static const char* names[] = {"", "own decisions window", "linked to a titan (slave)", "no state yet",
                                    "not on foot on both PCs / area / cutscene", "?"};
      REXLOG_INFO("OnlineTrace: P{} copy not driven: {}", p + 1, names[reason]);
    }
  }
  if (p >= 0 && body == 0 && p != net::LocalSlot() && net::Active() && Trace()) {
    static std::array<int, kPlayers> was{-1, -1, -1, -1};
    if (drive && was[p] != 1) REXLOG_INFO("OnlineTrace: P{} copy driven again", p + 1);
    was[p] = drive ? 1 : 0;
  }
  uint32_t enemy_id = 0;
  if (p < 0 && !IsSlave(tree) && EnemyDriving(tree, enemy_id)) {
    // An enemy copy: the host's newest state (any age: the copy always
    // follows), then the same no-decision update as a player's copy.
    drive = true;
    Wanted ew;
    uint32_t applied = 0;
    {
      std::lock_guard lock(g_mutex);
      ew = g_enemy_wanted[enemy_id];
      applied = g_enemy_applied[Read32(tree + kTreeActor)];
    }
    if (ew.valid && ew.seq != applied) {
      {
        std::lock_guard lock(g_mutex);
        g_enemy_applied[Read32(tree + kTreeActor)] = ew.seq;
      }
      ForceState(ctx, base, tree, ew, "enemy " + std::to_string(enemy_id));
    }
  }
  if (drive && p >= 0) {
    Wanted w;
    {
      std::lock_guard lock(g_mutex);
      w = g_wanted[p][body & 1];
    }
    if (w.valid && w.seq != g_applied[p][body & 1] &&
        std::chrono::steady_clock::now() - w.received > kTooOld) {
      g_applied[p][body & 1] = w.seq;  // meant for another moment: skipped
      if (Trace()) REXLOG_INFO("OnlineTrace: P{} old state {} (#{}) skipped", p + 1, w.node, w.seq);
    }
    if (w.valid && w.seq != g_applied[p][body & 1]) {
      g_applied[p][body & 1] = w.seq;
      // A player's mask round trip (enter 995 / mask 999 / leave 1037) is played
      // on the copy by the game's forced switch (net/mask_sync.h), not forced
      // here: the owner sends 995 while still on foot, so a copy got it before
      // its co-op code had any reason to be a mask.
      if (body == 0 && net::mask_sync::On() && net::mask_sync::IsMaskTransitionNode(w.node)) {
        if (Trace()) REXLOG_INFO("OnlineTrace: P{} mask state {} left to the mask switch", p + 1, w.node);
      } else {
        ForceState(ctx, base, tree, w, "P" + std::to_string(p + 1));
      }
    }
  }
  if (drive) {
    // The game's own "no decision this frame" flag + no picked exit.
    uint8_t* flags = Guest(tree + kTreeFlags);
    const uint8_t old = *flags;
    *flags = uint8_t(old | kNoDecision);
    Write32(tree + kTreeExit, uint32_t(-1));
    __imp__sub_820C1170(ctx, base);
    flags = Guest(tree + kTreeFlags);
    *flags = uint8_t((*flags & ~kNoDecision) | (old & kNoDecision));
    Write32(tree + kTreeExit, uint32_t(-1));
  } else {
    __imp__sub_820C1170(ctx, base);
  }
  tree_probe::AfterUpdate(tree);
}

// The variant picker (r3 tree; calls the tree's compiled test +0x2C, answers a
// node number or -1). While we put a copy into its owner's state, the answer is
// the OWNER's variant, not one from the copy's own situation.
extern "C" REX_FUNC(sub_820C0ED8) {
  using namespace net::action_sync;
  // ONLY the call made by the state change itself (sub_820C1578 at 0x820C1680):
  // the new state's enter actions can play high-level states, which ask the
  // picker about ANOTHER bank's nodes through the same tree (sub_8213E700);
  // answering those with our variant crashed (read of 0x1C, 2026-11-27).
  // Same for the master change sub_820C1BB8: its variant (0x820C1D88) and its
  // second variant (+0x50 = the slave's, 0x820C1DB4).
  if (g_forcing && ctx.r3.u32 == g_forcing_tree) {
    int answer = 0;
    bool ours = true;
    if (ctx.lr == 0x820C1684 || ctx.lr == 0x820C1D8C) {
      answer = g_forcing_variant;
    } else if (ctx.lr == 0x820C1DB8) {
      answer = g_forcing_variant2;
    } else {
      ours = false;
    }
    if (ours) {
      ctx.r3.u64 = answer > 0 ? uint32_t(answer) : uint32_t(-1);
      return;
    }
  }
  __imp__sub_820C0ED8(ctx, base);
}

// A MASTER tree's state change with its slave (a ridden titan + its rider).
// Owner: sent once, after (an inner sub_820C1578 call isn't sent on its own).
// Copy we drive: refused unless it's our own forcing.
extern "C" REX_FUNC(sub_820C1BB8) {
  using namespace net::action_sync;
  const uint32_t tree = ctx.r3.u32, exit = ctx.r5.u32;
  if (BlockStateChange(tree)) return;
  ++g_inside_slave_change;
  __imp__sub_820C1BB8(ctx, base);
  --g_inside_slave_change;
  OnStateEntered(tree, exit, /*with_slave=*/true);
}
