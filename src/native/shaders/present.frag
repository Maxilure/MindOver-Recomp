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
//
// SHARPENING (the port's `sharpening` setting, findings/41): before the
// ramp, an optional CONTRAST-ADAPTIVE SHARPENING filter, the idea of AMD's
// FidelityFX CAS (MIT licence; written here from its published description):
// each pixel is pushed away from its 4 neighbours (an unsharp mask), by an
// amount that shrinks where the 3x3 neighbourhood already has strong
// contrast. Soft detail (textures blurred by mipmaps / anti-aliasing) gets
// crisper; hard edges don't get halos or ringing. The frame and the output
// are the same size (1280x720), so neighbours are whole texels.
// `push.sharpness` 0 = off (the plain copy), up to 1 = the strongest.
// =============================================================================
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_frame;
layout(set = 0, binding = 1) uniform sampler2D u_gamma_ramp;  // 256x1, point-sampled

layout(push_constant) uniform PresentPush {
  float sharpness;  // 0 off .. 1 strongest (NativeRenderer, kSharpness)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// One texel of our frame, `offset` texels away from this pixel (clamped at
// the picture's border).
vec3 Texel(ivec2 centre, ivec2 offset) {
  const ivec2 last = textureSize(u_frame, 0) - 1;
  return texelFetch(u_frame, clamp(centre + offset, ivec2(0), last), 0).rgb;
}

vec3 Sharpened(vec3 e) {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  //  a b c
  //  d e f
  //  g h i
  const vec3 a = Texel(p, ivec2(-1, -1)), b = Texel(p, ivec2(0, -1)), c = Texel(p, ivec2(1, -1));
  const vec3 d = Texel(p, ivec2(-1, 0)), f = Texel(p, ivec2(1, 0));
  const vec3 g = Texel(p, ivec2(-1, 1)), h = Texel(p, ivec2(0, 1)), i = Texel(p, ivec2(1, 1));
  // Soft minimum / maximum: the cross, plus the corners (as CAS does: the
  // two add up, so a lone bright corner counts less than an edge).
  vec3 lo = min(min(min(d, e), min(f, b)), h);
  vec3 hi = max(max(max(d, e), max(f, b)), h);
  lo += min(lo, min(min(a, c), min(g, i)));
  hi += max(hi, max(max(a, c), max(g, i)));
  // How much room to sharpen: near 1 in flat / soft areas, near 0 where the
  // neighbourhood already spans black to white.
  const vec3 amount = sqrt(clamp(min(lo, 2.0 - hi) / max(hi, vec3(1e-5)), 0.0, 1.0));
  // Neighbour weight: -1/8 (gentle) .. -1/5 (strong), as CAS's slider.
  const vec3 w = amount * (-1.0 / mix(8.0, 5.0, push.sharpness));
  return clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}

void main() {
  vec3 colour = texture(u_frame, v_uv).rgb;
  if (push.sharpness > 0.0) {
    colour = Sharpened(colour);
  }
  // 0..1 -> table index 0..255 (our frame is 8 bits per channel: exact).
  ivec3 index = ivec3(colour * 255.0 + 0.5);
  o_color = vec4(texelFetch(u_gamma_ramp, ivec2(index.r, 0), 0).r,
                 texelFetch(u_gamma_ramp, ivec2(index.g, 0), 0).g,
                 texelFetch(u_gamma_ramp, ivec2(index.b, 0), 0).b, 1.0);
}
