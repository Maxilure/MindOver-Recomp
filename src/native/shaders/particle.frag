// =============================================================================
// particle.frag -- the game's particle material (xnParticleShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shaders (particleshaderfp and
// palparticleshaderfp for palettized sprites; microcode only, disassembled
// with the SDK's --dump_shaders, inputs named by their constant tables;
// docs/findings/14). In order:
//
//   1. "Soft particles": a flat sprite that cuts into the ground or a wall
//      shows a hard straight edge where it meets it. So the shader looks up
//      the scene's depth under this pixel (a depth resolve the game made
//      earlier in the frame), turns both that and the sprite's own depth
//      into distances, and fades the sprite out over the last "Falloff"
//      (c4.x, 1.5 units in the game's data) before the scene:
//        scene = NearFar.y / ((1 - NearFar.x) - stored depth)
//        soft  = saturate((scene - sprite depth) / Falloff)
//      (the stored depth is the game's reversed one; the sprite's depth is
//      its clip-space z, the same distance the fog uses).
//   2. colour = texture x vertex colour; alpha = texture alpha x vertex alpha
//      x soft^2 x fade (c2.x). The game really squares `soft`.
//   3. Additive / subtractive sprites (bools b80 / b81: alpha does nothing
//      there) scale their colour by the same soft^2 x fade instead.
//   4. Distance fog (b69, c70-c72). Normal sprites blend towards the fog
//      colour; additive ones are multiplied by mix(fog colour, 1, f) x f,
//      so they fade out into the fog instead of adding it.
// Numbers written out (0.5, 1) are the literal constants in the shader file.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "particle_params.glsl"
#include "palette.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_texture;  // point sampler if palettized
layout(set = 0, binding = 1) uniform sampler2D u_depth;    // the scene's depth (R32F copy)
layout(set = 0, binding = 8) uniform sampler2D u_palette;  // the game's sampler 8 too

layout(location = 0) in vec4 in_colour;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_clip;

layout(location = 0) out vec4 out_colour;

bool AlphaTestPasses(float alpha) {
  float ref = particle.fog_colour.w;
  switch (particle.flags.y) {
    case 0u: return false;
    case 1u: return alpha < ref;
    case 2u: return alpha == ref;
    case 3u: return alpha <= ref;
    case 4u: return alpha > ref;
    case 5u: return alpha != ref;
    case 6u: return alpha >= ref;
    default: return true;
  }
}

void main() {
  // 1. Soft edge against the scene's depth.
  float soft = 1.0;
  if (ParticleFlag(kParticleSoft) && !ParticleFlag(kParticleNoSoft)) {
    // This pixel's spot on the screen, 0..1 from the top left.
    vec2 screen = vec2(0.5 + 0.5 * in_clip.x / in_clip.w, 0.5 - 0.5 * in_clip.y / in_clip.w);
    float stored = texture(u_depth, screen).r;
    vec2 near_far = particle.fade_falloff_near_far.zw;
    float scene = near_far.y / ((1.0 - near_far.x) - stored);
    soft = clamp((scene - in_clip.z) / particle.fade_falloff_near_far.y, 0.0, 1.0);
  }
  float fade = soft * soft * particle.fade_falloff_near_far.x;

  // 2. The sprite (white without a texture: the game would sample a stale one).
  vec4 tex = vec4(1.0);
  if (ParticleFlag(kParticleTextured) && !ParticleFlag(kParticleWhiteTex)) {
    tex = ParticleFlag(kParticlePalettized)
              ? SamplePalettized(u_texture, u_palette, particle.fog_range_texture_size.zw, in_uv)
              : ParticleFlag(kParticleBaseMip)     ? textureLod(u_texture, in_uv, 0.0)    // F12
              : ParticleFlag(kParticleSmallestMip) ? textureLod(u_texture, in_uv, 100.0)  // F12
                                                   : texture(u_texture, in_uv);
  }
  vec4 vertex = ParticleFlag(kParticleWhiteColour) ? vec4(1.0) : in_colour;  // F12 experiment
  vec4 colour = vec4(tex.rgb * vertex.rgb, vertex.a * tex.a * fade);

  // 3. Additive / subtractive: fade the colour itself.
  bool fade_rgb = ParticleFlag(kParticleFadeRgb);
  if (fade_rgb) {
    colour.rgb *= fade;
  }

  // 4. Fog: 1 nearer than the start, 0 beyond the end, linear in between.
  if (ParticleFlag(kParticleFog) && !ParticleFlag(kParticleNoFog)) {
    float start = particle.fog_range_texture_size.x, end = particle.fog_range_texture_size.y;
    float d = in_clip.z;
    float f = d < start ? 1.0 : d > end ? 0.0 : (end - d) / max(end - start, 1e-6);
    vec3 fog = particle.fog_colour.rgb;
    colour.rgb = fade_rgb ? colour.rgb * mix(fog, vec3(1.0), f) * f : mix(fog, colour.rgb, f);
  }

  // Debug (F11, spotter.h): a flat see-through magenta patch over the whole
  // quad, ignoring texture, fade and softness, so even a particle we draw
  // invisibly shows where it is.
  if (ParticleFlag(kParticleHighlight)) {
    colour = vec4(1.0, 0.0, 1.0, 0.5);
  }
  if (ParticleFlag(kParticleAlphaTest) && !AlphaTestPasses(colour.a)) {
    discard;
  }
  out_colour = colour;
}
