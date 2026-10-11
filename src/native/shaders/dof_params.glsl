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
  uvec4 flags;                  // x kDof* below, y how many player spheres
  // The port's motion blur (kDofMotionBlur; native/camera_motion.h).
  mat4 reproject;               // this frame's D3D clip position -> last frame's
  mat4 unproject;               // this frame's D3D clip position -> world position
  vec4 spheres[4];              // the players' bodies: capsules (xyz bottom of the line, w radius)
  vec4 heights;                 // ... the length of each capsule's vertical line
  vec4 motion;                  // x shutter (share of a frame's movement), y longest smear (pixels)
} dof;

// flags.x (frame.h, kDof*).
const uint kDofNearBlur = 1u;    // the game's bEnableNearBlur (PS bool b10)
const uint kDofMotionBlur = 2u;  // ours: motion blur on, this frame has a camera

bool DofFlag(uint flag) { return (dof.flags.x & flag) != 0u; }
