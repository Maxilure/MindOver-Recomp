// =============================================================================
// gameplay/reduce_motion.cpp -- Options -> Accessibility -> Reduce Motion
// =============================================================================
// WHAT
//   One switch (off by default: the original) for players who get dizzy or
//   sick from movement on screen that is not needed to play. With it on:
//     1. Menu animations end at once: every front-end transition (the panels
//        that slide in, the items that spring past their place and settle)
//        jumps straight to its end.
//     2. The selected menu item and the pulsing texts stop pulsing: they stay
//        at their highlighted size; the icons floating over things (button
//        prompts, the "!" and its flash) stop wobbling and pulsing.
//     2b. Mojo and other pick-ups are collected at once instead of flying to
//        the player.
//     2c. The character on the Level Up / upgrade screen (Crash spinning or
//        kicking, a titan) stays on one frame of its animation.
//     3. The game's slow-motion moments run at normal speed (the scripted
//        ones; the Phantom's time shift, an ability, keeps its slowdown).
//     4. The port's motion blur stays off whatever its own row says.
//   Research and what is not covered: docs/findings/67-reduce-motion.md.
//
// HOW (the game's own functions, wrapped; the originals stay as __imp__)
//   1. CFETransition (no RTTI base; subclasses CFETransitionLinear, vtable
//      0x8203A0A8, and CFETransitionOverShoot, 0x8203A0B0) is the front end's
//      animated value: +4 direction (0 forwards, 1 backwards), +8 start,
//      +0xC end, +0x10 progress 0..1, +0x14 speed (1 / seconds), +0x18
//      running. Its step, sub_8225EAE0(this, f1 = time step), moves the
//      progress and stops at the end. With the setting on it is given a huge
//      time step, so the progress lands on its end on the first frame and the
//      value read from it (linear, or OverShoot's start + (end - start) x
//      (2t - t^3)) is the end value. 23 front-end screens step their
//      transitions this way (Options' slide sub_820CE2C8 among them).
//   2. The Scrooby menu's update (Menu vtable slot 5, sub_8237A070) scales
//      the selected item by 1.1 + 0.1 x |sin(timer)|, timer at menu +0x94
//      (+4 per second); a Scrooby text's pulse (Text vtable slot 5,
//      sub_823730B0) does the same with its timer at +0xAC. The timer is held
//      at 0 around the original call: the item keeps the size 1.1 and stops
//      pulsing (it still turns yellow).
//   3. CTimeManager::GetScale (sub_822FFA40, cheats/cheats.cpp wraps it and
//      asks GameTimeScale below) returns a per-mode base x the slow-motion
//      factor at manager +4 (unless bit 0x80 of +0x18). Two things ease that
//      factor (the manager's update, vtable slot 10, sub_822FF7B8): a
//      scripted slow motion (EnableSlowMotion, sub_822FF8A8: target at +8)
//      and the TIME SHIFT, a gameplay ability (the Phantom's special; pushed
//      and popped by sub_822FF8F0: counter +0xC, its factor +0x10). Only the
//      scripted one is a "slow-motion moment": a factor between 0 and 1 is
//      divided back out while it comes from a script, never while a time
//      shift is or was last behind it (findings/67 s.2.3).
//   4. native/recorder.cpp asks MotionBlurLevel() instead of reading
//      `motion_blur` itself.
// =============================================================================
#include "reduce_motion.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(reduce_motion, false, "CrashMoM",
                    "Accessibility: menu animations end at once, menu items and texts stop pulsing, "
                    "slow-motion moments run at normal speed and motion blur stays off");
REXCVAR_DECLARE(int32_t, motion_blur);  // effects/post_effects.cpp

namespace reduce_motion {
namespace {

// Big-endian guest floats, always through GuestPtr (AGENTS.md s.5).
float LoadFloat(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, address), sizeof(v));
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
void StoreFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, sizeof(v));
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, address), &v, sizeof(v));
}

constexpr uint32_t kMenuPulseTimer = 0x94;  // Scrooby Menu: the selected item's pulse timer
constexpr uint32_t kTextPulseTimer = 0xAC;  // Scrooby Text: its pulse timer
constexpr uint32_t kSlowMotion = 0x04;      // CTimeManager: the slow-motion factor
// CTimeManager: a flags byte; while its 0x80 bit is set the game IGNORES the
// factor (GetScale sub_822FFA40: `lbz r9,24(r31)`, `rlwinm r6,r9,0,0,24`,
// `bne` past the multiply), so there is nothing to undo then.
constexpr uint32_t kTimeFlags = 0x18;
constexpr uint8_t kIgnoreSlowMotion = 0x80;
// CTimeManager +0x18 bit 0x40: a time shift is active. The update
// (sub_822FF7B8) copies it from the time-shift counter at +0xC (> 0) each
// frame; while it is set the factor at +4 eases toward the time shift's
// factor (+0x10) instead of the scripted target (+8).
constexpr uint8_t kTimeShiftActive = 0x40;
constexpr uint32_t kTimeShiftCount = 0x0C;  // CTimeManager: time shifts pushed (sub_822FF8F0)
constexpr uint32_t kSlowMotionTarget = 0x08;  // CTimeManager: EnableSlowMotion's target factor
constexpr uint32_t kIconWobble = 0x6C;      // CIconBehaviour: its wobble's timer
constexpr uint32_t kCollectableTimer = 0xC4;  // Collectable: the current state's timer (seconds)
constexpr uint32_t kCollectableState = 0xC8;  // Collectable: its state (4 = flying to the player)
constexpr uint32_t kStateFlying = 4;
constexpr float kFlightSeconds = 0.5f;      // the flight's progress is timer x 2 (constant 0x8201FB74)
constexpr uint32_t kUpgradePuppet = 0x3C;   // CUpgradeScreenAction: the shown character's puppet

