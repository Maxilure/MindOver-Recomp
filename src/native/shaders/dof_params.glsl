// =============================================================================
// dof_params.glsl -- per-draw parameters of the depth of field pass
// =============================================================================
// Shared by dof.vert and dof.frag. The same block as native::DofParams in
// src/native/frame.h (read that, and guest.h's kDofShaderVtable, for where
// each value comes from in the game): vec4s and a mat4 only, so std140 lays
// it out exactly like the C++ struct. One 1024-byte block per draw.
// =============================================================================

layout(set = 0, binding = 9, std140) uniform DofParams {
  mat4 mvp;                     // the full-screen quad's (D3D conventions)
  vec4 pixel_size_max_radius;   // xy 1 / screen size (PixelSizeHigh), zw MaxRadius
  vec4 planes;                  // x focal plane, y near blur plane, z far blur plane
  vec4 near_far;                // xy NearFar: stored depth -> distance
  uvec4 flags;                  // x kDof* below
} dof;

// flags.x (frame.h, kDof*).
const uint kDofNearBlur = 1u;  // the game's bEnableNearBlur (PS bool b10)

bool DofFlag(uint flag) { return (dof.flags.x & flag) != 0u; }
