// =============================================================================
// depth_copy.frag -- one sample of a multisampled depth/stencil surface
// =============================================================================
// The game resolves its depth/stencil surface into a texture that soft
// particles, water, depth of field (the depth) and the characters' soft
// shadows (the stencil) read later. When that surface is multisampled (the
// hub's is 2x MSAA), the Xbox copies ONE sample per pixel (a depth can't be
// averaged; the game asks for its sample 0), and Vulkan can't copy a
// multisampled image into a buffer. So this shader reads that sample itself
// and writes the two copies our render targets keep (render_targets.h):
//   output 0: the depth, as a 32-bit float (R32F image)
//   output 1: the stencil / 255 (R8 image: stores the stencil byte exactly)
// Each output pixel reads the source pixel at the same place shifted by
// `offset` (the resolve's source corner minus where it lands).
// =============================================================================

#version 450

layout(set = 0, binding = 0) uniform sampler2DMS u_depth;     // the depth aspect
layout(set = 0, binding = 1) uniform usampler2DMS u_stencil;  // the stencil aspect

layout(push_constant) uniform DepthCopyParams {
  ivec4 offset_sample;  // xy source minus destination pixel, z the Vulkan sample
} params;

layout(location = 0) out float out_depth;
layout(location = 1) out float out_stencil;

void main() {
  ivec2 source = ivec2(gl_FragCoord.xy) + params.offset_sample.xy;
  int sample_index = params.offset_sample.z;
  out_depth = texelFetch(u_depth, source, sample_index).r;
  out_stencil = float(texelFetch(u_stencil, source, sample_index).r) / 255.0;
}
