// =============================================================================
// lit.frag -- the game's "simple" material, lit variant, pixel side
// =============================================================================
// Written from our reading of the game's pixel shaders (simpleshaderlitfp,
// and palsimpleshaderlitfp for palettized textures; microcode disassembled
// with the SDK's --dump_shaders, inputs named by their constant tables;
// docs/findings/11). In order:
//
//   1. Up to 4 directional lights, each: diffuse (surface diffuse x light
//      colour x max(N.-L, 0)) plus specular (surface specular x light
//      colour x |N.V|^power, only where N.-L > 0 and N.V > 0). V points from
//      the camera to the surface, so "N.V > 0" means a surface facing AWAY
//      from the camera: the highlight hardly ever shows. That's the game's
//      shader as it is, reproduced faithfully. N is the interpolated normal,
//      not renormalized, like the game's.
//   2. colour = saturate(ambient x surface ambient + lights + emissive);
//      alpha = saturate(ambient.a x surface ambient.a + emissive.a + 1).
//   3. Textured materials: x the texture, then the Fx shadows.
//   4. "Proximity" point lights: the colour is multiplied by 1 + the sum of
//      their contributions (brightness x 0.2 x intensity x falloff squared,
//      falloff linear to 0 at the radius), then saturated.
//   5. Distance fog: towards the fog colour; additive / subtractive
//      materials fade out instead (colour x mix(fog colour, 1, f) x f).
//   6. The fade: alpha x fade; additive / subtractive also rgb x fade.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"
#include "lighting.glsl"
#include "palette.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_texture;  // point sampler if palettized
layout(set = 0, binding = 3) uniform sampler2D u_palette;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_view;
layout(location = 3) in vec3 in_world_position;
layout(location = 4) in float in_fog_depth;

layout(location = 0) out vec4 out_colour;

void main() {
  // 1. Directional lights.
  vec3 n = in_normal;
  float facing_away = dot(n, in_view);
  vec3 light = vec3(0.0);
  for (uint i = 0u; i < 4u; ++i) {
    if ((lit.counts.x & (1u << i)) == 0u) {
      continue;
    }
    float facing = dot(n, -lit.light_direction[i].xyz);
    float highlight = facing > 0.0 && facing_away > 0.0
                          ? Power(abs(facing_away), lit.specular_power.x)
                          : 0.0;
    light += (max(facing, 0.0) * lit.surface_diffuse.rgb +
              highlight * lit.surface_specular.rgb) * lit.light_colour[i].rgb;
  }

  // 2. Ambient + emissive.
  vec4 colour;
  colour.rgb = clamp(lit.ambient.rgb * lit.surface_ambient.rgb + light + lit.surface_emissive.rgb,
                     0.0, 1.0);
  colour.a = clamp(lit.ambient.a * lit.surface_ambient.a + lit.surface_emissive.a + 1.0, 0.0, 1.0);

  // 3. Texture, Fx shadows.
  if (LitFlag(kLitTextured)) {
    colour *= LitFlag(kLitPalettized)
                  ? SamplePalettized(u_texture, u_palette, lit.texture_sizes.xy, in_uv)
                  : texture(u_texture, in_uv);
    colour.rgb *= ShadowSpheres(in_world_position);
  }

  // 4. Point lights.
  if (lit.counts.y > 0u) {
    colour = clamp(vec4(colour.rgb * ProximityLights(in_world_position), colour.a), 0.0, 1.0);
  }

  // 5. Fog.
  bool fade_rgb = LitFlag(kLitFadeRgb);
  if (LitFlag(kLitFog)) {
    float f = FogFactor(in_fog_depth);
    colour.rgb = fade_rgb ? colour.rgb * mix(lit.fog_colour.rgb, vec3(1.0), f) * f
                          : mix(lit.fog_colour.rgb, colour.rgb, f);
  }

  // 6. Fade, alpha test.
  float fade = lit.fog_fade_alpha.z;
  colour.a *= fade;
  if (fade_rgb) {
    colour.rgb *= fade;
  }
  if (LitFlag(kLitAlphaTest) && !AlphaTestPasses(colour.a)) {
    discard;
  }
  out_colour = colour;
}
