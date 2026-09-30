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
layout(location = 0) out vec4 out_colour;

void main() {
  vec3 ycrcb = vec3(texture(u_y, in_uv).r, texture(u_cr, in_uv).r, texture(u_cb, in_uv).r);
  float k = params.c[3].x;
  out_colour = vec4(dot(ycrcb, params.c[0].xyz) + params.c[0].w * k,
                    dot(ycrcb, params.c[1].xyz) + params.c[1].w * k,
                    dot(ycrcb, params.c[2].xyz) + params.c[2].w * k, params.c[3].w);
}
