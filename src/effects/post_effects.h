// =============================================================================
// effects/post_effects.h -- the picture's effects as SETTINGS: depth of
// field (the game's own, both renderers), anti-aliasing and motion blur
// (native renderer only)
// =============================================================================
//
// DEPTH OF FIELD (findings/15): the game blurs the distance a little every
// frame, through FramebufferFxExt::v30 (0x82440A50), which stores the
// settings (focal plane, near / far blur planes, "MaxRadius" = how many
// pixels the farthest blur reaches) into its xnDOFShader and draws the pass.
// The flag `depth_of_field` scales MaxRadius on the way in: 0 Off (radius 0:
// the pass still runs, every sample lands on its own pixel = a sharp
// picture), 1 Original, 2 Strong (twice the radius). Because it changes what
// the game hands its renderer, BOTH pictures (emulated and native) follow.
//
// ANTI-ALIASING (findings/35): `anti_aliasing` (0 off, 1 original = the
// game's 2x, 2 4x, 3 8x) picks the MSAA sample count of the surfaces the game
// multisamples, in the native renderer (native/render_targets.cpp,
// SamplesFor). The emulated picture keeps the Xbox's 2x.
//
// MOTION BLUR (findings/34): the flag `motion_blur` (0 off, 1 low, 2 medium,
// 3 high) is read by the native renderer's recorder (native/camera_motion.h,
// shaders/dof.frag). The Xbox 360 game never had motion blur (findings/21),
// so the emulated picture can't show it.
// =============================================================================

#pragma once

namespace post_effects {

// Installs the depth of field handler (CrashMomApp, once, before the game
// runs; it works whichever picture is shown).
void Install();

}  // namespace post_effects
