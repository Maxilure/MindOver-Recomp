// =============================================================================
// net/titan_sync.cpp -- online titans: the owner says WHAT HAPPENED, every
// other PC replays that one step through the game's own question (see the .h).
// =============================================================================
#include "titan_sync.h"

#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "../players/more_players.h"
#include "action_sync.h"
#include "session_sync.h"
#include "state_sync.h"
#include "world_sync.h"

REXCVAR_DEFINE_BOOL(online_titan_events, true, "CrashMoM",
                    "Online co-op: other players' titans (ridden, pocketed) follow the steps their own PC "
                    "reports (pocket, take out, swap, get off, mount), each made with the game's own code");

namespace net::titan_sync {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPlayers = 4;
constexpr uint32_t kCoOpState = 0x8259B11C;  // u32 per player (more_players.h): 2 = in the game
// The game object 0x825B0008: +24 / +28 players 1-2's titans (more_players
// TitanOfPlayer), +32 THE POCKET: the pocketed CActor*, one for everyone. The
// game's pocket is really a two-place stack at 0x825A5474 / 0x825A5480 (12 B
// each; pocket = sub_8216D1D0 pushes the old entry down and puts the ridden
// titan on top + sets +32; take out = sub_8216D548 takes the OLDER entry when
// there are two, which is how a swap brings the other titan back).
constexpr uint32_t kPocketSlot = 0x825B0028;
constexpr uint8_t kEventPocket = 37;   // ENEMY map 6: RB riding (pocket, or swap with one pocketed)
constexpr uint8_t kEventTakeOut = 14;  // PLAYER map 5: RB on foot (take the pocketed one out)

// What happened to the owner's titans between two steady states.
enum Kind : uint8_t {
  kSnapshot = 0,  // no step: the state itself (every change + every 2 s), for the late check
  kPocket = 1,    // ridden -> pocket, now on foot
  kTakeOut = 2,   // pocket -> ridden
  kSwap = 3,      // ridden <-> pocketed (RB with one pocketed)
  kGetOff = 4,    // ridden one left in the world, on foot
  kMount = 5,     // mounted titan <target> (a jack, on foot)
};
const char* KindName(uint8_t k) {
  switch (k) {
    case kPocket: return "pocket";
    case kTakeOut: return "take out";
    case kSwap: return "swap";
    case kGetOff: return "get off";
    case kMount: return "mount";
    default: return "state";
  }
}

// The owner's values must hold for this many game frames before they count:
// the original's own swap has ONE frame with nothing ridden between the two
// titans (2026-11-28 single-player watch: (A, B) -> (0, A) -> (B, A) on some
// presses, one atomic (A, B) -> (B, A) frame on others). A 1-frame state is
// never a step.
constexpr int kSteadyFrames = 2;
constexpr auto kSnapshotEvery = std::chrono::seconds(2);
// A step that hasn't shown its result by then is dropped (logged); the
// late check below then compares states.
constexpr auto kStepTimeout = std::chrono::milliseconds(3000);
// The late check: the copy's riding / pocket state differs from the owner's
// for this long with no step under way = one step to fix it.
constexpr auto kLateCheck = std::chrono::seconds(4);

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t IdOf(uint32_t actor) { return actor ? net::world_sync::LastNetIdOf(actor) : 0; }
uint32_t Le32(const uint8_t* d) { return d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24; }
void PutLe32(std::vector<uint8_t>& m, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) m[at + i] = uint8_t(v >> (8 * i));
}
bool Trace() { return net::DebugTraceOn(); }

// ---- owner side ------------------------------------------------------------
struct Owner {
  bool init = false;
  uint32_t cur_ridden = 0, cur_pocket = 0;  // this frame's values
  int same_frames = 0;                      // how long they held
  uint32_t ridden = 0, pocket = 0;          // the last STEADY state (what the others know)
  Clock::time_point last_sent{};
  uint32_t seq = 0;
};
Owner g_own;
std::mutex g_mutex;

void Send(int local, uint8_t kind, uint32_t target) {
  std::vector<uint8_t> msg(kTitanSize, 0);
  msg[0] = kMsgTitan;
  msg[1] = uint8_t(local);
  PutLe32(msg, 2, IdOf(g_own.ridden));
  PutLe32(msg, 6, IdOf(g_own.pocket));
  msg[10] = uint8_t((g_own.ridden ? 1 : 0) | (g_own.pocket ? 2 : 0));
  uint32_t seq;
  {
    std::lock_guard lock(g_mutex);
    seq = ++g_own.seq;
  }
  PutLe32(msg, 11, seq);
  msg[15] = kind;
  PutLe32(msg, 16, target);
  action_sync::QueueMessage(std::move(msg));  // in order with our fight-tree states
  g_own.last_sent = Clock::now();
  if (Trace() && kind != kSnapshot) {
    REXLOG_INFO("OnlineTrace: P{} titan step sent: {}{} (now rides {}, pocket {}) (#{})", local + 1,
                KindName(kind), kind == kMount ? " id " + std::to_string(target) : std::string(),
                g_own.ridden ? "yes" : "no", g_own.pocket ? "yes" : "no", seq);
  }
}

