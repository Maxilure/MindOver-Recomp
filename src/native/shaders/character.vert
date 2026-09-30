// =============================================================================
// character.vert -- the game's character material (xnCharacterRimShader), vertex side
// =============================================================================
// Written from what the game's vertex shader does (characterrimvp; it ships
// with debug info naming its constants, docs/findings/11): position times
// the model-view-projection matrix; the world position; the normal, tangent
// and binormal rotated into world space (for the pixel shader's normal map
// and lighting). The vertices come already skinned (posed) by the game's
// CPU, so there are no bones here.
//
// Clip-space Y points up in D3D and down in Vulkan: flipped here (like
// simple.vert).
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec3 in_binormal;
layout(location = 5) in vec3 in_tangent;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec3 out_tangent;
layout(location = 3) out vec3 out_binormal;
layout(location = 4) out vec3 out_world_position;
// Clip-space depth before the divide, for the fog (the game passes the
// whole clip position; the fog only uses z).
layout(location = 5) out float out_fog_depth;

void main() {
  vec4 position = lit.mvp * vec4(in_position, 1.0);
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = LitFlag(kLitNoUv) ? vec2(0.0) : in_uv;
  mat3 rotation = mat3(lit.rotation);
  out_normal = LitFlag(kLitNoNormal) ? vec3(0.0, 0.0, 1.0) : rotation * in_normal;
  // Without tangents the normal map can't be oriented: its "x" and "y" then
  // simply add nothing sensible, so use any perpendicular pair.
  out_tangent = LitFlag(kLitNoTangents) ? vec3(1.0, 0.0, 0.0) : rotation * in_tangent;
  out_binormal = LitFlag(kLitNoTangents) ? vec3(0.0, 1.0, 0.0) : rotation * in_binormal;
  out_world_position = (lit.world * vec4(in_position, 1.0)).xyz;
  out_fog_depth = position.z;
}
