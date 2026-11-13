// =============================================================================
// bink.frag -- the game's movie material (xnBinkShader), pixel side
// =============================================================================
// Bink (the movie codec) decodes each frame into three 8-bit images: Y
// (brightness, full size) and the colour differences Cr and Cb (half size).
// The game's pixel shader "binkdecompress" (microcode read with the SDK's
// --dump_shaders, docs/findings/09) turns them into RGB with a matrix in its
// constants c0..c2:
//
//   rgb[i] = dot(vec3(Y, Cr, Cb), c[i].xyz) + c[i].w * c3.x,   alpha = c3.w
//
// The game loads the standard video (BT.601) numbers there; we get them from
// the game each frame instead of writing them down, so nothing is guessed.
//
// MOVIE BACKDROP (screen_size.w = 1, bink.vert): the same frame zoomed over
// the whole wide picture, BLURRED (a spiral of taps over a wide radius),
// DARKENED, and darker still right next to the movie's edges (a soft shadow
// that frames the sharp movie drawn on top of it).
// =============================================================================

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_y;
layout(set = 0, binding = 1) uniform sampler2D u_cr;
layout(set = 0, binding = 2) uniform sampler2D u_cb;

// Pixel shader c0..c3 of the game's movie shader, and the screen size used
// by bink.vert (BinkParams in native_renderer.cpp).
layout(push_constant) uniform BinkParams {
  vec4 c[4];
  vec4 screen_size;
} params;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in float in_clip_x;
layout(location = 0) out vec4 out_colour;

// One movie pixel, the game's way (above).
vec3 MovieRgb(vec2 uv) {
  vec3 ycrcb = vec3(texture(u_y, uv).r, texture(u_cr, uv).r, texture(u_cb, uv).r);
  float k = params.c[3].x;
  return vec3(dot(ycrcb, params.c[0].xyz) + params.c[0].w * k,
              dot(ycrcb, params.c[1].xyz) + params.c[1].w * k,
              dot(ycrcb, params.c[2].xyz) + params.c[2].w * k);
}

// Backdrop look (tuning knobs).
const int kTaps = 48;            // spiral taps (golden angle)
const float kRadius = 0.07;      // blur radius in movie widths
const float kBrightness = 0.45;  // the backdrop's brightness
const float kShadowWidth = 0.12; // clip units beside the movie's edge
const float kShadowDark = 0.35;  // brightness right at the edge (x kBrightness)

void main() {
  if (params.screen_size.w < 0.5) {
    out_colour = vec4(MovieRgb(in_uv), params.c[3].w);
    return;
  }
  // Blur: taps on a golden-angle spiral, sqrt spacing = even coverage; the
  // radius is kept round on screen (v scaled by the movie's 16:9).
  vec3 sum = vec3(0.0);
  for (int i = 0; i < kTaps; ++i) {
    float r = kRadius * sqrt((float(i) + 0.5) / float(kTaps));
    float a = float(i) * 2.39996323;
    vec2 uv = in_uv + vec2(cos(a), sin(a) * (16.0 / 9.0)) * r;
    sum += MovieRgb(clamp(uv, vec2(0.0), vec2(1.0)));
  }
  vec3 colour = sum / float(kTaps) * kBrightness;
  // Framing: darker toward the movie's edge (|x| = 1 / wide in clip units).
  float edge = abs(in_clip_x) - 1.0 / params.screen_size.z;
  colour *= mix(kShadowDark, 1.0, smoothstep(0.0, kShadowWidth, edge));
  out_colour = vec4(colour, 1.0);
}
