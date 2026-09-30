// =============================================================================
// bumpmega.vert -- the game's bump mega material (xnBumpMegaShader), vertex side
// =============================================================================
// Written from the game's own vertex shader, bumpmegavp (it ships with debug
// info: docs/findings/19): the clip position (matMVP), the world position
// (matWorld), the normal, tangent and binormal turned into world directions
// (matRot's 3x3 part), colour and texture coordinates passed on, and the
// clip position once more for the pixel shader's screen-space lookups.
//
// D3D's clip-space Y points up, Vulkan's down: flipped for gl_Position only
// (the pixel shader works with the D3D one, like the game's).
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "bumpmega_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec3 in_binormal;
layout(location = 5) in vec3 in_tangent;

layout(location = 0) out vec4 out_colour;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec3 out_normal;    // world directions, not normalized
layout(location = 3) out vec3 out_tangent;
layout(location = 4) out vec3 out_binormal;
layout(location = 5) out vec3 out_world;     // world position
layout(location = 6) out vec4 out_clip;      // clip-space position (D3D conventions)

void main() {
  vec4 position = vec4(in_position, 1.0);
  vec4 clip = bump.mvp * position;
  gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);
  out_clip = clip;
  out_world = (bump.world * position).xyz;
  mat3 rotation = mat3(bump.rotation);
  out_normal = rotation * in_normal;
  out_tangent = rotation * in_tangent;
  out_binormal = rotation * in_binormal;
  out_colour = BumpFlag(kBumpMegaNoColour) ? vec4(1.0) : in_colour;
  out_uv = BumpFlag(kBumpMegaNoUv) ? vec2(0.0) : in_uv;
}
