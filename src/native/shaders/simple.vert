// =============================================================================
// simple.vert -- the game's "simple" material (xnSimpleShader), vertex side
// =============================================================================
// Written from what the game's own vertex shader does (it ships with debug
// info naming its constants, see docs/findings/09): position times the
// model-view-projection matrix, vertex colour times a tint colour, texture
// coordinates passed through. For the proximity lights and Fx shadows
// (simple.frag) also the world position: model position times the world
// matrix (the game's c12-c15, "matWorld"), from the draw's LitParams block.
//
// Clip-space Y points up in D3D and down in Vulkan: flipped here. Depth
// (0..1) is the same in both.
//
// Pixel centres: D3D9 puts them at whole coordinates, Vulkan at +0.5, so we
// first shifted everything by half a pixel. Comparing screenshots with the
// emulated picture showed that made it worse (half a pixel right/down of
// the emulated one; without the shift they agree within a quarter pixel):
// the Xbox 360's D3D can use the +0.5 convention too (its half-pixel-offset
// state), and this game's sprites are placed for it. So: no shift.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "simple_params.glsl"
#include "lit_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 out_colour;
layout(location = 1) out vec2 out_uv;
// The fog distance: clip-space depth before the divide (the game's vertex
// shader passes Out.position.z for this).
layout(location = 2) out float out_fog_depth;
layout(location = 3) out vec3 out_world_position;  // only with kFlagLights

void main() {
  vec4 position = params.mvp * vec4(in_position, 1.0);
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  // Meshes without vertex colours are white (what the game's shader gets
  // from the GPU for a missing input), without UVs sample texel (0, 0).
  vec4 colour = (params.flags.x & kFlagNoColour) != 0u ? vec4(1.0) : in_colour;
  out_colour = vec4(colour.rgb * params.tint_fade.rgb, colour.a);
  out_uv = (params.flags.x & kFlagNoUv) != 0u ? vec2(0.0) : in_uv;
  out_fog_depth = position.z;
  out_world_position = (params.flags.x & kFlagLights) != 0u
                           ? (lit.world * vec4(in_position, 1.0)).xyz
                           : vec3(0.0);
}
