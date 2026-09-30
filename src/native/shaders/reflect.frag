// =============================================================================
// reflect.frag -- the game's reflect material (xnReflectShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shaders (reflectfp, and
// palreflectfp for palettized textures; microcode only, disassembled with
// the SDK's --dump_shaders, inputs named by their constant tables;
// docs/findings/13). What makes N. Gin's lab on Wumpa Island look like
// polished metal. In order:
//
//   1. Up to 4 directional lights (bools b0-b3, direction / colour in c9-c16):
//      diffuse = light colour x saturate(N.-L), summed; specular = light
//      colour x saturate(N.H)^power (c68.x), H = the "half vector" between
//      the directions to the camera and to the light. With NO light on,
//      diffuse counts as full white (1, 1, 1) and there's no specular.
//      Unlike the lit simple shader there's no surface diffuse / specular
//      colour, and nothing is clamped. N is the interpolated normal, not
//      renormalized, like the game's.
//   2. light = ambient (c8) x surface ambient (c67) + diffuse + emissive (c66).
//   3. The reflection: the second texture is a "sphere map" (a picture of the
//      surroundings as seen in a mirrored ball), looked up by the world-space
//      normal: u = 0.5 + 0.5 N.x, v = 0.5 - 0.5 N.y. It's scaled by the
//      diffuse texture's ALPHA (the texture artist's "how shiny is this
//      pixel" mask) and the tint colour c100 ("EnvColour").
//   4. colour = vertex colour (only if bool b10 "HasVertexColour") x texture
//      x light + reflection + specular.
//   5. Distance fog towards the fog colour (bool b69, c70-c72).
//   6. alpha = vertex alpha (or 1) x fade (c2.x). The texture's alpha is NOT
//      part of it: it only masks the reflection.
// Numbers written out (0.5, 1) are the literal constants in the shader file.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"
#include "lighting.glsl"
#include "palette.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_texture;     // point sampler if palettized
layout(set = 0, binding = 1) uniform sampler2D u_reflection;  // the sphere map
layout(set = 0, binding = 3) uniform sampler2D u_palette;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_view;
layout(location = 3) in vec4 in_colour;
layout(location = 4) in float in_fog_depth;

layout(location = 0) out vec4 out_colour;

void main() {
  vec3 n = in_normal;

  // 1. Directional lights.
  bool any_light = (lit.counts.x & 15u) != 0u;
  vec3 diffuse = any_light ? vec3(0.0) : vec3(1.0);
  vec3 specular = vec3(0.0);
  for (uint i = 0u; i < 4u; ++i) {
    if ((lit.counts.x & (1u << i)) == 0u) {
      continue;
    }
    vec3 l = lit.light_direction[i].xyz;
    diffuse += clamp(dot(n, -l), 0.0, 1.0) * lit.light_colour[i].rgb;
    // in_view points from the camera to the surface and l the way the light
    // travels, so -(view + l) lies halfway between "to the camera" and "to
    // the light".
    vec3 h = -(in_view + l);
    float h_length2 = dot(h, h);
    h = h_length2 > 0.0 ? h * inversesqrt(h_length2) : vec3(0.0);
    specular += Power(clamp(dot(h, n), 0.0, 1.0), lit.specular_power.x) * lit.light_colour[i].rgb;
  }

  // 2. Ambient + emissive.
  vec3 light = lit.ambient.rgb * lit.surface_ambient.rgb + diffuse + lit.surface_emissive.rgb;

  // 3. The reflection, masked by the texture's alpha.
  vec4 tex = vec4(1.0);  // no texture: white (the game would sample a stale one)
  if (LitFlag(kLitTextured)) {
    tex = LitFlag(kLitPalettized)
              ? SamplePalettized(u_texture, u_palette, lit.texture_sizes.xy, in_uv)
              : texture(u_texture, in_uv);
  }
  vec2 sphere_uv = vec2(0.5 + 0.5 * n.x, 0.5 - 0.5 * n.y);
  vec3 reflection = texture(u_reflection, sphere_uv).rgb * tex.a * lit.rim_colour.rgb;

  // 4. The surface.
  vec4 vertex = LitFlag(kLitVertexColour) ? in_colour : vec4(1.0);
  vec4 colour;
  colour.rgb = vertex.rgb * tex.rgb * light + reflection + specular;

  // 5. Fog.
  if (LitFlag(kLitFog)) {
    colour.rgb = mix(lit.fog_colour.rgb, colour.rgb, FogFactor(in_fog_depth));
  }

  // 6. Fade, alpha test.
  colour.a = vertex.a * lit.fog_fade_alpha.z;
  if (LitFlag(kLitAlphaTest) && !AlphaTestPasses(colour.a)) {
    discard;
  }
  out_colour = colour;
}
