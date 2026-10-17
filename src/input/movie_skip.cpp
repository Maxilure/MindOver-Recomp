// =============================================================================
// input/movie_skip.cpp -- see movie_skip.h
// =============================================================================
#include "movie_skip.h"

#include <atomic>
#include <chrono>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

extern "C" REX_FUNC(__imp__sub_82187EF0);  // CMoviePlayerBehaviour: Update (vtable 0x820334E4 slot 4)

namespace movie_skip {
namespace {

using Clock = std::chrono::steady_clock;

// The game object (*0x8259B190) +8 = the game state (5 = a level is played;
// the behaviour asks it through sub_82248EA8).
constexpr uint32_t kGameGlobal = 0x8259B190, kGameState = 8;
constexpr uint32_t kInPlay = 5;
// CMoviePlayerBehaviour: +36 = its CMoviePlayer; +56 bit 0x80 = a mode of its
// own (the Update's first branch: no movie input asked there).
constexpr uint32_t kPlayer = 36, kModeByte = 56;
constexpr uint8_t kOtherMode = 0x80;
// CMoviePlayer +296 bit 0x20 = the Skip prompt is shown (sub_822C49F0).
constexpr uint32_t kPlayerFlags = 296;
constexpr uint8_t kSkipShown = 0x20;
// How long one Update's "prompt hidden" counts (a few frames at 30 fps: a
// movie that ends simply stops refreshing it).
constexpr auto kFresh = std::chrono::milliseconds(250);

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// When the last Update saw a movie in play with its prompt hidden (steady
// clock ticks; 0 = never). Written on the main thread, read by input polls.
std::atomic<int64_t> g_hidden_seen{0};

}  // namespace

bool WaitsForAnyButton() {
  const int64_t seen = g_hidden_seen.load(std::memory_order_relaxed);
  return seen != 0 && Clock::now().time_since_epoch().count() - seen <
                          std::chrono::duration_cast<Clock::duration>(kFresh).count();
}

}  // namespace movie_skip

// CMoviePlayerBehaviour's Update: runs unchanged; afterwards, note whether a
// movie is playing in a level with its Skip prompt hidden (movie_skip.h).
extern "C" REX_FUNC(sub_82187EF0) {
  using namespace movie_skip;
  const uint32_t behaviour = ctx.r3.u32;
  __imp__sub_82187EF0(ctx, base);
  const uint32_t game = Read32(base, kGameGlobal);
  if (!behaviour || !game || Read32(base, game + kGameState) != kInPlay) return;
  if (*GuestPtr(base, behaviour + kModeByte) & kOtherMode) return;
  const uint32_t player = Read32(base, behaviour + kPlayer);
  if (!player) return;
  const bool shown = (*GuestPtr(base, player + kPlayerFlags) & kSkipShown) != 0;
  static bool was_shown = true;
  if (shown != was_shown) {
    REXLOG_INFO("Movie skip: prompt {}", shown ? "shown" : "hidden");
    was_shown = shown;
  }
  g_hidden_seen.store(shown ? 0 : Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}
