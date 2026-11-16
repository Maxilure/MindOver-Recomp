// =============================================================================
// gameplay/camera_shake.cpp -- Options -> Accessibility -> Camera Shake
// =============================================================================
// WHAT
//   How much the camera shakes when something heavy lands, explodes or hits
//   (a titan's slam, a boss's stomp, a scripted quake): Off, Low, Medium or
//   Original (the default, AGENTS.md "keep the original's defaults"). For
//   players who get motion sick or find the shaking hard to follow. Only the
//   SIZE of the shaking changes: a shake lasts as long as in the original.
//   Works with both pictures (it is the game's camera logic, not drawing).
//
// HOW THE GAME SHAKES THE CAMERA (docs/findings/66-camera-shake.md)
//   Every shake goes through one door: the camera manager's shake entry
//   (sub_8223A950: a point in the world + a strength), called by the level
//   scripts' `CameraShake` and by the CActionCameraShake objective event.
//   It sends a camera message (type 7, subtype 5) to the camera's behaviour
//   controller (CCameraBehaviourController, vtable 0x8202973C), whose
//   message handler (slot 5, sub_82104E20) keeps the strongest shake as felt
//   at the camera: its point at +0xAC, its strength at +0xB8, a random phase
//   at +0xBC.
//   Each frame the controller's update (slot 4, sub_82104B70) turns that into
//   a wobble: felt = strength x (1 - distance / 40)^2 (sub_82105058, nothing
//   beyond 40 units), and the camera's position and direction are pushed by
//   waves (sines at 47, 37 and 33 x the phase) whose size grows with felt^2.
//   Then the strength drops by 10 per second and the phase moves on.
//
// THE CHANGE
//   Our update (below) wraps the game's: just before it, the stored strength
//   is scaled by sqrt(level) (the wobble's size goes with strength^2, so its
//   size becomes `level` of the original); just after, the strength is put
//   back with the game's own decay applied to it, so the shake still fades out
//   at the original's pace and lasts as long. "Off" uses a tiny scale, not 0:
//   with 0 the game skips the decay too and a stopped shake would come back
//   when the setting is raised again.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

// 0 off, 1 low, 2 medium, 3 original (the default).
REXCVAR_DEFINE_INT32(camera_shake, 3, "CrashMoM",
                     "How much the camera shakes (heavy landings, explosions, slams): 0 off, 1 low "
                     "(30 %), 2 medium (60 %), 3 original")
    .range(0, 3);

namespace camera_shake {
namespace {

// The controller's fields (findings/66 s.2).
constexpr uint32_t kStrength = 0xB8;  // float: the current shake's strength (0 = none)

// The wobble's size kept at each level (the Options row's Off / Low / Medium /
// Original). Off keeps 1e-6 of it: nothing visible, but the decay still runs.
constexpr float kSize[] = {1e-6f, 0.3f, 0.6f, 1.0f};

// Big-endian floats in guest memory (always through GuestPtr: AGENTS.md).
float LoadFloat(uint8_t* base, uint32_t address) {
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

}  // namespace
}  // namespace camera_shake

// CCameraBehaviourController::Update (vtable slot 4): r3 = this, f1 = the
// time step, r6 = the mode (the game's code does nothing unless it is 3).
extern "C" REX_FUNC(__imp__sub_82104B70);
extern "C" REX_FUNC(sub_82104B70) {
  using namespace camera_shake;
  const int level = std::clamp(int(REXCVAR_GET(camera_shake)), 0, 3);
  const uint32_t self = ctx.r3.u32;
  const float strength = level < 3 ? LoadFloat(base, self + kStrength) : 0.0f;
  if (!(strength > 0.0f)) {  // the original, or no shake right now
    __imp__sub_82104B70(ctx, base);
    return;
  }
  // The wobble's size goes with strength^2: scale the strength by its root.
  const float scaled = strength * std::sqrt(kSize[level]);
  StoreFloat(base, self + kStrength, scaled);
  __imp__sub_82104B70(ctx, base);
  // The game took its decay off the scaled strength (or nothing, in another
  // mode): take the same amount off the real one.
  const float after = LoadFloat(base, self + kStrength);
  StoreFloat(base, self + kStrength, strength - (scaled - after));
}
