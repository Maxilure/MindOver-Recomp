// =============================================================================
// depth_copy_single.frag -- depth_copy.frag for a 1x (not multisampled)
// depth/stencil surface
// =============================================================================
// The same copy as depth_copy.frag (read it first): the depth as a 32-bit
// float, the stencil / 255, each pixel shifted by `offset`. A 1x surface
// could also be copied by the GPU's copy engine through a staging buffer
// (image -> buffer -> image, once per aspect), which is what the renderer
// did first; with the anti-aliasing setting Off (every surface 1x) that
// route cost about 10 % more GPU time than drawing 2x (measured 2026-10-11,
// findings/35: each copy waits for the one before), so 1x surfaces take
// this shader too. Same values: texelFetch reads the stored numbers as they
// are.
// =============================================================================

#version 450

layout(set = 0, binding = 0) uniform sampler2D u_depth;     // the depth aspect
layout(set = 0, binding = 1) uniform usampler2D u_stencil;  // the stencil aspect

layout(push_constant) uniform DepthCopyParams {
  ivec4 offset_sample;  // xy source minus destination pixel (z unused: one sample)
} params;

layout(location = 0) out float out_depth;
layout(location = 1) out float out_stencil;

void main() {
  ivec2 source = ivec2(gl_FragCoord.xy) + params.offset_sample.xy;
  out_depth = texelFetch(u_depth, source, 0).r;
  out_stencil = float(texelFetch(u_stencil, source, 0).r) / 255.0;
}
