// =============================================================================
// palette.glsl -- sampling an 8-bit palettized texture, the way the game does
// =============================================================================
// A palettized texture stores 8 bits per texel, each an index into a
// 256-colour palette (a 256x1 texture). Filtering the indices would blend
// unrelated palette entries, so like the game's pixel shaders we fetch the 4
// texels around the sample point with point sampling (`indices` must use a
// NEAREST sampler), look each one up, and blend the 4 colours ourselves.
// Used by simple.frag (menus, HUD, world), character.frag (diffuse and
// normal map) and lit.frag.
// =============================================================================

vec4 PaletteColour(sampler2D indices, sampler2D palette, vec2 texel_centre) {
  // R8 index 0..1 -> palette entry 0..255.
  float index = texture(indices, texel_centre).r;
  return texelFetch(palette, ivec2(int(index * 255.0 + 0.5), 0), 0);
}

// `size`: the index texture's size in texels.
vec4 SamplePalettized(sampler2D indices, sampler2D palette, vec2 size, vec2 uv) {
  vec2 texel = uv * size - 0.5;  // texel coordinates of the sample point
  vec2 base = floor(texel);
  vec2 weight = texel - base;
  // Texel centres of the 4 neighbours; the sampler's address mode wraps or
  // clamps them like the GPU would.
  vec2 c00 = (base + 0.5) / size;
  vec2 step = 1.0 / size;
  vec4 p00 = PaletteColour(indices, palette, c00);
  vec4 p10 = PaletteColour(indices, palette, c00 + vec2(step.x, 0.0));
  vec4 p01 = PaletteColour(indices, palette, c00 + vec2(0.0, step.y));
  vec4 p11 = PaletteColour(indices, palette, c00 + step);
  return mix(mix(p00, p10, weight.x), mix(p01, p11, weight.x), weight.y);
}
