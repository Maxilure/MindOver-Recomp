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
//        at their highlighted size; the button prompts floating over things
//        to use stop wobbling.
//     3. The game's slow-motion moments run at normal speed.
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
//      factor at manager +4 (unless bit 0x80 of +0x18). A factor between 0
//      and 1 is divided back out.
//   4. native/recorder.cpp asks MotionBlurLevel() instead of reading
//      `motion_blur` itself.
// =============================================================================
#include "reduce_motion.h"

#include <cstdint>
#include <cstring>

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
constexpr uint32_t kIconWobble = 0x6C;      // CIconBehaviour: its wobble's timer

}  // namespace

bool On() { return REXCVAR_GET(reduce_motion); }

int MotionBlurLevel() { return On() ? 0 : int(REXCVAR_GET(motion_blur)); }

double GameTimeScale(const uint8_t* base, uint32_t manager, double scale) {
  if (!On() || manager == 0) return scale;
  const float factor = LoadFloat(base, manager + kSlowMotion);
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
// time step. The icons floating over things to use (the button prompt over
// a door or a hatch) wobble: the timer at +0x6C advances each frame and the
// icon is turned by sin(timer) and cos(timer x a constant). Held at 0, the
// icon stays upright in its resting place.
extern "C" REX_FUNC(__imp__sub_82178ED8);
extern "C" REX_FUNC(sub_82178ED8) {
  if (!reduce_motion::On()) {
    __imp__sub_82178ED8(ctx, base);
    return;
  }
  const uint32_t icon = ctx.r3.u32;
  reduce_motion::StoreFloat(base, icon + reduce_motion::kIconWobble, 0.0f);
  __imp__sub_82178ED8(ctx, base);
  reduce_motion::StoreFloat(base, icon + reduce_motion::kIconWobble, 0.0f);
}
