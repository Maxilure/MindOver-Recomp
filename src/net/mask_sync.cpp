// =============================================================================
// net/mask_sync.cpp -- online masks by announcement, not by B (see the .h).
//
// Tables read here (more_players.h, findings/26 s.2):
//   0x8259B11C u32[4] co-op state (ECoOpPlayerState: 0 not joined, 1 MASK,
//              2 IN_GAME, 3 ENTERING_MASK)
//   0x824F3C38 u32[4] co-op sub-state (4 forced out of the mask, 5 BEING_FORCED_
//              INTO_MASK, 6 dropped out, 7 DYING, 9 nothing going on)
// =============================================================================
#include "mask_sync.h"

#include <array>
#include <chrono>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../players/more_players.h"
#include "action_sync.h"
#include "session_sync.h"
#include "state_sync.h"

REXCVAR_DEFINE_BOOL(online_mask_events, true, "CrashMoM",
                    "Online co-op: other players turn into masks and back when their own PC says so "
                    "(the game's forced mask switches), not from their replayed B button");

namespace net::mask_sync {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPlayers = 4;
constexpr uint32_t kCoOpState = 0x8259B11C, kSubState = 0x824F3C38;
constexpr uint32_t kSubForcedOutOfMask = 4, kSubForcedIntoMask = 5, kSubDroppedOut = 6, kSubDying = 7;
constexpr uint8_t kNoHost = 0xFF;
// Crash.bfig states of a mask round trip (traced 2026-11-27, both PCs):
// on foot -> 995 (exit 994, entering) -> 999 (exit 998, the mask);
// a mask -> 1037 (exit 1036, leaving) -> 28 -> 587 -> 316 (ordinary ground states).
// Joining as a mask enters 999 too (exit 1054).
constexpr int kNodeEnterMask = 995, kNodeMask = 999, kNodeLeaveMask = 1037;
// A copy that hasn't followed after this long: said once in the log (the
// forced switch keeps being asked; the next announcement restarts it).
constexpr auto kSlow = std::chrono::seconds(4);

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t CoOp(int p) { return Read32(kCoOpState + 4 * uint32_t(p)); }
uint32_t Sub(int p) { return Read32(kSubState + 4 * uint32_t(p)); }

struct Wanted {           // the newest co-op state an owner announced
  bool valid = false;
  uint32_t seq = 0;
  uint8_t state = 0;      // 1 mask, 2 on foot, 3 entering a mask
  uint8_t host = kNoHost;
  Clock::time_point since;  // when it arrived (for the "slow" note)
  bool slow_noted = false;
  // The copy got there once: this announcement is DONE. Not enforced after
  // that: the game may move the copy itself (a disconnected carrier's rider
  // stepping in, lost_controller ONLINE), and the owner's PC, doing the same,
  // announces the result. Enforcing forever fought those.
  bool done = false;
};

std::mutex g_mutex;
std::array<Wanted, kPlayers> g_wanted{};
// Owner side: what we last announced (state + host), and the counter.
uint32_t g_sent_state = ~0u;
int g_sent_host = -2;
uint32_t g_seq = 0;
// Said again every kRepeat even without a change: a PC that connected later
// (or reset) learns where we stand; repeats are harmless (newest wins).
Clock::time_point g_sent_at{};
constexpr auto kRepeat = std::chrono::seconds(2);

bool Trace() { return net::DebugTraceOn(); }
bool IsMaskState(uint8_t s) { return s == 1 || s == 3; }

// OWNER: our player's co-op state (and host) changed -> announce it.
void SendOurChanges(int local) {
  const uint32_t state = CoOp(local);
  const int host = state == 1 ? more_players::MaskHostOf(local) : -1;
  const auto now = Clock::now();
  if (state == g_sent_state && host == g_sent_host && now - g_sent_at < kRepeat) return;
  const bool changed = state != g_sent_state || host != g_sent_host;
  g_sent_at = now;
  g_sent_state = state;
  g_sent_host = host;
  if (state == 0) return;  // not joined: the join path (state_sync START) handles that
  std::vector<uint8_t> msg(kMaskSize, 0);
  msg[0] = kMsgMask;
  msg[1] = uint8_t(local);
  msg[2] = uint8_t(state);
  msg[3] = host >= 0 ? uint8_t(host) : kNoHost;
  uint32_t seq;
  {
    std::lock_guard lock(g_mutex);
    seq = ++g_seq;
  }
  for (int i = 0; i < 4; ++i) msg[4 + i] = uint8_t(seq >> (8 * i));
  action_sync::QueueMessage(std::move(msg));  // same queue as our fight-tree states: order kept
  if (Trace() && changed) {
    REXLOG_INFO("OnlineTrace: P{} co-op state {} (host P{}) announced (#{})", local + 1, state,
                host >= 0 ? host + 1 : 0, seq);
  }
}

// COPY: make player p's copy follow its owner's announced state.
void Follow(PPCContext& ctx, uint8_t* base, int p, Wanted& w) {
  if (w.done) return;
  const uint32_t here = CoOp(p), sub = Sub(p);
  if (here == 0) {  // not joined here yet: the join comes first (state_sync)
    w.since = Clock::now();  // (the "slow" clock starts once the copy can follow)
    return;
  }
  const bool want_mask = IsMaskState(w.state);
  bool matched = false;
  if (want_mask) {
    if (here == 2) {
      // The game's forced switch: the copy's tree takes CoOpBecomeMask itself.
      // Not over a drop out / a death already under way (6 / 7).
      if (sub != kSubForcedIntoMask && sub != kSubDroppedOut && sub != kSubDying) {
        Write32(kSubState + 4 * uint32_t(p), kSubForcedIntoMask);
        if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy forced into a mask (owner's #{})", p + 1, w.seq);
      }
    } else if (here == 1 && w.host != kNoHost) {
      matched = true;
      // Same mask, other host (3-4 players: the game picks the nearest Crash on
      // foot here, the owner's picked theirs): moved onto the owner's host.
      const int host_here = more_players::MaskHostOf(p);
      if (host_here >= 0 && host_here != int(w.host) && CoOp(int(w.host)) == 2) {
        const bool moved = more_players::MoveMaskTo(ctx, base, p, int(w.host));
        REXLOG_INFO("Online: P{}'s mask rode P{} here, P{} on their PC: {}", p + 1, host_here + 1, w.host + 1,
                    moved ? "moved" : "NOT moved");
      }
    } else {
      matched = here == 1 || here == 3;
    }
  } else if (w.state == 2) {
    if (here == 1) {
      if (sub != kSubForcedOutOfMask) {
        Write32(kSubState + 4 * uint32_t(p), kSubForcedOutOfMask);
        if (Trace()) REXLOG_INFO("OnlineTrace: P{} copy forced out of its mask (owner's #{})", p + 1, w.seq);
      }
    } else {
      matched = here == 2;  // (3: still entering; it leaves once it is a mask)
    }
  }
  if (matched) {
    w.slow_noted = false;
    w.done = true;
  } else if (!w.slow_noted && Clock::now() - w.since > kSlow) {
    w.slow_noted = true;
    REXLOG_WARN("Online: P{}'s copy still isn't {} (state {}, sub-state {}) {} s after their PC said so", p + 1,
                want_mask ? "a mask" : "on foot", here, Read32(kSubState + 4 * uint32_t(p)),
                std::chrono::duration_cast<std::chrono::seconds>(kSlow).count());
  }
}

}  // namespace

bool On() { return REXCVAR_GET(online_mask_events) && net::Active(); }

// While copies can't follow (loading, cutscene), the "slow" clocks wait too:
// an announcement that arrived under a loading screen isn't late.
static void HoldClocks() {
  std::lock_guard lock(g_mutex);
  for (Wanted& w : g_wanted) w.since = Clock::now();
}

void Tick(PPCContext& ctx, uint8_t* base) {
  if (!On()) return;
  if (!more_players::InPlay() || more_players::Loading()) {
    HoldClocks();
    return;
  }
  const int local = net::LocalSlot();
  if (local >= 0 && local < kPlayers) SendOurChanges(local);
  // Copies follow outside cutscenes / area changes only (the game moves
  // everyone itself then; the newest announcement waits).
  if (session_sync::InTransitionOrCutsceneGrace()) {
    HoldClocks();
    return;
  }
  std::array<Wanted, kPlayers> wanted;
  {
    std::lock_guard lock(g_mutex);
    wanted = g_wanted;
  }
  for (int p = 0; p < kPlayers; ++p) {
    if (p == local || !wanted[p].valid) continue;
    Follow(ctx, base, p, wanted[p]);
    std::lock_guard lock(g_mutex);
    if (g_wanted[p].seq == wanted[p].seq) {
      g_wanted[p].slow_noted = wanted[p].slow_noted;
      g_wanted[p].since = wanted[p].since;
      g_wanted[p].done = wanted[p].done;
    }
  }
}

void OnReceive(const uint8_t* data, size_t size) {
  if (size < kMaskSize) return;
  const int p = data[1];
  if (p >= kPlayers) return;
  uint32_t seq = 0;
  for (int i = 0; i < 4; ++i) seq |= uint32_t(data[4 + i]) << (8 * i);
  std::lock_guard lock(g_mutex);
  Wanted& w = g_wanted[p];
  if (w.valid && int32_t(seq - w.seq) <= 0) return;  // older than what we have
  const uint8_t host = data[3] < kPlayers ? data[3] : kNoHost;
  const bool changed = !w.valid || w.state != data[2] || w.host != host;
  w.valid = true;
  w.seq = seq;
  if (!changed) return;  // a repeat: keeps `since` (the "slow" note counts from the change)
  w.state = data[2];
  w.host = host;
  w.since = Clock::now();
  w.slow_noted = false;
  w.done = false;
  if (Trace()) {
    REXLOG_INFO("OnlineTrace: P{} says co-op state {} (host {}) (#{})", p + 1, w.state,
                w.host != kNoHost ? int(w.host) + 1 : 0, seq);
  }
}

bool WantsMask(int p) {
  if (p < 0 || p >= kPlayers || !On()) return false;
  std::lock_guard lock(g_mutex);
  return g_wanted[p].valid && IsMaskState(g_wanted[p].state);
}

int WantedHost(int p) {
  if (p < 0 || p >= kPlayers || !On()) return -1;
  std::lock_guard lock(g_mutex);
  const Wanted& w = g_wanted[p];
  return w.valid && IsMaskState(w.state) && w.host != kNoHost ? int(w.host) : -1;
}

bool IsMaskTransitionNode(int node) {
  return node == kNodeEnterMask || node == kNodeMask || node == kNodeLeaveMask;
}

void Forget(int p) {
  if (p < 0 || p >= kPlayers) return;
  std::lock_guard lock(g_mutex);
  g_wanted[p] = {};
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_wanted = {};
  g_sent_state = ~0u;
  g_sent_host = -2;
  g_sent_at = {};
}

}  // namespace net::mask_sync
