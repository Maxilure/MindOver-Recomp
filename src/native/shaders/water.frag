// =============================================================================
// water.frag -- the game's water material (xnWaterSpecularShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shader (waterspecularfp;
// microcode only, disassembled with the SDK's --dump_shaders; its constant
// table names every input; docs/findings/14). The Xbox has no way to see
// "through" a surface while drawing it, so the game copies the picture drawn
// so far (BackGroundMap) and its depth (DepthMap) just before each water
// draw, and the water paints a wobbly view of that copy. In order:
//
//   1. Where this pixel is on the screen (0..1 from the top left), and two
//      wobble vectors from the DistortMap: one sampled at the water's UV
//      (moves with the water), one at the screen position.
//   2. Reflection (unless bool b10 "IsDisableReflection"): the planar
//      reflection picture (ReflectMap, rendered by the game first) at the
//      screen position pushed by the first wobble x ReflectBumpiness / 2,
//      tinted by ReflectColor; its strength = its alpha x ReflectColor's
//      alpha x the ReflectMask (sampled at the same spot).
//   3. Refraction: the scene copy, pushed down/up by the wobble's y x
//      ReflectBumpiness / 4. Unless that would pull in something standing IN
//      FRONT of the water: the depth copy is read under this pixel and at the
//      pushed spot (and one pixel above it); both are turned into distances
//      behind the water's surface (NearFar, like the particles). If a pushed
//      spot lies nearer than -half the water's local depth, no push.
//   4. The water's own colour: DiffuseTex at the UV pushed by the second
//      wobble x WaterBumpiness / 2, x vertex colour; darkened by the "Fx
//      shadows" (up to 8 points, e.g. under Crash: up to 65% within 2 units,
//      fading out over 18; this material's own numbers).
//   5. A "specular" glint: the normal = the NormalMap (at the pushed UV) +
//      the animated normal map (two frames of a 15-frame loop, NormalTex0/1,
//      mixed by Blend) - 1, normalized. A light from the camera's horizontal
//      direction (-ViewDirection.x, -ViewDirection.z) and a little down
//      (-0.5) is mirrored about it, and the glint = saturate(that . the
//      direction to the camera) x 0.2. The maps' blue channel is "up": the
//      comparison with the world direction swaps y and z.
//   6. colour = diffuse x (1 - shadow) + glint, then mixed towards the
//      reflection by its strength, then drawn OVER the scene copy by the
//      diffuse alpha (so the water blends itself; its own output alpha is
//      just the fade, c2.x).
//   7. Distance fog (b69, c70-c72), as the other materials.
// Numbers written out (0.5, 0.25, 0.2, 0.65, 2, 18, 400, -0.5) are the
// literal constants in the shader file.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "water_params.glsl"

// Bindings = the game's samplers (guest.h, kWaterSpecularShaderVtable).
layout(set = 0, binding = 0) uniform sampler2D u_diffuse;       // DiffuseTex
layout(set = 0, binding = 1) uniform sampler2D u_distort;       // DistortMap
layout(set = 0, binding = 2) uniform sampler2D u_background;    // BackGroundMap (scene copy)
layout(set = 0, binding = 3) uniform sampler2D u_depth;         // DepthMap (R32F copy)
layout(set = 0, binding = 4) uniform sampler2D u_normal_map;    // NormalMap
layout(set = 0, binding = 5) uniform sampler2D u_normal_0;      // NormalTex0 (frame i)
layout(set = 0, binding = 6) uniform sampler2D u_normal_1;      // NormalTex1 (frame i + 1)
layout(set = 0, binding = 7) uniform sampler2D u_reflect_map;   // ReflectMap
layout(set = 0, binding = 8) uniform sampler2D u_reflect_mask;  // ReflectMask

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_colour;
layout(location = 2) in vec3 in_world;
layout(location = 3) in vec4 in_clip;

layout(location = 0) out vec4 out_colour;

