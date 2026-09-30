// =============================================================================
// bumpmega.frag -- the game's bump mega material (xnBumpMegaShader), pixel side
// =============================================================================
// The "do everything" normal-mapped surface: the TK blocks and see-through
// ice, among others (docs/findings/20). Written from the game's
// pixel shader, bumpmegafp: microcode only (disassembled with the SDK's
// --dump_shaders), names from its constant table (tools/shader_constants.py),
// its literal constants from the .out file (c251-c255). Per pixel:
//
//   1. The texture colour (palettized or not) times the vertex colour. No
//      lights: the brightness is baked into the vertex colours.
//   2. The normal map tilts the surface direction: n = 2 x texel - 1, in the
//      space of the tangent, binormal and normal (all normalized first).
//   3. Fx shadows: up to 8 points darken what's near them, up to 30%.
//   4. A "shine": four fixed directions reflected off the bumpy surface
//      towards the camera, each pow(., 30), together x 0.1. The glint of
//      crystal and ice surfaces.
//      colour = texture x vertex colour x (1 - shadow) + shine
//   5. Reflection (bool b12): a picture sampled at the pixel's SCREEN
//      position, pushed by the bumps (x ReflectBumpiness / 2), tinted by
//      ReflectColor, its strength (alpha) masked by the ReflectMask texture
//      (b13, same position) or else by the normal map's alpha:
//      colour = mix(colour, reflection, strength).
//   6. See-through (b11): the picture so far (a copy resolved just before)
//      at the screen position pushed by the bumps (x RefractBumpiness), times
//      RefractBrightness (and times the colour with b16); then
//      colour = mix(that, colour, alpha) and the pixel becomes opaque. The
//      sideways push fades out near multiples of TileWidth: the Xbox draws
//      the screen in strips, and its copy was made strip by strip, so the
//      game never samples across a strip's edge. Reproduced as is.
//   7. Rim glow (b10): + RimColor x (1 - |N.V|)^3.
//   8. Distance fog (b69), then alpha = vertex alpha x texture alpha x Fade.
//
// The parts where the compiler's arithmetic is hard to follow (the normal
// perturbation, the four shine directions, the strip fade) are written out
// step by step after the microcode, register names kept in the comments, so
// they can be checked line by line against the disassembly.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "bumpmega_params.glsl"

// The game's samplers (guest.h). A palettized texture's indices are bound
// with a point sampler; the palettes are 256x1.
layout(set = 0, binding = 0) uniform sampler2D u_diffuse;     // s0 DiffuseTex
layout(set = 0, binding = 1) uniform sampler2D u_normal_map;  // s1 NormalMap
layout(set = 0, binding = 2) uniform sampler2D u_reflect;     // s2 ReflectMap
layout(set = 0, binding = 3) uniform sampler2D u_mask;        // s3 ReflectMask
layout(set = 0, binding = 4) uniform sampler2D u_background;  // s4 BackGroundMap
layout(set = 0, binding = 7) uniform sampler2D u_palette;     // s7 Palette (diffuse)
layout(set = 0, binding = 8) uniform sampler2D u_palette2;    // s8 Palette2 (normal map)

layout(location = 0) in vec4 in_colour;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec3 in_tangent;
layout(location = 4) in vec3 in_binormal;
layout(location = 5) in vec3 in_world;
layout(location = 6) in vec4 in_clip;

layout(location = 0) out vec4 out_colour;

// --- Palettized textures, the way THIS shader filters them ------------------
// Not palette.glsl's way: this shader blends the 4 texels at
// floor(uv x (size - 1)) and one texel right / down, weighted by the
// fraction of uv x (size - 1) (texel corners at 0 and 1, where the usual
// bilinear convention puts texel centres at 0.5 / size). Its microcode
// (the b142 / b143 blocks): inside 0..1 it reads the texel at that corner
// and the next ones (OffsetX / OffsetY = 1); outside it reads at uv and one
// texel on, letting the sampler's address mode wrap. Each index is looked up
// at index x 255/256 + 1/512 in the palette = entry round(index x 255).
vec4 PaletteEntry(sampler2D palette, float index) {
  return texelFetch(palette, ivec2(int(index * 255.0 + 0.5), 0), 0);
}