// Steady (R0, P0) -> steady (R1, P1): the steps that lead there. The pocket
// changing alone (another player's copy pocketed here: the slot is shared) is
// not this player's step.
void SendSteps(int local, uint32_t r0, uint32_t p0, uint32_t r1, uint32_t p1) {
  if (r0 && !r1) {
    Send(local, p1 == r0 ? kPocket : kGetOff, 0);
  } else if (!r0 && r1) {
    if (r1 == p0 && p1 != p0) Send(local, kTakeOut, 0);
    else Send(local, kMount, IdOf(r1));
  } else if (r0 && r1 && r0 != r1) {
    if (r1 == p0 && p1 == r0) {
      Send(local, kSwap, 0);
    } else if (p1 == r0) {  // jacked another while riding: the old one pocketed
      Send(local, kPocket, 0);
      Send(local, kMount, IdOf(r1));
    } else {
      Send(local, kGetOff, 0);
      Send(local, kMount, IdOf(r1));
    }
  }
}

void OwnerTick(int local) {
  const uint32_t r = more_players::TitanOfPlayer(local);
  const uint32_t pk = Read32(kPocketSlot);
  if (!g_own.init) {
    g_own = Owner{true, r, pk, kSteadyFrames, r, pk, {}, g_own.seq};
    Send(local, kSnapshot, 0);
    return;
  }
  if (r == g_own.cur_ridden && pk == g_own.cur_pocket) {
    ++g_own.same_frames;
  } else {
    g_own.cur_ridden = r;
    g_own.cur_pocket = pk;
    g_own.same_frames = 1;
  }
  if (g_own.same_frames >= kSteadyFrames && (r != g_own.ridden || pk != g_own.pocket)) {
    const uint32_t r0 = g_own.ridden, p0 = g_own.pocket;
    g_own.ridden = r;
    g_own.pocket = pk;
    SendSteps(local, r0, p0, r, pk);
    Send(local, kSnapshot, 0);
    return;
  }
  if (Clock::now() - g_own.last_sent >= kSnapshotEvery) Send(local, kSnapshot, 0);
}

// ---- the other PCs ---------------------------------------------------------
struct Step {
  uint8_t kind = kSnapshot;
  uint32_t target = 0;
};
struct Remote {
  bool valid = false;
  uint32_t seq = 0;
  // the owner's newest state
  bool rides = false, has_pocket = false;
  uint32_t ridden_id = 0;
  std::deque<Step> steps;  // steps not started yet, in order
  // the step under way
  bool active = false;
  Step step;
  Clock::time_point started{};
  uint32_t titan_before = 0;
  int tries = 0;
  Clock::time_point last_step_at{};  // a step arrived / ended: the late check waits
  Clock::time_point mismatch_since{};
};
std::array<Remote, kPlayers> g_remote{};

// The step's result is there?
bool StepDone(const Remote& r, uint32_t titan) {
  switch (r.step.kind) {
    case kPocket:
    case kGetOff: return titan == 0;
    case kSwap: return titan != 0 && titan != r.titan_before;
    case kTakeOut:
    case kMount: return titan != 0;
    default: return true;
  }
}

// Start one step on player p's copy. False = nothing to do / can't (logged).
bool StartStep(PPCContext& ctx, uint8_t* base, int p, Remote& r) {
  const uint32_t titan = more_players::TitanOfPlayer(p);
  const uint32_t pocket = Read32(kPocketSlot);
  const char* why = nullptr;
  switch (r.step.kind) {
    case kPocket:
      if (!titan) { why = "already on foot"; break; }
      if (pocket) { why = "our pocket is full (RB would swap)"; break; }
      net::GrantRemotePress(p, kEventPocket);
      break;
    case kSwap:
      if (!titan) { why = "on foot here"; break; }
      if (!pocket) { why = "our pocket is empty (RB would only pocket)"; break; }
      net::GrantRemotePress(p, kEventPocket);
      break;
    case kTakeOut:
      if (titan) { why = "riding here"; break; }
      if (!pocket) { why = "our pocket is empty"; break; }
      net::GrantRemotePress(p, kEventTakeOut);
      break;
    case kGetOff:
      if (!titan) { why = "already on foot"; break; }
      // The titan's own dismount question answered yes (lost_controller's
      // CheckInputEvent hook). B riding can also mean "jack the next titan":
      // never for a copy (CheckOpportunity answers nothing for other PCs'
      // players, below), so this is a get off and nothing else.
      session_sync::ForceDismount(p, titan);
      break;
    case kMount: {
      if (titan) { why = "riding here"; break; }
      const uint32_t copy = r.step.target ? net::world_sync::ActorOfNetId(r.step.target) : 0;
      if (!copy) { why = "no copy of that titan here"; break; }
      if (copy == pocket) { why = "that titan is in our pocket"; break; }
      for (int q = 0; q < kPlayers; ++q) {
        if (more_players::TitanOfPlayer(q) == copy) why = "another player rides it here";
      }
      if (why) break;
      session_sync::MountPlayerOnTitan(ctx, base, p, copy);
      break;
    }
    default: return false;
  }
  if (why) {
    REXLOG_INFO("Online: P{}'s titan step '{}' skipped here: {}", p + 1, KindName(r.step.kind), why);
    return false;
  }
  r.active = true;
  r.started = Clock::now();
  r.titan_before = titan;
  r.tries = 1;
  if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy titan step: {}", p + 1, KindName(r.step.kind));
  return true;
}

