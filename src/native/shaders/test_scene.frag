// =============================================================================
// test_scene.frag -- milestone 2 test picture: gradient + checkerboard
// =============================================================================
#version 450

layout(push_constant) uniform Params {
  vec4 params;
  vec4 tint;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  // Crash orange at the top, sky blue at the bottom, 8x8 checkerboard on top.
  vec3 color = mix(vec3(1.0, 0.55, 0.1), vec3(0.2, 0.6, 1.0), v_uv.y);
  float checker = mod(floor(v_uv.x * 8.0) + floor(v_uv.y * 8.0), 2.0);
  o_color = vec4(color * (0.8 + 0.2 * checker) * pc.tint.rgb, 1.0);
}
