// =============================================================================
// input/background_input.cpp -- see background_input.h
// =============================================================================
#include "background_input.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <SDL3/SDL_hints.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../players/more_players.h"

REXCVAR_DEFINE_BOOL(background_input, true, "CrashMoM",
                    "Controllers keep playing while the game window isn't focused (the keyboard "
                    "and mouse stop). false = the game opens its pause menu by itself when the "
                    "window loses the focus in a level");

extern "C" REX_FUNC(__imp__sub_8211E850);  // the front end's in-game decision (fight-tree node)
extern "C" REX_FUNC(sub_82265A60);  // "may nobody pause now?" (every player's controller locked: cutscenes)

namespace background_input {
namespace {

// The in-game decision's exits: -1 = stay (nothing happened), 516 = the
// pause menu: START (IsButtonPressed button 36) at 0x8211EAF4, taken only
// while the game isn't paused already (game +168 bit 0x20) and sub_82265A60
// says "no" (r3 = front end; "yes" = every player's controller is locked,
// sub_82272D68: cutscenes). Not 502: that's Back (button 32) = the MAP
// (a first test opened the map screen, state 927).
constexpr int32_t kStay = -1;
constexpr int32_t kPauseExit = 516;
constexpr uint32_t kPauseBits = 168;
constexpr uint8_t kGamePaused = 0x20;

std::mutex g_check_mutex;
std::function<bool()> g_focused;  // guarded by g_check_mutex

// The game object (*0x8259B190) +52 = the front end; its +144 = the level's
// fade-in timer, 1 -> 0 over ~1.2 s after GameRunning (debug_console.cpp
// "wait level"). A pause opened before it reaches 0 came up over a black
// picture (tested: 0.2 s after the level began).
constexpr uint32_t kGameGlobal = 0x8259B190, kFrontEnd = 52, kFadeIn = 144;

float ReadF32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  const uint32_t u = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Would the game's own START open the pause menu now (the two checks START's
// branch makes after the button)? The caller's registers are put back.
bool StartWouldPause(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game || (*GuestPtr(base, game + kPauseBits) & kGamePaused)) return false;
  const PPCContext saved = ctx;
  ctx.r3.u64 = Read32(base, game + kFrontEnd);
  sub_82265A60(ctx, base);
  const bool nobody_may = ctx.r3.u32 & 0xFF;
  ctx = saved;
  return !nobody_may;
}

// Is the level on screen: faded in (the decision only runs in GameRunning)?
bool FadedIn(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return false;
  const uint32_t front_end = Read32(base, game + kFrontEnd);
  return front_end && ReadF32(base, front_end + kFadeIn) <= 0.0f;
}

// The value last handed to SDL (-1 = never).
std::atomic<int> g_applied{-1};

// Seen by the decision: was a window focused at its last call? A drop from
// true to false = one pause owed (g_owed), paid at the next "stay" answer.
bool g_was_focused = true;  // game thread only
bool g_owed = false;        // game thread only

}  // namespace

bool Allowed() { return REXCVAR_GET(background_input); }

void Apply() {
  const int want = Allowed() ? 1 : 0;
  if (g_applied.exchange(want) == want) return;
  // SDL watches this hint: joystick / gamepad events then arrive (or are
  // dropped again) at once, no restart.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, want ? "1" : "0");
  REXLOG_INFO("Background input: controllers {} while the window isn't focused",
              want ? "keep playing" : "stop (the game pauses itself)");
}

void SetFocusCheck(std::function<bool()> focused) {
  std::lock_guard<std::mutex> lock(g_check_mutex);
  g_focused = std::move(focused);
}

bool Focused() {
  std::lock_guard<std::mutex> lock(g_check_mutex);
  return !g_focused || g_focused();
}

}  // namespace background_input

// The front end's in-game decision, asked every frame while a level runs and
// no in-game menu is up. The original first (its own exits win: START, the
// map, tutorials...); only a "stay" can become the pause menu.
extern "C" REX_FUNC(sub_8211E850) {
  using namespace background_input;
  __imp__sub_8211E850(ctx, base);
  // The setting may have changed from the debug console's `flag`: keep SDL
  // in step (no-op when unchanged).
  Apply();
  const bool focused = Focused();
  if (Allowed()) {
    g_owed = false;
    g_was_focused = focused;
    return;
  }
  if (g_was_focused && !focused) g_owed = true;
  if (focused) g_owed = false;  // came back before a pause could open
  g_was_focused = focused;
  // Only while the level can be played and is on screen (not under its
  // loading screen or its fade-in from black).
  if (!g_owed || int32_t(ctx.r3.u32) != kStay || !more_players::InPlay() || more_players::Loading() ||
      !FadedIn(base) || !StartWouldPause(ctx, base)) {
    return;
  }
  g_owed = false;
  ctx.r3.u64 = uint32_t(kPauseExit);
  REXLOG_INFO("Background input: the window lost the focus: pause menu opened");
}
