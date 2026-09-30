// =============================================================================
// character.frag -- the game's character material (xnCharacterRimShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shader (characterrimfp: no
// source ships with it; its microcode was disassembled with the SDK's
// --dump_shaders, and its constant table names each input; docs/findings/11).
// In order, it does:
//
//   1. The normal: the normal map (RGB -> -1..1) is a direction in the
//      surface's own frame (tangent, binormal, normal): turned into a world
//      direction N.
//   2. The surface colour: the diffuse texture, optionally blended towards
//      a second texture (the "evil map": weight lit.character.y), optionally
//      flashed yellow (red and green + a quarter of lit.character.x).
//   3. Up to 4 directional lights: each adds diffuse light (surface diffuse
//      x light colour x how much the surface faces the light) and a specular
//      highlight (the light's reflection off N, seen from the camera, to the
//      power "specular power"). Quirk kept from the game: the diffuse part
//      uses the plain interpolated vertex normal, not the normal-mapped one.
//      colour = (ambient x surface ambient + diffuse) x surface colour + specular
//   4. Fx shadows (lighting.glsl).
//   5. Rim light: surfaces seen edge-on (N at right angles to the view)
//      glow in the rim colour: (1 - |N.V|)^3 x amount x rim colour x
//      mix(rim colour, colour, rim blend). What gives Crash his outline glow.
//   6. Distance fog, then alpha = surface alpha x fade.
//
// Not done yet: the "refraction" variant (the pixel shader's bool b11; it
// mixes the colour with a copy of the screen, bent by the normal map, for
// glassy characters). The recorder reports when a draw wants it.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"
#include "lighting.glsl"
#include "palette.glsl"

// For palettized textures these samplers are point (nearest) ones.
layout(set = 0, binding = 0) uniform sampler2D u_diffuse;
layout(set = 0, binding = 1) uniform sampler2D u_normal_map;
layout(set = 0, binding = 2) uniform sampler2D u_blend_map;
layout(set = 0, binding = 3) uniform sampler2D u_diffuse_palette;
layout(set = 0, binding = 4) uniform sampler2D u_normal_palette;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_tangent;
layout(location = 3) in vec3 in_binormal;
layout(location = 4) in vec3 in_world_position;
layout(location = 5) in float in_fog_depth;

layout(location = 0) out vec4 out_colour;

void main() {
  // 1. The normal-mapped normal. Without a normal map (the game then
  // leaves whatever texture was bound before on that sampler), a flat one:
  // the plain vertex normal.
  vec3 bump = LitFlag(kLitNoNormalMap)
                  ? vec3(0.0, 0.0, 1.0)
                  : (LitFlag(kLitNormalPalettized)
                         ? SamplePalettized(u_normal_map, u_normal_palette, lit.texture_sizes.zw,
                                            in_uv)
                         : texture(u_normal_map, in_uv)).xyz * 2.0 - 1.0;
  vec3 n = normalize(normalize(in_tangent) * bump.x + normalize(in_binormal) * bump.y +
                     normalize(in_normal) * bump.z);
  vec3 v = normalize(lit.eye.xyz - in_world_position);  // towards the camera

  // 2. The surface colour.
  vec4 surface = LitFlag(kLitPalettized)
                     ? SamplePalettized(u_diffuse, u_diffuse_palette, lit.texture_sizes.xy, in_uv)
                     : texture(u_diffuse, in_uv);
  if (lit.character.y > 0.0 && LitFlag(kLitBlendMap)) {
    surface = mix(surface, texture(u_blend_map, in_uv), lit.character.y);
  }
  if (lit.character.x > 0.0) {
    surface.rg = clamp(surface.rg + 0.25 * lit.character.x, 0.0, 1.0);
    surface.ba = clamp(surface.ba, 0.0, 1.0);
  }

  // 3. Directional lights.
  vec3 diffuse = vec3(0.0), specular = vec3(0.0);
  for (uint i = 0u; i < 4u; ++i) {
    if ((lit.counts.x & (1u << i)) == 0u) {
      continue;
    }
    vec3 l = lit.light_direction[i].xyz;
    float facing = clamp(dot(in_normal, -l), 0.0, 1.0);
    float highlight = Power(clamp(dot(v, reflect(l, n)), 0.0, 1.0), lit.specular_power.x);
    diffuse += facing * lit.surface_diffuse.rgb * lit.light_colour[i].rgb;
    specular += highlight * lit.surface_specular.x * lit.light_colour[i].rgb;
  }
  vec3 colour = (lit.ambient.rgb * lit.surface_ambient.rgb + diffuse) * surface.rgb + specular;

  // 4. Fx shadows.
  colour *= ShadowSpheres(in_world_position);

  // 5. Rim light.
  if (lit.character.w > 0.0) {
    float edge = 1.0 - abs(dot(n, v));
    float rim = edge * edge * edge;
    vec3 tinted = mix(lit.rim_colour.rgb, colour, lit.character.z);
    colour += rim * lit.rim_colour.rgb * lit.character.w * tinted;
  }

  // 6. Fog, fade, alpha test.
  if (LitFlag(kLitFog)) {
    colour = mix(lit.fog_colour.rgb, colour, FogFactor(in_fog_depth));
  }
  float alpha = surface.a * lit.fog_fade_alpha.z;
  if (LitFlag(kLitAlphaTest) && !AlphaTestPasses(alpha)) {
    discard;
  }
  out_colour = vec4(colour, alpha);
}
