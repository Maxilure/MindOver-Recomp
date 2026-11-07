// =============================================================================
// effects/post_effects.cpp -- see post_effects.h
// =============================================================================
#include "post_effects.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>

#include "../pddi/intercept.h"

REXCVAR_DEFINE_INT32(depth_of_field, 1, "CrashMoM",
                     "Depth of field (the distance gently blurred), both renderers: 0 off, "
                     "1 original, 2 strong (twice the blur)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(anti_aliasing, 1, "CrashMoM",
                     "Anti-aliasing (MSAA) of the 3D scene, native renderer only: 0 off, "
                     "1 original (2x, like the Xbox 360), 2 4x, 3 8x")
    .range(0, 3);
REXCVAR_DEFINE_INT32(smooth_cutouts, 0, "CrashMoM",
                     "Smooth edges on leaves, fences and other cut-out shapes (their alpha test becomes "
                     "alpha-to-coverage) where anti-aliasing is on, native renderer only: 0 off, 1 on")
    .range(0, 1);
// DEFAULT 1080 (2026-11-06 the port's out-of-the-box 1080p; with the emulated
// picture it does nothing). 2026-11-07: a PIXEL HEIGHT (any, 360-4320), -1 =
// the window's own; the old step numbers 0-3 (720p, 1080p, 1440p, 4K) still
// read the same, so settings files from before need nothing.
REXCVAR_DEFINE_INT32(render_resolution, 1080, "CrashMoM",
                     "Render resolution of the 3D scene and menus, native renderer only: the "
                     "picture's HEIGHT in pixels (720 = the original, 1080, 1440, 2160 = 4K, ...; "
                     "the width follows the aspect ratio), or -1 = match the window. Old values "
                     "0-3 = 720p, 1080p, 1440p, 4K")
    .range(-1, 4320);
REXCVAR_DEFINE_INT32(motion_blur, 0, "CrashMoM",
                     "Motion blur, the port's own (the original has none), native renderer only: "
                     "0 off, 1 low, 2 medium, 3 high")
    .range(0, 3);
REXCVAR_DEFINE_INT32(shadow_softness, 1, "CrashMoM",
                     "Soft character shadows' edge, native renderer only: 0 sharper, 1 original, "
                     "2 softer, 3 softest")
    .range(0, 3);
REXCVAR_DEFINE_INT32(sharpening, 0, "CrashMoM",
                     "Sharpening filter over the finished picture (contrast-adaptive), native renderer "
                     "only: 0 off, 1 low, 2 medium, 3 high")
    .range(0, 3);

namespace post_effects {
namespace {

// What each depth_of_field value does to the game's MaxRadius.
constexpr float kRadiusScale[3] = {0.0f, 1.0f, 2.0f};

float FloatOf(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint32_t BitsOf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// FramebufferFxExt::v30 (0x82440A50): r3 = the extension, f1 focal plane,
// f2 near blur plane, f3 far blur plane, r7 = MaxRadius, a pair of floats
// passed IN ONE 64-bit register (the function stores it with one `std` at
// xnDOFShader +108: the first float in the high half). Scaled, then the
// original stores it and draws.
void OnDepthOfField(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const int setting = std::clamp(int(REXCVAR_GET(depth_of_field)), 0, 2);
  if (setting != 1) {
    const float scale = kRadiusScale[setting];
    const uint64_t pair = ctx.r7.u64;
    const float first = FloatOf(uint32_t(pair >> 32)) * scale;
    const float second = FloatOf(uint32_t(pair)) * scale;
    ctx.r7.u64 = uint64_t(BitsOf(first)) << 32 | BitsOf(second);
  }
  original(ctx, base);
}

}  // namespace

void Install() { pddi::SetHandler(pddi::kFn_82440A50, &OnDepthOfField); }

}  // namespace post_effects
