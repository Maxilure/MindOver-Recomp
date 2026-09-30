// =============================================================================
// fxgrid.frag -- the game's fx grid material (xnFxGridShader), pixel side
// =============================================================================
// Screen-wide effects drawn in the post-processing, e.g. the brightness
// flash while the double mojo power-up collects mojo (docs/findings/18).
// The game copies the picture so far into a texture, then covers the screen
// with batches of triangles through this material; each vertex has a
// colour (with alpha) and a texture coordinate into that copy.
//
// Written from our reading of the game's pixel shader (fxgrid, microcode
// only, disassembled with the SDK's --dump_shaders; guest.h,
// shader::kFxGridShaderVtable):
//
//   copy = the picture's copy at the vertex's texture coordinate
//   rgb  = copy + vertex colour     (its bool b10 on: a glow)
//          copy x vertex colour     (b10 off: a tint)
//   a    = vertex alpha
//
// The blending (the flash: src alpha / 1 - src alpha) then mixes that over
// the picture, which with an unmoved copy comes down to "picture + colour
// x alpha". The vertex side is the simple material's (simple.vert, with a
// white tint): the game's fxgridvp does the same matrix and passes colour
// and texture coordinate on.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "simple_params.glsl"

// The copy of the picture so far (a resolve of the post surface).
layout(set = 0, binding = 0) uniform sampler2D u_picture;

layout(location = 0) in vec4 in_colour;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in float in_fog_depth;  // simple.vert's; unused (no fog)

layout(location = 0) out vec4 out_colour;

void main() {
  uint flags = params.flags.x;
  vec3 copy = texture(u_picture, in_uv).rgb;
  vec3 rgb = (flags & kFlagFxAdd) != 0u ? copy + in_colour.rgb : copy * in_colour.rgb;
  vec4 colour = vec4(rgb, in_colour.a);
  if ((flags & kFlagAlphaTest) != 0u && !AlphaTest(colour.a)) {
    discard;
  }
  // Values above 1 (a bright glow on a bright picture) are clamped by the
  // 8-bit render target before blending, like on the Xbox.
  out_colour = colour;
}