vec4 SamplePalettizedBump(sampler2D indices, sampler2D palette, vec2 size, vec2 uv) {
  vec2 texel = uv * (size - 1.0);
  vec2 corner = floor(texel);
  vec2 weight = texel - corner;
  bool inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
  // The game reads at corner / size, a texel's edge, where point sampling
  // gives that texel; its centre is the same texel without rounding doubts.
  vec2 at = inside ? (corner + 0.5) / size : uv;
  vec2 step = 1.0 / size;
  vec4 p00 = PaletteEntry(palette, texture(indices, at).r);
  vec4 p10 = PaletteEntry(palette, texture(indices, at + vec2(step.x, 0.0)).r);
  vec4 p01 = PaletteEntry(palette, texture(indices, at + vec2(0.0, step.y)).r);
  vec4 p11 = PaletteEntry(palette, texture(indices, at + step).r);
  return mix(mix(p00, p10, weight.x), mix(p01, p11, weight.x), weight.y);
}

// --- The four-way shine (step 4), as the microcode computes it --------------
// p = the bumped surface direction, v = towards the camera (both unit
// length). The shader keeps both in (z, y, x) order: r9 = p.zyx and
// r6 = (v.z, v.y, -, v.x); a = 0.7071 (c255.x, and -a = c255.y).
// Instruction numbers are the disassembly's.
float Shine(vec3 p, vec3 v) {
  const float a = 0.7071;
  vec3 r9 = p.zyx;
  vec4 r6 = vec4(v.z, v.y, 0.0, v.x);
  float r3y = dot(vec4(r9.z, r9.z, r9.y, r9.y), vec4(a, a, -a, -a));  // 246
  float r3x = dot(vec4(r9.z, r9.z, r9.y, r9.y), vec4(-a));              // 247
  float r0y = r9.x * a, r0z = r9.x * a, r0w = r9.y * -a;                // 248
  float r3z = r0w + r0z, r3w = r0w + r0y;                               // 249
  float r7x = -r9.y * r3x - a, r7y = -r9.y * r3y - a;                   // 250
  float r5z = r9.y * -a + r9.x * a + r3w;                               // 251
  r0y = r9.y * -a + r9.x * -a + r3z;                                    // 252
  // 253: r3 = -r9.xzxz * r3.xyyx
  float r3x2 = -r9.x * r3x, r3y2 = -r9.z * r3y, r3z2 = -r9.x * r3y, r3w2 = -r9.z * r3x;
  float r5x = r3x2 * r6.x, r5y = r3z2 * r6.x;                           // 254
  float r0y2 = r9.x * r0y, r0z2 = r9.y * r0y, r0w2 = r9.z * r0y;        // 255
  float r7z = -a + r3w2;                                                // 255 (scalar)
  vec3 r8 = -r9 * r5z + vec3(a, -a, 0.0);                               // 256
  float s1 = clamp(r6.y * r8.y + r6.x * r8.x + r6.w * r8.z, 0.0, 1.0);  // 257
  float r7w = a + r3y2;                                                 // 257 (scalar)
  float r0w3 = -r0w2;                                                   // 258
  float s2 = clamp(r6.w * r7w + r6.y * r7y + r5y, 0.0, 1.0);            // 259
  float s3 = clamp(r6.w * r7z + r6.y * r7x + r5x, 0.0, 1.0);            // 260
  float r0y3 = -r0y2 - a, r0z3 = -r0z2 - a;                             // 261
  float s4 = clamp(r6.y * r0z3 + r6.x * r0y3 + r6.w * r0w3, 0.0, 1.0);  // 262
  // 263-269: log, x 30 (c254.z), exp: each to the power 30, summed x 0.1.
  return 0.1 * (pow(s1, 30.0) + pow(s2, 30.0) + pow(s3, 30.0) + pow(s4, 30.0));
}

// The GPU's alpha test (the state cache's function and reference): keep?
bool AlphaTestPasses(float alpha) {
  float ref = bump.fog_fade.w;
  switch (bump.flags.z) {
    case 0u: return false;
    case 1u: return alpha < ref;
    case 2u: return alpha == ref;
    case 3u: return alpha <= ref;
    case 4u: return alpha > ref;
    case 5u: return alpha != ref;
    case 6u: return alpha >= ref;
    default: return true;
  }
}