void Follow(PPCContext& ctx, uint8_t* base, int p, Remote& r) {
  const auto now = Clock::now();
  const uint32_t titan = more_players::TitanOfPlayer(p);
  if (r.active) {
    if (StepDone(r, titan)) {
      r.active = false;
      r.last_step_at = now;
      if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy titan step '{}' done", p + 1, KindName(r.step.kind));
    } else if (r.step.kind == kGetOff && r.tries == 1 && now - r.started > std::chrono::milliseconds(1200)) {
      r.tries = 2;  // the dismount question may have come mid-animation: ask once more
      session_sync::ForceDismount(p, titan);
    } else if (now - r.started > kStepTimeout) {
      r.active = false;
      r.last_step_at = now;
      REXLOG_WARN("Online: P{}'s titan step '{}' didn't happen here in {} s", p + 1, KindName(r.step.kind),
                  std::chrono::duration_cast<std::chrono::seconds>(kStepTimeout).count());
    }
    return;
  }
  while (!r.steps.empty()) {
    r.step = r.steps.front();
    r.steps.pop_front();
    r.last_step_at = now;
    if (StartStep(ctx, base, p, r)) return;
  }
  // THE LATE CHECK: riding / not riding (and pocket full / empty) compared,
  // never titan ids (a titan ridden since a save load never had one).
  const bool rides_here = titan != 0;
  if (rides_here == r.rides) {
    r.mismatch_since = {};
    return;
  }
  if (r.mismatch_since == Clock::time_point{}) r.mismatch_since = now;
  if (now - r.mismatch_since < kLateCheck || now - r.last_step_at < kLateCheck) return;
  r.mismatch_since = now;  // one try per kLateCheck
  const bool pocket_here = Read32(kPocketSlot) != 0;
  Step fix;
  if (r.rides) {
    if (pocket_here && !r.has_pocket) fix = {kTakeOut, 0};
    else if (r.ridden_id) fix = {kMount, r.ridden_id};
  } else {
    fix = {(r.has_pocket && !pocket_here) ? kPocket : kGetOff, 0};
  }
  if (fix.kind == kSnapshot) return;
  REXLOG_WARN("Online: P{}'s copy {} a titan here but not on their PC for {} s: '{}'", p + 1,
              rides_here ? "rides" : "doesn't ride", std::chrono::duration_cast<std::chrono::seconds>(kLateCheck).count(),
              KindName(fix.kind));
  r.step = fix;
  StartStep(ctx, base, p, r);
}

}  // namespace

bool On() { return REXCVAR_GET(online_titan_events) && net::Active(); }

void Tick(PPCContext& ctx, uint8_t* base) {
  if (!On() || !more_players::InPlay() || more_players::Loading()) return;
  const int local = net::LocalSlot();
  if (local >= 0 && local < kPlayers && Read32(kCoOpState + 4 * uint32_t(local)) == 2) OwnerTick(local);
  if (session_sync::InTransitionOrCutsceneGrace()) return;
  for (int p = 0; p < kPlayers; ++p) {
    if (p == local || Read32(kCoOpState + 4 * uint32_t(p)) != 2) continue;
    std::lock_guard lock(g_mutex);
    Remote& r = g_remote[p];
    if (!r.valid) continue;
    Follow(ctx, base, p, r);
  }
}

