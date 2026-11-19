// =============================================================================
// input/movie_skip.cpp -- see movie_skip.h
// =============================================================================
#include "movie_skip.h"

#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

REXCVAR_DECLARE(bool, debug_key_events_trace);

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
// DEBUG (with --debug_key_events_trace): what the movie's two questions
// (sub_82274BE8: MOVIE map, PAUSE 36 / BACK 12) will find, BEFORE the
// Update asks: every controller of the two lists at *(uber+0x38) +8 / +24
// (the sockets, then others), its lock byte, its MOVIE maps and their
// events 12 / 36 (h = happened, r = removed). One line per change.
// (2026-11-15: keys made MOVIE 12 every press, yet a movie after a skipped
// 3D cutscene would not skip.)
static void TraceMovieQuestions(uint8_t* base) {
  using movie_skip::Read32;
  const uint32_t uber = Read32(base, 0x8259B190);
  if (!uber) return;
  const uint32_t lists = Read32(base, uber + 0x38);
  if (!lists) return;
  std::string out;
  for (uint32_t at : {8u, 24u}) {
    const uint32_t begin = Read32(base, lists + at), end = Read32(base, lists + at + 4);
    if (end < begin || end - begin > 4 * 16) continue;
    for (uint32_t c = begin; c < end; c += 4) {
      const uint32_t ctl = Read32(base, c);
      if (!ctl) continue;
      out += fmt::format(" [{}{:08X} lock {}", at == 8 ? "" : "other ", ctl, *GuestPtr(base, ctl + 0x204));
      const uint32_t mb = Read32(base, ctl + 0x198), me = Read32(base, ctl + 0x19C);
      for (uint32_t m = mb; m < me && m < mb + 4 * 32; m += 4) {
        const uint32_t map = Read32(base, m);
        if (int32_t(Read32(base, map + 0x18)) != 2) continue;
        out += fmt::format(" MOVIE {:08X}:", map);
        const uint32_t vec = Read32(base, map + 0x1C);
        const uint32_t eb = Read32(base, vec), ee = Read32(base, vec + 4);
        for (uint32_t e = eb; e + 28 <= ee && e < eb + 28 * 64; e += 28) {
          const int id = int(int32_t(Read32(base, e)));
          if (id != 12 && id != 36) continue;
          out += fmt::format(" {}{}{}", id, (*GuestPtr(base, e + 0x14) & 0x80) ? "h" : "",
                             *GuestPtr(base, e + 0x18) ? "r" : "");
        }
      }
      out += "]";
    }
  }
  static std::string last;
  if (out != last) {
    REXLOG_INFO("Movie skip: questions see{}", out);
    last = out;
  }
}

extern "C" REX_FUNC(sub_82187EF0) {
  using namespace movie_skip;
  const uint32_t behaviour = ctx.r3.u32;
  if (REXCVAR_GET(debug_key_events_trace)) TraceMovieQuestions(base);
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