void main() {
  // The pixel's place on screen, 0..1 from the top left, as the shader works
  // it out from the clip position (instructions 38-43): the see-through and
  // the reflection pictures are sampled there.
  float inv_w = 1.0 / in_clip.w;
  vec2 screen = vec2(0.5 * in_clip.x * inv_w + 0.5, 1.0 - (0.5 * in_clip.y * inv_w + 0.5));
  // Towards the camera (40-44).
  vec3 v = normalize(bump.camera.xyz - in_world);

  // 2. The normal map (b143 palettized, else a plain lookup), and the
  //    bumped direction p = T n.x + B n.y + N n.z (82-98).
  vec4 normal_texel = BumpFlag(kBumpMegaNoNormalMap) ? vec4(0.5, 0.5, 1.0, 1.0)
                      : BumpFlag(kBumpMegaPalette2)
                          ? SamplePalettizedBump(u_normal_map, u_palette2, bump.palette_sizes.zw, in_uv)
                          : texture(u_normal_map, in_uv);
  vec3 n = 2.0 * normal_texel.xyz - 1.0;  // the shader: (texel - 1) + texel
  vec3 t = normalize(in_tangent);
  vec3 b = normalize(in_binormal);
  vec3 N = normalize(in_normal);
  vec3 p = normalize(t * n.x + b * n.y + N * n.z);

  // 5. Reflection (b12, 99-106): at the screen position pushed by the bumps.
  vec4 reflection = vec4(0.0);
  if (BumpFlag(kBumpMegaReflect)) {
    vec2 at = screen + 0.5 * bump.bump.y * n.xy;
    reflection = texture(u_reflect, at) * bump.reflect_colour;
    reflection.a *= BumpFlag(kBumpMegaReflectMask) ? texture(u_mask, at).r : normal_texel.a;
  }

  // 6. See-through (b11, 108-117): the picture behind, pushed by the bumps;
  //    the sideways push fades out within RefractBumpiness + 0.025 of each
  //    multiple of TileWidth (a strip edge of the Xbox's tiled copy).
  vec3 behind = vec3(0.0);
  if (BumpFlag(kBumpMegaRefract)) {
    float k = bump.bump.x + 0.025;
    float tile = bump.bump.z;
    float fade_x = 1.0;
    if (tile > 0.0) {
      float from_edge = screen.x - floor(screen.x * (1.0 / tile) + 0.5) * tile;  // 109-111
      fade_x = clamp(clamp(abs(from_edge) - k, 0.0, 1.0) * (1.0 / k), 0.0, 1.0);  // 112-113
    }
    vec2 at = screen + bump.bump.x * vec2(n.x * fade_x, n.y);                  // 113-115
    behind = texture(u_background, at).rgb * bump.bump.w;                      // 116-117
  }

  // 1. The texture colour x the vertex colour (b142 palettized, 119-157).
  vec4 diffuse = BumpFlag(kBumpMegaPalette)
                     ? SamplePalettizedBump(u_diffuse, u_palette, bump.palette_sizes.xy, in_uv)
                     : texture(u_diffuse, in_uv);
  vec4 surface = diffuse * in_colour;

  // 3. Fx shadows (158-245): the darkest of up to 8.
  float shadow = 0.0;
  for (uint i = 0u; i < bump.flags.y; ++i) {
    vec3 d = in_world - bump.shadow_spheres[i].xyz;
    float distance2 = dot(d, d);
    if (distance2 > 400.0) {
      continue;
    }
    float f = max(1.0 - max(sqrt(distance2) - 2.0, 0.0) / 18.0, 0.0);
    shadow = max(shadow, 0.3 * f * f);
  }

  // 4. + the shine (246-270).
  vec3 colour = surface.rgb * (1.0 - shadow) + Shine(p, v);
  float alpha = surface.a;
  // 5. (271-272)
  if (BumpFlag(kBumpMegaReflect)) {
    colour = mix(colour, reflection.rgb, reflection.a);
  }
  // 6. (273-276)
  if (BumpFlag(kBumpMegaRefract)) {
    if (BumpFlag(kBumpMegaRefractModulated)) {
      behind *= colour;
    }
    colour = mix(behind, colour, alpha);
    alpha = 1.0;
  }
  // 7. Rim glow (b10, 277-281).
  if (BumpFlag(kBumpMegaRim)) {
    float edge = 1.0 - abs(dot(p, v));
    colour += edge * edge * edge * bump.rim_colour.rgb;
  }
  // 8. Distance fog (b69, 282-289) by the clip-space depth, like the other
  //    materials; alpha x Fade (290).
  if (BumpFlag(kBumpMegaFog)) {
    float start = bump.fog_fade.x, end = bump.fog_fade.y;
    float z = in_clip.z;
    float f = z < start ? 1.0 : z > end ? 0.0 : (end - z) / max(end - start, 1e-6);
    colour = mix(bump.fog_colour.rgb, colour, f);
  }
  alpha *= bump.fog_fade.z;
  if (!AlphaTestPasses(alpha)) {
    discard;
  }
  // Debug (F11, spotter.h): paint the material magenta so it can be found
  // on screen. Mostly magenta, a little shading left to see its shape.
  if (BumpFlag(kBumpMegaHighlight)) {
    colour = mix(colour, vec3(1.0, 0.0, 1.0), 0.75);
  }
  out_colour = vec4(colour, alpha);
}
