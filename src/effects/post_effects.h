// =============================================================================
// effects/post_effects.h -- the picture's effects as SETTINGS: depth of
// field (the game's own, both renderers), render resolution, anti-aliasing, motion blur, shadow
// softness and sharpening (native renderer only)
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
// RENDER RESOLUTION (findings/45): `render_resolution` (0 720p = the
// original, 1 1080p, 2 1440p, 3 4K) scales every surface and resolved
// texture of the native renderer by 1 / 1.5 / 2 / 3 (native/render_targets.h
// "RENDER SCALE"): the game still thinks in its 1280x720 pixels, we draw
// more of them. The emulated picture stays 720p.
//
// MOTION BLUR (findings/34): the flag `motion_blur` (0 off, 1 low, 2 medium,
// 3 high) is read by the native renderer's recorder (native/camera_motion.h,
// shaders/dof.frag). The Xbox 360 game never had motion blur (findings/21),
// so the emulated picture can't show it.
//
// SHADOW SOFTNESS (findings/41): `shadow_softness` (0 sharper, 1 original,
// 2 softer, 3 softest) widens or narrows the 9-tap blur the game's
// xnShadowShader runs over its stencil mask (native/recorder.cpp passes the
// scale, shaders/shadow.frag spreads the taps). Native renderer only.
//
// SHARPENING (findings/41): `sharpening` (0 off, 1 low, 2 medium, 3 high) =
// a contrast-adaptive sharpening pass (AMD's FidelityFX CAS idea) inside the
// native renderer's present pass (shaders/present.frag). Native only.
// =============================================================================

#pragma once

namespace post_effects {

// Installs the depth of field handler (CrashMomApp, once, before the game
// runs; it works whichever picture is shown).
void Install();

}  // namespace post_effects
