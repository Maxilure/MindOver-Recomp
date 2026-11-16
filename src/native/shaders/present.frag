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
//
// COLOUR BLINDNESS (the port's `colourblindness_mode` / `_intensity`,
// findings/64): after the sharpening, before the ramp, an optional
// DALTONISATION: colours a player with that colour vision deficiency would
// confuse are pushed apart into channels they still see well.
//   1. simulate what the player sees: a 3x3 matrix in linear light, from
//      Machado, Oliveira and Fernandes (2009, "A Physiologically-based Model
//      for Simulation of Color Vision Deficiency"): severity 1.0 for the
//      -anopias (one cone type missing), 0.6 for the -anomalies (shifted);
//   2. the error = what is lost (the colour minus the simulation);
//   3. that error is added back where it can be seen: red-green losses
//      (protan / deutan) into green and blue, blue-yellow losses (tritan)
//      into red and green (the classic daltonise redistribution);
//   4. ACHROMATOPSIA (no colour at all) can't be corrected by moving colour
//      around: the lost colour becomes a little BRIGHTNESS instead (reddish
//      lighter, bluish darker), so two colours of the same brightness don't
//      turn into the same grey. Achromatomaly = the same, gentler.
// `push.intensity` mixes the original (0) and the full correction (1);
// mode 0 = off (the plain colour, nothing computed).
// =============================================================================
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_frame;
layout(set = 0, binding = 1) uniform sampler2D u_gamma_ramp;  // 256x1, point-sampled

layout(push_constant) uniform PresentPush {
  float sharpness;  // 0 off .. 1 strongest (NativeRenderer, kSharpness)
  int cvd_mode;     // colourblindness_mode: 0 off, 1-8 (see CvdSimulation)
  float intensity;  // colourblindness_intensity / 100: 0 original .. 1 full
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

// The frame's 0..1 values are display-encoded (the gamma ramp comes after
// this): the simulation's matrices work on LINEAR light, so in and out
// through a 2.2 power (the Xbox's TV gamma; close enough for this).
vec3 ToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 ToDisplay(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }

// Machado et al. 2009, linear RGB, rows = output R, G, B. GLSL's mat3 is
// filled column by column, hence transpose() on rows written as rows.
mat3 Rows(vec3 r, vec3 g, vec3 b) { return transpose(mat3(r, g, b)); }

// What a player with this deficiency sees (`mode` 1-6; 7-8 = grey).
vec3 CvdSimulation(vec3 c, int mode) {
  if (mode == 1) {  // protanopia: no red cones (severity 1.0)
    return Rows(vec3(0.152286, 1.052583, -0.204868), vec3(0.114503, 0.786281, 0.099216),
                vec3(-0.003882, -0.048116, 1.051998)) * c;
  }
  if (mode == 2) {  // protanomaly: shifted red cones (severity 0.6)
    return Rows(vec3(0.385450, 0.769005, -0.154455), vec3(0.100526, 0.829802, 0.069673),
                vec3(-0.007442, -0.022190, 1.029632)) * c;
  }
  if (mode == 3) {  // deuteranopia: no green cones
    return Rows(vec3(0.367322, 0.860646, -0.227968), vec3(0.280085, 0.672501, 0.047413),
                vec3(-0.011820, 0.042940, 0.968881)) * c;
  }
  if (mode == 4) {  // deuteranomaly
    return Rows(vec3(0.547494, 0.607765, -0.155259), vec3(0.181692, 0.781742, 0.036566),
                vec3(-0.010410, 0.027275, 0.983136)) * c;
  }
  if (mode == 5) {  // tritanopia: no blue cones
    return Rows(vec3(1.255528, -0.076749, -0.178779), vec3(-0.078411, 0.930809, 0.147602),
                vec3(0.004733, 0.691367, 0.303900)) * c;
  }
  if (mode == 6) {  // tritanomaly
    return Rows(vec3(1.104996, -0.046633, -0.058363), vec3(-0.032137, 0.971635, 0.060503),
                vec3(0.001336, 0.317922, 0.680742)) * c;
  }
  // 7 achromatopsia: only brightness (Rec. 709 luminance); 8 achromatomaly:
  // most of the colour gone (0.6 of the way to grey, like the -anomalies).
  const vec3 grey = vec3(dot(c, vec3(0.2126, 0.7152, 0.0722)));
  return mode == 7 ? grey : mix(c, grey, 0.6);
}

// The daltonised colour (display-encoded in and out).
vec3 Daltonised(vec3 display, int mode) {
  const vec3 c = ToLinear(display);
  const vec3 err = c - CvdSimulation(c, mode);
  vec3 fixed_colour;
  if (mode <= 4) {
    // Red-green: the lost red difference moved into green and blue.
    fixed_colour = c + vec3(0.0, 0.7 * err.r + err.g, 0.7 * err.r + err.b);
  } else if (mode <= 6) {
    // Blue-yellow: the lost blue difference moved into red and green.
    fixed_colour = c + vec3(err.r + 0.7 * err.b, err.g + 0.7 * err.b, 0.0);
  } else {
    // No colour vision: the lost colour as brightness (reddish lighter,
    // bluish darker); gentler for achromatomaly.
    const float k = mode == 7 ? 0.5 : 0.3;
    fixed_colour = c + vec3(k * (err.r - err.b));
  }
  return ToDisplay(fixed_colour);
}

void main() {
  vec3 colour = texture(u_frame, v_uv).rgb;
  if (push.sharpness > 0.0) {
    colour = Sharpened(colour);
  }
  if (push.cvd_mode > 0 && push.intensity > 0.0) {
    colour = mix(colour, Daltonised(colour, push.cvd_mode), push.intensity);
  }
  // 0..1 -> table index 0..255 (our frame is 8 bits per channel: exact).
  ivec3 index = ivec3(colour * 255.0 + 0.5);
  o_color = vec4(texelFetch(u_gamma_ramp, ivec2(index.r, 0), 0).r,
                 texelFetch(u_gamma_ramp, ivec2(index.g, 0), 0).g,
                 texelFetch(u_gamma_ramp, ivec2(index.b, 0), 0).b, 1.0);
}