// How far behind the water's surface the scene is at screen position `at`
// (negative: in front of it). The depth copy holds the game's reversed
// depth; NearFar turns it into the same kind of distance as the clip-space z.
float BehindSurface(vec2 at) {
  float stored = texture(u_depth, at).r;
  return water.near_far.y / ((1.0 - water.near_far.x) - stored) - in_clip.z;
}

// This material's Fx shadows: how much darker (0..0.65).
float FxShadow() {
  float shadow = 0.0;
  for (uint i = 0u; i < water.flags.y; ++i) {
    vec3 d = in_world - water.shadow_spheres[i].xyz;
    float distance2 = dot(d, d);
    if (distance2 > 400.0) {
      continue;
    }
    float f = max(1.0 - max(sqrt(distance2) - 2.0, 0.0) / 18.0, 0.0);
    shadow = max(shadow, 0.65 * f * f);
  }
  return shadow;
}

void main() {
  float reflect_bumpiness = water.bumpiness_blend_pixel.x;
  float water_bumpiness = water.bumpiness_blend_pixel.y;

  // 1. Screen position, wobbles.
  vec2 screen = vec2(0.5 + 0.5 * in_clip.x / in_clip.w, 0.5 - 0.5 * in_clip.y / in_clip.w);
  vec2 wobble_uv = 2.0 * texture(u_distort, in_uv).ab - 1.0;
  vec2 wobble_screen = 2.0 * texture(u_distort, screen).rg - 1.0;

  // 2. Reflection.
  vec4 reflection = vec4(0.0);
  if (WaterFlag(kWaterReflection)) {
    vec2 at = screen + wobble_uv * (reflect_bumpiness * 0.5);
    float mask = texture(u_reflect_mask, at).r;
    reflection = texture(u_reflect_map, at) * water.reflect_colour;
    reflection.a *= mask;
  }

  // 3. Refraction: the scene copy, pushed vertically unless that reaches
  //    something in front of the water.
  float push = reflect_bumpiness * 0.25 * wobble_uv.y;
  vec2 pushed = vec2(screen.x, screen.y + push);
  vec2 pushed_above = vec2(screen.x, screen.y + push - water.bumpiness_blend_pixel.w);
  float limit = -0.5 * BehindSurface(screen);
  if (limit > BehindSurface(pushed) || limit > BehindSurface(pushed_above)) {
    pushed.y = screen.y;
  }
  vec3 background = texture(u_background, pushed).rgb;

  // 4. The water's own colour.
  vec2 diffuse_uv = in_uv + wobble_screen * (water_bumpiness * 0.5);
  vec4 diffuse = texture(u_diffuse, diffuse_uv) * in_colour;
  float shadow = FxShadow();

  // 5. The glint. Normals in the maps' own axes (x, y, z = red, green, blue).
  vec3 animated = mix(texture(u_normal_0, in_uv).rgb, texture(u_normal_1, in_uv).rgb,
                      water.bumpiness_blend_pixel.z);
  vec3 n = normalize(animated + texture(u_normal_map, diffuse_uv).rgb - 1.0);
  vec3 l = normalize(vec3(-water.view_direction.x, -water.view_direction.z, -0.5));
  vec3 r = l - 2.0 * dot(n, l) * n;
  vec3 to_camera = normalize(water.camera.xyz - in_world);
  float glint = clamp(clamp(dot(to_camera, r.xzy), 0.0, 1.0) * 0.2, 0.0, 1.0);

  // 6. Combine, reflect, draw over the scene copy.
  vec3 colour = diffuse.rgb * (1.0 - shadow) + glint;
  colour = mix(colour, reflection.rgb, reflection.a);
  colour = mix(background, colour, diffuse.a);

  // 7. Fog.
  if (WaterFlag(kWaterFog)) {
    float start = water.fog_fade.x, end = water.fog_fade.y, d = in_clip.z;
    float f = d < start ? 1.0 : d > end ? 0.0 : (end - d) / max(end - start, 1e-6);
    colour = mix(water.fog_colour.rgb, colour, f);
  }
  out_colour = vec4(colour, water.fog_fade.z);
}
