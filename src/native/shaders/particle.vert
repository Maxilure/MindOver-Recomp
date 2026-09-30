// =============================================================================
// particle.vert -- the game's particle material (xnParticleShader), vertex side
// =============================================================================
// Written from the game's vertex shader (particleshadervp; it ships with
// debug info, docs/findings/14): position x the model-view-projection matrix
// (c4-c7); colour and UV passed through; and the clip-space position once
// more, for the pixel shader to find its spot on the screen (where it reads
// the scene's depth) and its own depth.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "particle_params.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 out_colour;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec4 out_clip;  // clip-space position (D3D conventions)

void main() {
  vec4 position = particle.mvp * vec4(in_position, 1.0);
  // D3D's clip-space Y points up, Vulkan's down.
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  // Missing components: white / texel (0, 0), what the GPU hands the game's shader.
  out_colour = ParticleFlag(kParticleNoColour) ? vec4(1.0) : in_colour;
  out_uv = ParticleFlag(kParticleNoUv) ? vec2(0.0) : in_uv;
  out_clip = position;
}
