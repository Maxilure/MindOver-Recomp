// =============================================================================
// lit.vert -- the game's "simple" material, lit variant, vertex side
// =============================================================================
// Written from what the game's vertex shader does (simpleshaderlitvp; it
// ships with debug info naming its constants, docs/findings/11): position
// times the model-view-projection matrix, the normal rotated into world
// space, the world position, and the direction from the camera to the
// vertex. (The game's shader also outputs a screen-space velocity for a
// motion blur, into render target 1. The Xbox game never binds that target
// and its motion blur functions are empty, so the velocity is thrown away:
// we skip it. docs/findings/21.)
// The lit variant has no vertex colours: the pixel shader lights the
// surface instead.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_normal;
// From the camera to the vertex, normalized per vertex (as the game does).
layout(location = 2) out vec3 out_view;
layout(location = 3) out vec3 out_world_position;
layout(location = 4) out float out_fog_depth;  // clip-space depth before the divide

void main() {
  vec4 position = lit.mvp * vec4(in_position, 1.0);
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = LitFlag(kLitNoUv) ? vec2(0.0) : in_uv;
  out_normal = LitFlag(kLitNoNormal) ? vec3(0.0) : mat3(lit.rotation) * in_normal;
  vec3 world_position = (lit.world * vec4(in_position, 1.0)).xyz;
  out_view = normalize(world_position - lit.eye.xyz);
  out_world_position = world_position;
  out_fog_depth = position.z;
}
