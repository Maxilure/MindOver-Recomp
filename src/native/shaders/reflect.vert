// =============================================================================
// reflect.vert -- the game's reflect material (xnReflectShader), vertex side
// =============================================================================
// Written from what the game's vertex shader does (reflectvp; it ships with
// debug info naming its constants, docs/findings/13):
//   position     x the model-view-projection matrix (c4-c7)
//   colour, uv   passed through
//   normal       rotated into world space (c0-c3; the pixel shader uses it
//                both for the lights and to look up the reflection)
//   view         from the camera (c9, LitParams::eye) to the vertex,
//                normalized per vertex, plus the clip-space depth for fog
// The same as the lit "simple" vertex shader (lit.vert) with the vertex
// colour added and no world position.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "lit_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec3 out_view;
layout(location = 3) out vec4 out_colour;
layout(location = 4) out float out_fog_depth;  // clip-space depth before the divide

void main() {
  vec4 position = lit.mvp * vec4(in_position, 1.0);
  // D3D's clip-space Y points up, Vulkan's down.
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = LitFlag(kLitNoUv) ? vec2(0.0) : in_uv;
  out_normal = LitFlag(kLitNoNormal) ? vec3(0.0) : mat3(lit.rotation) * in_normal;
  vec3 world_position = (lit.world * vec4(in_position, 1.0)).xyz;
  out_view = normalize(world_position - lit.eye.xyz);
  // Missing vertex colour = white, what the GPU hands the game's shader.
  out_colour = LitFlag(kLitNoColour) ? vec4(1.0) : in_colour;
  out_fog_depth = position.z;
}
