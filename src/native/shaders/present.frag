// =============================================================================
// present.frag -- copy the native renderer's frame into the guest output
// =============================================================================
// The presenter's guest-output image can be drawn into but not copied into
// (no transfer-destination usage), so the hand-off is this sampling pass.
//
// It also applies the Xbox's DISPLAY GAMMA RAMP: the console's video output
// maps every 8-bit channel value through a 256-entry table the game sets
// (the emulated GPU does the same when it presents, see
// NativeRenderer::UpdateGammaRamp). Without it the native picture came out
// brighter, most visibly in dark colours (27 -> 42 out of 255).
// =============================================================================
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_frame;
layout(set = 0, binding = 1) uniform sampler2D u_gamma_ramp;  // 256x1, point-sampled

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  vec3 colour = texture(u_frame, v_uv).rgb;
  // 0..1 -> table index 0..255 (our frame is 8 bits per channel: exact).
  ivec3 index = ivec3(colour * 255.0 + 0.5);
  o_color = vec4(texelFetch(u_gamma_ramp, ivec2(index.r, 0), 0).r,
                 texelFetch(u_gamma_ramp, ivec2(index.g, 0), 0).g,
                 texelFetch(u_gamma_ramp, ivec2(index.b, 0), 0).b, 1.0);
}
