// =============================================================================
// shadow.vert -- the game's soft character shadow (xnShadowShader), vertex side
// =============================================================================
// Written from what the game's vertex shader does (shadowshadervp; it ships
// with debug info, docs/findings/12): the full-screen quad's position times
// the model-view-projection matrix, and the position itself as the texture
// coordinate (the game draws the quad from 0,0 to 1,1 with an orthographic
// matrix); the vertex colour is the shadow's darkness.
// =============================================================================

#version 450

layout(set = 0, binding = 9, std140) uniform ShadowParams {
  mat4 mvp;
  vec4 offsets[9];
  vec4 weights[9];
} shadow;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;  // the game's D3DCOLOR, read as B8G8R8A8_UNORM

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_colour;

void main() {
  vec4 position = shadow.mvp * vec4(in_position, 1.0);
  gl_Position = vec4(position.x, -position.y, position.z, position.w);
  out_uv = in_position.xy;
  out_colour = in_colour;
}
