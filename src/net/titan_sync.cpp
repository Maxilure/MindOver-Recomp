// =============================================================================
// net/titan_sync.cpp -- online titans by outcome, not by presses (see the .h).
// =============================================================================
#include "titan_sync.h"

#include <array>
#include <chrono>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "../players/more_players.h"
#include "action_sync.h"
#include "session_sync.h"
#include "state_sync.h"
#include "world_sync.h"

REXCVAR_DEFINE_BOOL(online_titan_events, true, "CrashMoM",
                    "Online co-op: other players' titans (ridden, pocketed) follow what their own PC says "
                    "happened, through the game's own mount / pocket / take-out / dismount, not their presses");

namespace net::titan_sync {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPlayers = 4;
constexpr uint32_t kCoOpState = 0x8259B11C;  // u32 per player (more_players.h)
constexpr uint32_t kPocketSlot = 0x825B0028; // game object 0x825B0008 +32: the pocketed CActor* (one for all)
constexpr uint8_t kEventPocket = 37;         // ENEMY map 6: RB riding (pocket / swap)
constexpr uint8_t kEventTakeOut = 14;        // PLAYER map 5: RB on foot (take the pocketed one out)
constexpr auto kStepEvery = std::chrono::milliseconds(800);  // one step, then let it play out
// THE PRESS STILL TRAVELS, AS A "HOLD" ONLY (2026-11-27 two-PC tests): the
// owner's titan fight-tree states for an RB / B (e.g. Ratcicle / Spike 2206 exit
// 1511, then 835 exit 2205 for a swap) reach a copy ~60 ms BEFORE this
// announcement (the pocket slot changes only once the state's script ran).
// Forced on the copy they ran the swap once, our answer then swapped back; and
// while a pocket plays out the game keeps rider and titan LINKED: a copy
// driven by forced states got unlinked early -> a titan message read freed
// memory (guest crash, read of 0x34 in sub_820CD6B8 under sub_8212EE90). So the
// owner's accepted press (kMsgPocket, state_sync) now only opens the copy's own
// decisions (no forced states, nothing pressed), and THIS message picks the
// answer its decision hears.
constexpr auto kGiveUp = std::chrono::seconds(4);            // then session_sync's repair decides
constexpr auto kRepeat = std::chrono::seconds(2);

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

struct Wanted {             // the newest titan state an owner announced
  bool valid = false;
  uint32_t seq = 0;
  bool rides = false;
  uint32_t ridden = 0;      // host net id (0 = not synced / none)
  bool pocket = false;
  uint32_t pocketed = 0;
  Clock::time_point since;  // when this state arrived
  Clock::time_point next_step{};
  bool done = false;        // the copy got there once (not enforced after)
  bool slow_noted = false;
  // An RB answer (pocket / swap / take out) was given for this announcement:
  // never again for it. A swap's animation outlasts any step interval, and a
  // second answer swapped back (2026-11-27 two-PC test: flip-flops every 0.8 s).
  bool pressed = false;
};

std::mutex g_mutex;
std::array<Wanted, kPlayers> g_wanted{};
// Owner side: what we last announced.
bool g_sent_valid = false;
uint32_t g_sent_ridden = 0, g_sent_pocketed = 0;
bool g_sent_rides = false, g_sent_pocket = false;
Clock::time_point g_sent_at{};
uint32_t g_seq = 0;

bool Trace() { return net::DebugTraceOn(); }

void SendOurs(int local) {
  const uint32_t titan = more_players::TitanOfPlayer(local);
  const uint32_t pocket = Read32(kPocketSlot);
  const bool rides = titan != 0, has_pocket = pocket != 0;
  const uint32_t ridden = IdOf(titan), pocketed = IdOf(pocket);
  const auto now = Clock::now();
  const bool changed = !g_sent_valid || rides != g_sent_rides || ridden != g_sent_ridden ||
                       has_pocket != g_sent_pocket || pocketed != g_sent_pocketed;
  if (!changed && now - g_sent_at < kRepeat) return;
  g_sent_valid = true;
  g_sent_rides = rides;
  g_sent_ridden = ridden;
  g_sent_pocket = has_pocket;
  g_sent_pocketed = pocketed;
  g_sent_at = now;
  std::vector<uint8_t> msg(kTitanSize, 0);
  msg[0] = kMsgTitan;
  msg[1] = uint8_t(local);
  PutLe32(msg, 2, ridden);
  PutLe32(msg, 6, pocketed);
  msg[10] = uint8_t((rides ? 1 : 0) | (has_pocket ? 2 : 0));
  uint32_t seq;
  {
    std::lock_guard lock(g_mutex);
    seq = ++g_seq;
  }
  PutLe32(msg, 11, seq);
  action_sync::QueueMessage(std::move(msg));  // in order with our fight-tree states
  if (Trace() && changed) {
    REXLOG_INFO("OnlineTrace: P{} titans announced: rides {} (id {}), pocket {} (id {}) (#{})", local + 1,
                rides ? "yes" : "no", ridden, has_pocket ? "yes" : "no", pocketed, seq);
  }
}

// One step of player p's copy toward the owner's titans (the .h's four steps).
void Follow(PPCContext& ctx, uint8_t* base, int p, Wanted& w) {
  if (w.done) return;
  const auto now = Clock::now();
  const uint32_t titan = more_players::TitanOfPlayer(p);
  const uint32_t titan_id = IdOf(titan);
  const bool matched = w.rides ? (titan && (titan_id == w.ridden || w.ridden == 0)) : titan == 0;
  if (matched) {
    w.done = true;
    if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy's titans match their PC (#{})", p + 1, w.seq);
    return;
  }
  if (now - w.since > kGiveUp) {
    if (!w.slow_noted) {
      w.slow_noted = true;
      REXLOG_WARN("Online: P{}'s copy doesn't ride what their PC says (here titan id {}, there {} {}) after {} s: "
                  "left to the titan repair", p + 1, titan_id, w.rides ? "rides" : "rides nothing", w.ridden,
                  std::chrono::duration_cast<std::chrono::seconds>(kGiveUp).count());
    }
    return;
  }
  if (now < w.next_step) return;  // the last step is still playing out
  w.next_step = now + kStepEvery;
  if (titan) {
    // Riding something the owner doesn't: pocketed there -> RB here, the
    // game's own RB riding: our pocket empty = it pockets the titan; our pocket
    // holding the other one = it SWAPS (pockets this one, takes that one out),
    // which is what the owner's RB did (a swap arrives as "rides nothing,
    // pocket X", then "rides Y" one frame later, or both at once; 2026-11-27
    // two-PC test: refusing a full pocket left the copy on the old titan).
    if (w.pocket && titan_id && w.pocketed == titan_id) {
      if (w.pressed) return;  // its own RB is playing out (or failed: the repair after kGiveUp)
      w.pressed = true;
      net::GrantRemotePress(p, kEventPocket);
      if (Trace()) {
        REXLOG_INFO("OnlineTrace: P{} copy {} its titan (id {})", p + 1,
                    Read32(kPocketSlot) ? "swaps (RB with a full pocket)" : "pockets", titan_id);
      }
    } else {
      session_sync::ForceDismount(p, titan);
      if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy gets off its titan (id {})", p + 1, titan_id);
    }
    return;
  }
  // On foot here, riding there.
  if (!w.ridden) {  // not a synced titan (unknown id): nothing to mount by
    w.done = true;
    return;
  }
  const uint32_t pocket = Read32(kPocketSlot);
  const uint32_t copy_of_ridden = net::world_sync::ActorOfNetId(w.ridden);
  if (pocket && (IdOf(pocket) == w.ridden || pocket == copy_of_ridden)) {
    if (w.pressed) return;
    w.pressed = true;
    net::GrantRemotePress(p, kEventTakeOut);
    if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy takes its titan out of the pocket (id {})", p + 1, w.ridden);
    return;
  }
  const uint32_t copy = copy_of_ridden;
  if (!copy) {
    if (Trace()) REXLOG_INFO("OnlineTrace: P{} rides titan id {} there, no copy of it here yet", p + 1, w.ridden);
    return;
  }
  for (int q = 0; q < kPlayers; ++q) {
    if (q != p && more_players::TitanOfPlayer(q) == copy) {
      REXLOG_WARN("Online: P{} rides titan id {} there, P{} rides it here: left to the repair", p + 1, w.ridden,
                  q + 1);
      w.next_step = now + kGiveUp;
      return;
    }
  }
  session_sync::MountPlayerOnTitan(ctx, base, p, copy);
  REXLOG_INFO("Online: P{} mounted on their titan's copy here (id {}, {:08X}), as their PC says", p + 1, w.ridden,
              copy);
}

}  // namespace