// A big-endian guest word.
uint32_t LoadWord(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, address), sizeof(v));
  return __builtin_bswap32(v);
}

}  // namespace

bool On() { return REXCVAR_GET(reduce_motion); }

int MotionBlurLevel() { return On() ? 0 : int(REXCVAR_GET(motion_blur)); }

double GameTimeScale(const uint8_t* base, uint32_t manager, double scale) {
  // Who is behind the slowdown now: a script's slow-motion moment (undone)
  // or a time shift (the Phantom's special: gameplay, kept). Remembered
  // until the factor is back to normal, so a time shift's ease back to 1
  // after it ends stays the game's own too. (One time manager; this runs
  // on the game's main thread, from GetScale.)
  static bool s_time_shift_behind = false;
  if (manager == 0) return scale;
  const uint8_t flags = *GuestPtr(base, manager + kTimeFlags);
  const float factor = LoadFloat(base, manager + kSlowMotion);
  const bool time_shift = (flags & kTimeShiftActive) || int32_t(LoadWord(base, manager + kTimeShiftCount)) > 0;
  if (time_shift) {
    s_time_shift_behind = true;
  } else if (!(factor > 0.0f && factor < 1.0f) || LoadFloat(base, manager + kSlowMotionTarget) < 1.0f) {
    // Back to normal, or a script asked for slow motion since: the
    // slowdown is no longer the time shift's.
    s_time_shift_behind = false;
  }
  if (!On()) return scale;
  // The game left the factor out of `scale` (flag set): dividing it back out
  // would run the game FASTER than normal (e.g. 4x for a stored 0.25).
  if (flags & kIgnoreSlowMotion) return scale;
  // The time shift is an ability the player uses: its slowdown is how it
  // plays, not a cinematic moment. Left as it is.
  if (s_time_shift_behind) return scale;
  // Only a real slowdown is undone; 0 (stopped) and >= 1 stay as they are.
  return factor > 0.0f && factor < 1.0f ? scale / double(factor) : scale;
}

}  // namespace reduce_motion

// CFETransition's step: r3 = this, f1 = the time step (seconds).
// f1 is put back afterwards: the step doesn't change it, and its callers go
// on using it as their own time step (the announcement display subtracts it
// from its display time right after, sub_8225B750).
extern "C" REX_FUNC(__imp__sub_8225EAE0);
extern "C" REX_FUNC(sub_8225EAE0) {
  if (!reduce_motion::On()) {
    __imp__sub_8225EAE0(ctx, base);
    return;
  }
  const double step = ctx.f1.f64;
  ctx.f1.f64 = 1.0e6;  // any transition's whole length at once: it ends now
  __imp__sub_8225EAE0(ctx, base);
  ctx.f1.f64 = step;
}

// Scrooby Menu update (slot 5): r3 = the menu, f1 = the time step. The pulse
// timer is held at 0 (sin 0 = 0: the size stays 1.1).
extern "C" REX_FUNC(__imp__sub_8237A070);
extern "C" REX_FUNC(sub_8237A070) {
  if (!reduce_motion::On()) {
    __imp__sub_8237A070(ctx, base);
    return;
  }
  const uint32_t menu = ctx.r3.u32;
  reduce_motion::StoreFloat(base, menu + reduce_motion::kMenuPulseTimer, 0.0f);
  __imp__sub_8237A070(ctx, base);
  reduce_motion::StoreFloat(base, menu + reduce_motion::kMenuPulseTimer, 0.0f);
}

// Scrooby Text pulse (slot 5): r3 = the text, f1 = the time step. Same hold.
extern "C" REX_FUNC(__imp__sub_823730B0);
extern "C" REX_FUNC(sub_823730B0) {
  if (!reduce_motion::On()) {
    __imp__sub_823730B0(ctx, base);
    return;
  }
  const uint32_t text = ctx.r3.u32;
  reduce_motion::StoreFloat(base, text + reduce_motion::kTextPulseTimer, 0.0f);
  __imp__sub_823730B0(ctx, base);
  reduce_motion::StoreFloat(base, text + reduce_motion::kTextPulseTimer, 0.0f);
}

