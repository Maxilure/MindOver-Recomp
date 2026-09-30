// =============================================================================
// dof.vert -- the game's depth of field pass (xnDOFShader), vertex side
// =============================================================================
// Written from the game's vertex shader (DOFvp; it ships with debug info,
// docs/findings/15): position x the model-view-projection matrix (c4-c7), the
// UV passed through, and the clip-space position once more, for the pixel
// shader to find its spot on the screen. The game draws one quad covering
// the screen with it, UV 0..1.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "dof_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_clip;  // clip-space position (D3D conventions)

void main() {
  vec4 position = dof.mvp * vec4(in_position, 1.0);
  // D3D's clip-space Y points up, Vulkan's down.
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = in_uv;
  out_clip = position;
}
