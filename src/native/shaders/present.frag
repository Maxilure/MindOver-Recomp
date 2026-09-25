// =============================================================================
// present.frag -- copy the native renderer's frame into the guest output
// =============================================================================
// The presenter's guest-output image can be drawn into but not copied into
// (no transfer-destination usage), so the hand-off is this sampling pass.
// Later it's also the natural place for scaling, gamma or sharpening.
// =============================================================================
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_frame;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  o_color = texture(u_frame, v_uv);
}