// CIconBehaviour::Update (slot 4, vtable 0x8203248C): r3 = this, f1 = the
// time step. The icons floating over things (the button prompt over a door,
// the "!" over someone to talk to, with its flash) move two ways: the update
// advances a timer at +0x6C by 3 x the step and turns the icon by sin(timer)
// x 0.5 and sin(timer x 2) x 0.2; then it passes the step to the icon's own
// drawable (+0x74, vtable slot 5), whose animation is the flash growing and
// shrinking. With the setting on the update runs with a step of 0: the
// timer stays at 0 (the icon upright in its resting place) and the
// animation stays on its first frame. f1 is put back afterwards.
extern "C" REX_FUNC(__imp__sub_82178ED8);
extern "C" REX_FUNC(sub_82178ED8) {
  if (!reduce_motion::On()) {
    __imp__sub_82178ED8(ctx, base);
    return;
  }
  const uint32_t icon = ctx.r3.u32;
  const double step = ctx.f1.f64;
  reduce_motion::StoreFloat(base, icon + reduce_motion::kIconWobble, 0.0f);
  ctx.f1.f64 = 0.0;
  __imp__sub_82178ED8(ctx, base);
  ctx.f1.f64 = step;
}

// Collectable::Update (vtable 0x82034DF4, slot 21): r3 = this, r4 = its
// actor, f1 = the time step. A collectable (mojo, health, power-ups) runs a
// small state machine at +0xC8 with a timer at +0xC4 (the update adds the
// step to it first). State 4 is the flight to the player who picked it up:
// from the point where it was picked up (+0xD0, 3 floats) to the player's
// position along a small arc, progress = timer x 2 (half a second), and at
// progress 1 it counts as collected. With the setting on, a collectable in
// that state gets its timer set so the progress is exactly 1 on this frame:
// it is collected at once, without the flight.
extern "C" REX_FUNC(__imp__sub_82198738);
extern "C" REX_FUNC(sub_82198738) {
  if (reduce_motion::On()) {
    const uint32_t self = ctx.r3.u32;
    if (reduce_motion::LoadWord(base, self + reduce_motion::kCollectableState) == reduce_motion::kStateFlying) {
      const float step = float(ctx.f1.f64);
      reduce_motion::StoreFloat(base, self + reduce_motion::kCollectableTimer,
                                // + a hair, so float rounding can't leave it at 0.99999
                                std::max(0.0f, reduce_motion::kFlightSeconds - step) + 1e-4f);
    }
  }
  __imp__sub_82198738(ctx, base);
}

namespace reduce_motion {

// LEVEL UP / UPGRADE SCREEN (CUpgradeScreenAction, vtable 0x82025AA4): it
// shows the player's character (Crash or the titan being upgraded) as a
// real actor of its own (screen +0x34) whose animations (spinning, kicking,
// a titan's moves) play through a choreo::Puppet (screen +0x3C, vtable
// 0x8200ABD8). sub_820E57A0 builds both when the screen opens; the screen's
// exit (slot 6, sub_820E5F20) destroys them. Every frame the puppet is
// advanced by Puppet::Advance (sub_82350F88: r3 = the puppet, f1 = the time
// step), like any actor's. The screen's puppets are noted here; with the
// setting on they are advanced by 0, so the character stays on the frame
// its animation starts on (the first Advance, made while the screen builds
// it, still runs: it puts the character in its first pose).
std::mutex g_frozen_mutex;
std::vector<uint32_t> g_frozen_puppets;  // the open upgrade screens' puppets

bool IsFrozenPuppet(uint32_t puppet) {
  std::lock_guard lock(g_frozen_mutex);
  return std::find(g_frozen_puppets.begin(), g_frozen_puppets.end(), puppet) != g_frozen_puppets.end();
}

// Called by the port's own wrappers of the two screen functions (they are
// already overridden for other work: players/outfits.cpp sub_820E57A0,
// players/menu_input.cpp sub_820E5F20).
void UpgradeScreenBuilt(const uint8_t* base, uint32_t screen) {
  const uint32_t puppet = LoadWord(base, screen + kUpgradePuppet);
  if (puppet == 0) return;
  std::lock_guard lock(g_frozen_mutex);
  g_frozen_puppets.push_back(puppet);
}
void UpgradeScreenClosing(const uint8_t* base, uint32_t screen) {
  const uint32_t puppet = LoadWord(base, screen + kUpgradePuppet);
  std::lock_guard lock(g_frozen_mutex);
  g_frozen_puppets.erase(std::remove(g_frozen_puppets.begin(), g_frozen_puppets.end(), puppet),
                         g_frozen_puppets.end());
}

}  // namespace reduce_motion

// choreo::Puppet::Advance: r3 = the puppet, f1 = the time step. f1 is put
// back after a frozen call.
extern "C" REX_FUNC(__imp__sub_82350F88);
extern "C" REX_FUNC(sub_82350F88) {
  if (!reduce_motion::On() || !reduce_motion::IsFrozenPuppet(ctx.r3.u32)) {
    __imp__sub_82350F88(ctx, base);
    return;
  }
  const double step = ctx.f1.f64;
  ctx.f1.f64 = 0.0;
  __imp__sub_82350F88(ctx, base);
  ctx.f1.f64 = step;
}
