// =============================================================================
// bumpmega_params.glsl -- per-draw parameters of the bump mega material
// =============================================================================
// Shared by bumpmega.vert and bumpmega.frag. The same block as
// native::BumpMegaParams in src/native/frame.h (read that, and guest.h's
// kBumpMegaShaderVtable, for where each value comes from in the game): vec4s
// and mat4s only, so std140 lays it out exactly like the C++ struct. One
// 1024-byte block per draw.
// =============================================================================

layout(set = 0, binding = 9, std140) uniform BumpMegaParams {
  mat4 mvp;                 // matMVP (D3D conventions)
  mat4 world;               // matWorld: model -> world
  mat4 rotation;            // matRot: normals, tangents, binormals -> world
  vec4 camera;              // CameraPos (xyz)
  vec4 rim_colour;          // RimColor
  vec4 reflect_colour;      // ReflectColor (RGBA)
  vec4 bump;                // x RefractBumpiness, y ReflectBumpiness, z TileWidth, w RefractBrightness
  vec4 palette_sizes;       // xy BilinearScale (diffuse), zw BilinearScale2 (normal map)
  vec4 fog_colour;          // rgb
  vec4 fog_fade;            // x fog start, y fog end, z Fade, w alpha test reference
  vec4 shadow_spheres[8];   // FxShadowPositions (xyz)
  uvec4 flags;              // x kBumpMega* below, y Fx shadow count, z alpha test function
} bump;

// flags.x (frame.h, kBumpMega*).
const uint kBumpMegaRim = 1u;
const uint kBumpMegaRefract = 2u;
const uint kBumpMegaReflect = 4u;
const uint kBumpMegaReflectMask = 8u;
const uint kBumpMegaPalette = 16u;
const uint kBumpMegaPalette2 = 32u;
const uint kBumpMegaRefractModulated = 64u;
const uint kBumpMegaFog = 128u;
const uint kBumpMegaNoColour = 256u;
const uint kBumpMegaNoNormalMap = 512u;
const uint kBumpMegaNoUv = 1024u;
const uint kBumpMegaHighlight = 2048u;

bool BumpFlag(uint flag) { return (bump.flags.x & flag) != 0u; }
