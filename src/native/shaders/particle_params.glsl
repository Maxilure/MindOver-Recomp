// =============================================================================
// particle_params.glsl -- per-draw parameters of the particle material
// =============================================================================
// Shared by particle.vert and particle.frag. The same block as
// native::ParticleParams in src/native/frame.h (read that for where each
// value comes from in the game): vec4s and a mat4 only, so std140 lays it
// out exactly like the C++ struct. One 1024-byte block per draw.
// =============================================================================

layout(set = 0, binding = 9, std140) uniform ParticleParams {
  mat4 mvp;                     // model-view-projection (D3D conventions)
  vec4 fade_falloff_near_far;   // x fade, y falloff, zw NearFar
  vec4 fog_colour;              // rgb fog colour, w alpha test reference
  vec4 fog_range_texture_size;  // x fog start, y fog end, zw texture size (texels)
  uvec4 flags;                  // x kParticle* below, y alpha test compare function
} particle;

// flags.x (frame.h, kParticle*).
const uint kParticleTextured = 1u;
const uint kParticlePalettized = 2u;
const uint kParticleFadeRgb = 4u;    // additive / subtractive blending
const uint kParticleFog = 8u;
const uint kParticleAlphaTest = 16u;
const uint kParticleNoColour = 32u;
const uint kParticleNoUv = 64u;
const uint kParticleSoft = 128u;     // the scene's depth is bound
const uint kParticleHighlight = 256u;  // debug: F11 (spotter.h)
// Debug experiments (F12, spotter.h): skip one step.
const uint kParticleNoSoft = 512u;
const uint kParticleNoFog = 1024u;
const uint kParticleWhiteTex = 2048u;
const uint kParticleWhiteColour = 4096u;
const uint kParticleBaseMip = 8192u;
const uint kParticleSmallestMip = 16384u;

bool ParticleFlag(uint flag) { return (particle.flags.x & flag) != 0u; }
