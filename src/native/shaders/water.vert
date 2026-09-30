// =============================================================================
// water.vert -- the game's water material (xnWaterSpecularShader), vertex side
// =============================================================================
// Written from the game's vertex shader (waterspecularvp; it ships with debug
// info, docs/findings/14): position x the model-view-projection matrix
// (c4-c7); colour and UV passed through; the world position (x the world
// matrix, c12-c15: the Fx shadows and the view direction need it); and the
// clip-space position once more (where the pixel is on the screen, and its
// depth). The vertices also carry a normal, which this shader ignores: the
// surface's normal comes from its normal maps.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "water_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_colour;
layout(location = 2) out vec3 out_world;  // world position
layout(location = 3) out vec4 out_clip;   // clip-space position (D3D conventions)

void main() {
  vec4 position = water.mvp * vec4(in_position, 1.0);
  // D3D's clip-space Y points up, Vulkan's down.
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = WaterFlag(kWaterNoUv) ? vec2(0.0) : in_uv;
  out_colour = WaterFlag(kWaterNoColour) ? vec4(1.0) : in_colour;
  out_world = (water.world * vec4(in_position, 1.0)).xyz;
  out_clip = position;
}