bool On() { return REXCVAR_GET(online_titan_events) && net::Active(); }

void Tick(PPCContext& ctx, uint8_t* base) {
  if (!On() || !more_players::InPlay() || more_players::Loading()) return;
  const int local = net::LocalSlot();
  if (local >= 0 && local < kPlayers && Read32(kCoOpState + 4 * uint32_t(local)) == 2) SendOurs(local);
  if (session_sync::InTransitionOrCutsceneGrace()) return;
  std::array<Wanted, kPlayers> wanted;
  {
    std::lock_guard lock(g_mutex);
    wanted = g_wanted;
  }
  for (int p = 0; p < kPlayers; ++p) {
    if (p == local || !wanted[p].valid || Read32(kCoOpState + 4 * uint32_t(p)) != 2) continue;
    Follow(ctx, base, p, wanted[p]);
    std::lock_guard lock(g_mutex);
    Wanted& w = g_wanted[p];
    if (w.seq == wanted[p].seq) {
      w.done = wanted[p].done;
      w.next_step = wanted[p].next_step;
      w.slow_noted = wanted[p].slow_noted;
      w.pressed = wanted[p].pressed;
    }
  }
}

void OnReceive(const uint8_t* data, size_t size) {
  if (size < kTitanSize) return;
  const int p = data[1];
  if (p >= kPlayers) return;
  const uint32_t seq = Le32(data + 11);
  const bool rides = data[10] & 1, pocket = (data[10] & 2) != 0;
  const uint32_t ridden = Le32(data + 2), pocketed = Le32(data + 6);
  std::lock_guard lock(g_mutex);
  Wanted& w = g_wanted[p];
  if (w.valid && int32_t(seq - w.seq) <= 0) return;
  const bool changed = !w.valid || w.rides != rides || w.ridden != ridden || w.pocket != pocket ||
                       w.pocketed != pocketed;
  w.valid = true;
  w.seq = seq;
  if (!changed) return;  // a repeat
  // A SWAP comes as two announcements a frame apart ("rides nothing, pocket X",
  // then "rides Y, pocket X"): the RB answered for the first IS the second's
  // too (the game's swap does both). Same pocket, now riding = kept.
  const bool same_swap = w.valid && w.pressed && !w.rides && rides && w.pocket && pocket &&
                         w.pocketed == pocketed && Clock::now() - w.since < std::chrono::milliseconds(500);
  w.rides = rides;
  w.ridden = ridden;
  w.pocket = pocket;
  w.pocketed = pocketed;
  w.since = Clock::now();
  w.next_step = {};
  w.done = false;
  w.slow_noted = false;
  w.pressed = same_swap;
  if (Trace()) {
    REXLOG_INFO("OnlineTrace: P{} says titans: rides {} (id {}), pocket {} (id {}) (#{})", p + 1,
                rides ? "yes" : "no", ridden, pocket ? "yes" : "no", pocketed, seq);
  }
}

bool Busy(int p) {
  if (p < 0 || p >= kPlayers || !On()) return false;
  std::lock_guard lock(g_mutex);
  const Wanted& w = g_wanted[p];
  return w.valid && !w.done && Clock::now() - w.since < kGiveUp;
}

void Forget(int p) {
  if (p < 0 || p >= kPlayers) return;
  std::lock_guard lock(g_mutex);
  g_wanted[p] = {};
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_wanted = {};
  g_sent_valid = false;
  g_sent_at = {};
}

}  // namespace net::titan_sync
