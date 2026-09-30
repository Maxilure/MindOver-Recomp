// =============================================================================
// lighting.glsl -- pieces the game's lit pixel shaders share
// =============================================================================
// Written from our reading of the game's pixel shaders (microcode
// disassembled with the SDK's --dump_shaders; docs/findings/11): the lit
// "simple" shader (simpleshaderlitfp) and the character shader
// (characterrimfp) repeat these sections word for word. Numbers written out
// here (0.3, 2, 18, 400...) are the literal constants the shader compiler
// stored inside the game's shader files.
// Needs lit_params.glsl included first.
// =============================================================================

// "Fx shadows": up to 8 points (e.g. the characters' feet) darken what is
// near them. Each darkens by up to `strength`: fully within 2 units, fading
// out linearly (then squared) over the next 18, nothing beyond 20. The
// darkest one wins. Returns the factor to multiply the colour with.
// The strength is a literal in each shader file: 0.3 in the lit ones
// (simpleshaderlitfp, characterrimfp), 0.65 in the unlit simple ones
// (simpleshaderunlitfp, palsimpleshaderunlitfp) and water.
float ShadowSpheres(vec3 world_position, float strength) {
  float shadow = 0.0;
  for (uint i = 0u; i < lit.counts.z; ++i) {
    vec3 d = world_position - lit.shadow_spheres[i].xyz;
    float distance2 = dot(d, d);
    if (distance2 > 400.0) {
      continue;
    }
    float f = max(1.0 - max(sqrt(distance2) - 2.0, 0.0) / 18.0, 0.0);
    shadow = max(shadow, strength * f * f);
  }
  return 1.0 - shadow;
}
float ShadowSpheres(vec3 world_position) {
  return ShadowSpheres(world_position, 0.3);
}

// "Proximity" point lights (up to 4, lit.counts.y of them): each brightens
// the colour near its position, in its own colour. Returns the factor to
// multiply the colour with (1 = no light). Per light: falloff = how far
// inside its radius (1 at the centre, 0 at the edge; radius clamped to
// 0.1..20), then brightness x 0.2 x falloff^2 x intensity, clamped to 1.
// The lit and unlit simple shaders do this word for word (docs/findings/20).
vec3 ProximityLights(vec3 world_position) {
  vec3 gain = vec3(1.0);
  for (uint i = 0u; i < lit.counts.y; ++i) {
    vec4 p = lit.point_params[i];
    float radius = clamp(p.y, 0.1, 20.0);
    float d = length(world_position - lit.point_position[i].xyz);
    float falloff = clamp((radius - d) / radius, 0.0, 1.0);
    gain += clamp(p.x * 0.2 * falloff * falloff * p.z, 0.0, 1.0) * lit.point_colour[i].rgb;
  }
  return gain;
}

// How much of the colour survives the distance fog at clip-space depth `d`:
// 1 nearer than the start, 0 beyond the end, linear in between.
float FogFactor(float d) {
  float start = lit.fog_fade_alpha.x, end = lit.fog_fade_alpha.y;
  if (d < start) return 1.0;
  if (d > end) return 0.0;
  return (end - d) / max(end - start, 1e-6);
}

// The GPU's alpha test, done in the shader: true = keep the pixel.
bool AlphaTestPasses(float alpha) {
  float ref = lit.fog_fade_alpha.w;
  switch (lit.more.x) {
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

// pow() as the game's shaders compute it (log, multiply, exp): 0 stays 0.
float Power(float x, float p) {
  return x > 0.0 ? exp2(p * log2(x)) : 0.0;
}
