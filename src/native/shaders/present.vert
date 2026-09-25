// =============================================================================
// present.vert -- one triangle that covers the whole target
// =============================================================================
// The classic trick: vertices (0,0), (2,0), (0,2) in UV space, i.e. a
// triangle twice the screen size, clipped to exactly the screen. Vulkan's
// clip space has y pointing down, so uv (0,0) is the top-left corner, which
// is also texel (0,0) of the image we sample: no flip needed.
// =============================================================================
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
  v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
