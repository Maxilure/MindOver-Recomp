// =============================================================================
// simple.frag -- the game's "simple" material (xnSimpleShader), pixel side
// =============================================================================
// Written from our reading of the game's unlit pixel shaders
// (simpleshaderunlitfp, palsimpleshaderunlitfp; microcode disassembled with
// the SDK's --dump_shaders, see docs/findings/09). For the 2D screens they
// come down to:
//
//   colour = vertex colour, times the texture if the material has one
//   alpha *= fade                  (menus fading in and out)
//   rgb   *= fade  for additive / subtractive blending (where alpha does nothing)
//
// In the 3D world they also do, in this order (docs/findings/20):
//
//   colour *= proximity lights   (up to 4 coloured point lights that
//                                 brighten what's near them: the green
//                                 flash of the Ratcicle's roar)
//   colour *= 1 - Fx shadow      (up to 8 points darken what's near them,
//                                 up to 65% here, 30% in the lit shaders)
//   distance fog                 (the fog colour gets the same shadow)
//
// Both sections are off (counts 0) on the 2D screens; their numbers live in
// the draw's LitParams block (kFlagLights), the maths in lighting.glsl.
// Alpha test (the GPU compares the output alpha with a reference) is done
// here with `discard`.
//
// PALETTIZED TEXTURES (8-bit indices into a 256-colour palette) are filtered
// by hand: palette.glsl.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "simple_params.glsl"
#include "lit_params.glsl"
#include "lighting.glsl"
#include "palette.glsl"

// For palettized textures this sampler is a point (nearest) one.
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_palette;

layout(location = 0) in vec4 in_colour;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in float in_fog_depth;
layout(location = 3) in vec3 in_world_position;  // only with kFlagLights

layout(location = 0) out vec4 out_colour;

void main() {
  uint flags = params.flags.x;
  vec4 colour = in_colour;
  if ((flags & kFlagTextured) != 0u) {
    colour *= (flags & kFlagPalettized) != 0u
                  ? SamplePalettized(u_texture, u_palette, params.texture_size_fog_range.xy, in_uv)
                  : texture(u_texture, in_uv);
  }
  // Proximity lights, then Fx shadows (lighting.glsl). The game's shader
  // clamps colour and alpha to 0..1 after the lights; the shadow factor also
  // darkens the fog colour below (its microcode multiplies both by it).
  float unshadowed = 1.0;
  if ((flags & kFlagLights) != 0u) {
    if (lit.counts.y > 0u) {
      colour = clamp(vec4(colour.rgb * ProximityLights(in_world_position), colour.a), 0.0, 1.0);
    }
    unshadowed = ShadowSpheres(in_world_position, 0.65);
    colour.rgb *= unshadowed;
  }
  // Distance fog, as the game's shader does it: blend towards the fog colour,
  // fully fogged beyond `end`, not at all nearer than `start`. Additive /
  // subtractive materials fade out instead (their colour is added to what's
  // behind, so "towards the fog colour" would brighten): colour x
  // mix(fog colour, 1, f) x f, like the lit shaders (lit.frag).
  if ((flags & kFlagFog) != 0u) {
    float start = params.texture_size_fog_range.z, end = params.texture_size_fog_range.w;
    float f = in_fog_depth < start ? 1.0
            : in_fog_depth > end   ? 0.0
                                   : (end - in_fog_depth) / max(end - start, 1e-6);
    vec3 fog_colour = params.alpha_ref_fog_colour.yzw * unshadowed;
    if ((flags & kFlagFadeRgb) != 0u) {
      colour.rgb *= mix(params.alpha_ref_fog_colour.yzw, vec3(1.0), f) * f;
    } else {
      colour.rgb = mix(fog_colour, colour.rgb, f);
    }
  }
  float fade = params.tint_fade.a;
  colour.a *= fade;
  if ((flags & kFlagFadeRgb) != 0u) {
    colour.rgb *= fade;
  }
  if ((flags & kFlagAlphaTest) != 0u && !AlphaTest(colour.a)) {
    discard;
  }
  out_colour = colour;
}