void OnReceive(const uint8_t* data, size_t size) {
  if (size < kTitanSize) return;
  const int p = data[1];
  if (p >= kPlayers) return;
  const uint32_t seq = Le32(data + 11);
  std::lock_guard lock(g_mutex);
  Remote& r = g_remote[p];
  if (r.valid && int32_t(seq - r.seq) <= 0) return;  // old / repeated
  r.valid = true;
  r.seq = seq;
  r.rides = data[10] & 1;
  r.has_pocket = (data[10] & 2) != 0;
  r.ridden_id = Le32(data + 2);
  const uint8_t kind = data[15];
  if (kind != kSnapshot && kind <= kMount) {
    r.steps.push_back({kind, Le32(data + 16)});
    r.last_step_at = Clock::now();
    if (Trace()) REXLOG_INFO("OnlineTrace: P{} titan step heard: {} (#{})", p + 1, KindName(kind), seq);
  }
}

bool Busy(int p) {
  if (p < 0 || p >= kPlayers || !On()) return false;
  std::lock_guard lock(g_mutex);
  const Remote& r = g_remote[p];
  return r.valid && (r.active || !r.steps.empty() || Clock::now() - r.last_step_at < kLateCheck);
}

void Forget(int p) {
  if (p < 0 || p >= kPlayers) return;
  std::lock_guard lock(g_mutex);
  g_remote[p] = {};
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_remote = {};
  g_own.init = false;
}

bool IsOtherPcsPlayer(uint32_t actor) {
  if (!On() || !actor) return false;
  const int local = net::LocalSlot();
  for (int q = 0; q < kPlayers; ++q) {
    if (q != local && more_players::CharacterOfPlayer(q) == actor) return true;
  }
  return false;
}

}  // namespace net::titan_sync

// CheckOpportunity (sub_82157058; r3 = the asking Crash's COpportunityBehaviour,
// its actor at +0x1C; r4 the name's hash, f1 the range): "is there a titan I
// may jack within range?" -- asked by Crash's own B on foot and, through the
// ridden titan's CheckOpportunitySlave, by B while riding (prompt_icons.cpp
// has the full read). ANOTHER PC'S PLAYER NEVER PICKS A TITAN HERE: their own
// PC jacks and reports "mount titan N" (titan_sync step kMount). Without this,
// the copy's forced get-off (the titan's B question answered yes) turned into
// a JACK of whatever knocked-out titan stood nearby (2026-11-28 two-PC test:
// the guest's copy of player 1 jacked a spawned Ratcicle instead of getting
// off, the repair fought it and the guest crashed).
// BUT an offer made FOR that player alone stays theirs: a titan coming out of
// the pocket (take out, the second half of a swap) offers itself only to its
// taker (COpportunity +0x10 = that Crash) and the swap script jacks it through
// this same check. Hiding everything left the copy on foot after a swap (same
// test). So for other PCs' players only the "anyone" offers (+0x10 = 0: a
// knocked-out enemy) are hidden, for this one call: their +0x10 is set to a
// value no actor has, then put back. Table: 0x825A5418 = a binary tree (root
// word, sentinel = table + 4; node +0 left, +4 right, +0x14 a list: entry +0
// next, +8 the COpportunity).
namespace net::titan_sync {
namespace {
constexpr uint32_t kOpportunityTable = 0x825A5418;
constexpr uint32_t kNoOne = 0xFFFFFFF0u;
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
// Every open "anyone" offer -> hidden (their addresses into `out`).
void HideAnyoneOffers(uint32_t node, int depth, std::vector<uint32_t>& out) {
  const uint32_t sentinel = kOpportunityTable + 4;
  if (!node || node == sentinel || depth > 32 || !guest_memory::Readable(node, 0x18)) return;
  int n = 0;
  for (uint32_t e = Read32(node + 0x14); e && n < 64 && guest_memory::Readable(e, 12); e = Read32(e), ++n) {
    const uint32_t opp = Read32(e + 8);
    if (opp && guest_memory::Readable(opp + 0x10, 4) && Read32(opp + 0x10) == 0) {
      Write32(opp + 0x10, kNoOne);
      out.push_back(opp);
    }
  }
  HideAnyoneOffers(Read32(node), depth + 1, out);
  HideAnyoneOffers(Read32(node + 4), depth + 1, out);
}
}  // namespace
}  // namespace net::titan_sync

extern "C" REX_FUNC(__imp__sub_82157058);
extern "C" REX_FUNC(sub_82157058) {
  using namespace net::titan_sync;
  const uint32_t beh = ctx.r3.u32;
  const uint32_t actor = On() && beh && guest_memory::Readable(beh + 0x1C, 4) ? Read32(beh + 0x1C) : 0;
  if (!IsOtherPcsPlayer(actor)) {
    __imp__sub_82157058(ctx, base);
    return;
  }
  std::vector<uint32_t> hidden;
  HideAnyoneOffers(Read32(kOpportunityTable), 0, hidden);
  __imp__sub_82157058(ctx, base);
  for (const uint32_t opp : hidden) Write32(opp + 0x10, 0);
}
