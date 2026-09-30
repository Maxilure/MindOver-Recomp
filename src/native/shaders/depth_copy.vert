// =============================================================================
// depth_copy.vert -- one triangle covering the whole target (no vertex buffer)
// =============================================================================
// For the depth copy of a multisampled surface (depth_copy.frag,
// render_targets.cpp: ResolveDepthMultisampled). Vertices 0, 1, 2 land at
// (-1, -1), (3, -1), (-1, 3): a triangle bigger than the screen, so every
// pixel of the viewport is covered once; the scissor limits it to the
// resolve's rectangle.
// =============================================================================

#version 450

void main() {
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
