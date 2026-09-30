// =============================================================================
// simple_params.glsl -- per-draw parameters of the "simple" material
// =============================================================================
// Shared by simple.vert, simple.frag and fxgrid.frag (the fx grid reuses the
// simple vertex shader and these parameters). Filled per draw by
// NativeRenderer::RecordAndSubmit (struct SimpleParams in native_renderer.cpp,
// same layout, 128 bytes: the size every Vulkan GPU guarantees for push
// constants).
// =============================================================================

layout(push_constant) uniform SimpleParams {
  // The game's model-view-projection matrix (its vertex shader's c4..c7),
  // column-major, D3D conventions.
  mat4 mvp;
  // rgb: the game's tint (vertex shader c100); a: fade (pixel shader c2.x).
  vec4 tint_fade;
  // xy: texture size in texels (palettized filtering); zw: fog start / end
  // (the game's pixel shader c71.x / c72.x).
  vec4 texture_size_fog_range;
  // x: alpha test reference 0..1; yzw: fog colour (pixel shader c70).
  vec4 alpha_ref_fog_colour;
  // x: flags (kFlag* below); y: alpha test compare function (Xbox enum:
  // 0 never, 1 less, 2 equal, 3 less-equal, 4 greater, 5 not-equal,
  // 6 greater-equal, 7 always).
  uvec4 flags;
} params;

const uint kFlagTextured = 1u;
const uint kFlagPalettized = 2u;
const uint kFlagFadeRgb = 4u;
const uint kFlagAlphaTest = 8u;
// The vertex has no colour / no texture coordinates: the attribute holds
// unrelated bytes (Vulkan wants it fed anyway) and must be ignored.
const uint kFlagNoColour = 16u;
const uint kFlagNoUv = 32u;
// Distance fog on (the game's pixel shader bool b69).
const uint kFlagFog = 64u;
// Fx grid only (fxgrid.frag): add the vertex colour to the picture's copy
// instead of multiplying (the game's pixel shader bool b10).
const uint kFlagFxAdd = 128u;
// The draw has proximity lights and/or Fx shadows on (the game's pixel shader
// c29.x / c200.x > 0): their positions, colours and the world matrix don't
// fit in push constants, so they come in the draw's LitParams block
// (lit_params.glsl; only world, point_*, shadow_spheres and counts.yz are
// filled). Without this flag that block is a zeroed one: don't read it.
const uint kFlagLights = 256u;

// The GPU's alpha test (the game's materials set it in their state cache):
// true = keep the pixel. Compare function and reference from `params`.
bool AlphaTest(float alpha) {
  float ref = params.alpha_ref_fog_colour.x;
  switch (params.flags.y) {
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
