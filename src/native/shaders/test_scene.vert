// =============================================================================
// test_scene.vert -- milestone 2 test picture: one rotating quad
// =============================================================================
// Proves the native renderer draws its own frames, one per game frame: the
// angle comes from the game's frame counter (see NativeRenderer::OnFrameEnd).
// No vertex buffer: the 6 corners of two triangles come from gl_VertexIndex.
// =============================================================================
#version 450

layout(push_constant) uniform Params {
  vec4 params;  // x = angle (radians), y = target aspect ratio (width / height)
  vec4 tint;    // rgb multiplier for the quad
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
  const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
  vec2 p = corners[gl_VertexIndex];
  v_uv = p * 0.5 + 0.5;
  float c = cos(pc.params.x);
  float s = sin(pc.params.x);
  vec2 rotated = vec2(c * p.x - s * p.y, s * p.x + c * p.y) * 0.45;
  rotated.x /= pc.params.y;  // square on screen, not stretched to 16:9
  gl_Position = vec4(rotated, 0.0, 1.0);
}
