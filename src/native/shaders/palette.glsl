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
//
// Two mappings of uv to texels, because the game's shaders differ:
// SamplePalettized puts texel CENTRES at (i + 0.5) / size (the usual GPU
// bilinear); SamplePalettizedCorner follows the shaders that filter at
// uv x (size - 1) (texel corners at 0 and 1): bumpmegafp and
// palparticleshaderfp (literals c254 = (-1, 255/256, ..), docs/findings/43;
// bumpmega.frag keeps its own copy, SamplePalettizedBump).
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

// The CORNER mapping (bumpmegafp, palparticleshaderfp): the 4 texels at
// floor(uv x (size - 1)) and one texel right / down, weighted by the
// fraction of uv x (size - 1). Their microcode: inside 0..1 it reads the
// texel at that corner and the next ones (OffsetX / OffsetY = 1); outside it
// reads at uv and one texel on, letting the sampler's address mode wrap.
// Each index is looked up at index x 255/256 + 1/512 in the palette = entry
// round(index x 255). On a soft round sprite (a glow) the centre mapping
// samples slightly further out than the game: up to ~8% too dim towards the
// edge (findings/43).
vec4 SamplePalettizedCorner(sampler2D indices, sampler2D palette, vec2 size, vec2 uv) {
  vec2 texel = uv * (size - 1.0);
  vec2 corner = floor(texel);
  vec2 weight = texel - corner;
  bool inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
  // The game reads at corner / size, a texel's edge, where point sampling
  // gives that texel; its centre is the same texel without rounding doubts.
  vec2 at = inside ? (corner + 0.5) / size : uv;
  vec2 step = 1.0 / size;
  vec4 p00 = PaletteColour(indices, palette, at);
  vec4 p10 = PaletteColour(indices, palette, at + vec2(step.x, 0.0));
  vec4 p01 = PaletteColour(indices, palette, at + vec2(0.0, step.y));
  vec4 p11 = PaletteColour(indices, palette, at + step);
  return mix(mix(p00, p10, weight.x), mix(p01, p11, weight.x), weight.y);
}
