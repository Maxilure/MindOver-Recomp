// =============================================================================
// shadow.frag -- the game's soft character shadow (xnShadowShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shader (shadowshaderfp;
// microcode + constant table, docs/findings/12).
//
// Before this quad, the game draws each character's SHADOW VOLUME (its
// silhouette stretched along the light) with two-sided stencil: the stencil
// ends up non-zero where the shadow falls on the ground. It then copies the
// stencil into a texture (a depth/stencil resolve) and draws this quad over
// the whole screen, where the stencil is non-zero (and zeroes it there).
//
// Per pixel it looks at 9 nearby points of the stencil copy (a small blur
// kernel: offsets and weights from the game's data): each one inside the
// shadow adds weight x (1 - shadow colour) of darkness. The result is
// multiplied onto the scene (blend: destination x source). Near the edge of
// the shadow some points fall outside it, so the edge fades: a soft shadow.
//
// The stencil copy is an R8 image (stencil / 255, bilinear like the game's
// sampler): "inside" = any stencil around that point, value > 0.
// =============================================================================

#version 450

layout(set = 0, binding = 9, std140) uniform ShadowParams {
  mat4 mvp;
  vec4 offsets[9];  // xy: texture coordinate offsets
  vec4 weights[9];  // per channel
} shadow;

layout(set = 0, binding = 0) uniform sampler2D u_stencil;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_colour;

layout(location = 0) out vec4 out_colour;

void main() {
  vec4 darkness = vec4(0.0);
  for (int i = 0; i < 9; ++i) {
    if (texture(u_stencil, in_uv + shadow.offsets[i].xy).r > 0.0) {
      darkness += shadow.weights[i] * (1.0 - in_colour);
    }
  }
  out_colour = 1.0 - darkness;
}
