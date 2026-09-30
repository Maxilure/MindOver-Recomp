// =============================================================================
// water_params.glsl -- per-draw parameters of the water material
// =============================================================================
// Shared by water.vert and water.frag. The same block as native::WaterParams
// in src/native/frame.h (read that, and guest.h's kWaterSpecularShaderVtable,
// for where each value comes from in the game): vec4s and mat4s only, so
// std140 lays it out exactly like the C++ struct. One 1024-byte block per draw.
// =============================================================================

layout(set = 0, binding = 9, std140) uniform WaterParams {
  mat4 mvp;                     // model-view-projection (D3D conventions)
  mat4 world;                   // model -> world
  vec4 camera;                  // CameraPos (xyz)
  vec4 view_direction;          // ViewDirection (xyz)
  vec4 reflect_colour;          // ReflectColor (RGBA)
  vec4 bumpiness_blend_pixel;   // x ReflectBumpiness, y WaterBumpiness, z Blend, w PixelHeight
  vec4 near_far;                // NearFar (xy)
  vec4 fog_colour;              // rgb
  vec4 fog_fade;                // x fog start, y fog end, z fade
  vec4 shadow_spheres[8];       // FxShadowPositions (xyz)
  uvec4 flags;                  // x kWater* below, y Fx shadow count
} water;

// flags.x (frame.h, kWater*).
const uint kWaterReflection = 1u;
const uint kWaterFog = 2u;
const uint kWaterNoColour = 4u;
const uint kWaterNoUv = 8u;

bool WaterFlag(uint flag) { return (water.flags.x & flag) != 0u; }
