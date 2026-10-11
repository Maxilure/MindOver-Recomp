// =============================================================================
// dof.frag -- the game's depth of field pass (xnDOFShader), pixel side
// =============================================================================
// Written from our reading of the game's pixel shader (DOF; microcode only,
// disassembled with the SDK's --dump_shaders, inputs named by its constant
// table; docs/findings/15). What it does, for every pixel of the screen:
//
//   1. How blurry is this spot? The scene's depth under the pixel (the depth
//      copy the particles use) becomes a distance d, as particles and water
//      do: d = NearFar.y / ((1 - NearFar.x) - stored depth). Its "circle of
//      confusion" (CoC), how out of focus it is:
//        behind the focal plane:  saturate((d - focal) / (far - focal))
//                                 0 at the focal plane, 1 at the far plane
//        in front of it:          (d - focal) / (focal - near), NOT clamped
//                                 (-1 at the near plane, below that further),
//                                 only if near blur is on; else 0 (sharp)
//   2. A blur radius from the CoC: R = (CoC x 0.5 + 0.5) x MaxRadius.y -
//      MaxRadius.x, in pixels (the game's MaxRadius is (r, 2r), so R goes
//      from -r at the near plane through 0 in focus to r at the far plane).
//      Only its size |R| is used.
//   3. Seven more samples of the picture around the pixel, at fixed offsets
//      on a disk (a "Poisson disk": irregular, so the blur shows no pattern)
//      scaled by |R| pixels. Each sample's own CoC decides its weight: 1 if
//      it's at least as blurry as the centre, else |its CoC|. A sharp object
//      in front of a blurry background therefore doesn't smear into it (its
//      samples get a small weight); the background still blurs over itself.
//   4. colour = (centre + sum of weight x sample) / (1 + sum of weights),
//      alpha 1. The pass draws over the whole screen, unblended.
// Numbers written out (0.5, 1 and the seven offsets) are the literal
// constants in the shader file. Samples take the offsets' components
// swapped (x from the literal's second number): kept as the game does it.
//
// MOTION BLUR (ours, an option: kDofMotionBlur; native/camera_motion.h and
// docs/findings/34). The Xbox 360 game has none; we draw it here because this
// pass runs every frame right after the 3D scene and already has the picture
// and its depth. For each pixel:
//   a. Where was this spot last frame? Pixel + depth -> D3D clip position ->
//      x `reproject` (last frame's camera after this frame's inverse) -> last
//      frame's screen position. The difference is how far the CAMERA's
//      movement carried it across the screen in one frame.
//   b. The smear = that movement x the shutter (motion.x: the setting's share
//      of 1/30 s over this frame's length, so it looks the same at any frame
//      rate), at most motion.y pixels.
//   c. Players stay sharp: the camera follows them, so the camera's movement
//      says nothing about theirs. A pixel whose world position (x `unproject`)
//      is inside a player's capsule keeps the plain result, and samples that
//      land on a player are left out of the background's smear (else Crash
//      would ghost into the scenery around him).
//   d. Up to 16 samples along the smear, centred on the pixel, each one the
//      depth of field result at that spot (DofAt), averaged.
// With the motion blur off, or no movement, the pass is exactly the game's.
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require

#include "dof_params.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_picture;  // the picture so far (a resolve)
layout(set = 0, binding = 1) uniform sampler2D u_depth;    // the scene's depth (R32F copy)

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_clip;

layout(location = 0) out vec4 out_colour;

// The seven sample offsets (x, y), in the order the game samples them. In
// the shader's literals each pair is stored (y, x); written here as used.
const vec2 kTaps[7] = vec2[7](
    vec2(0.5278369784, -0.0858680010),
    vec2(-0.0400880016, 0.5360869765),
    vec2(-0.6704450250, -0.1799490005),
    vec2(-0.4194180071, -0.6160389781),
    vec2(0.4404529930, -0.6393989921),
    vec2(-0.7570880055, 0.3493340015),
    vec2(0.5746189952, 0.6858789921));

// Step 1: the circle of confusion of a stored depth.
float CircleOfConfusion(float stored) {
  float d = dof.near_far.y / ((1.0 - dof.near_far.x) - stored);
  float focal = dof.planes.x;
  if (!(focal > d)) {  // at or behind the focal plane (the game's test, NaN-exact)
    return clamp((d - focal) / (dof.planes.z - focal), 0.0, 1.0);
  }
  return DofFlag(kDofNearBlur) ? (d - focal) / (focal - dof.planes.y) : 0.0;
}

// The game's pass, steps 1-4, for any spot `uv` (the motion blur's samples;
// main() keeps its own copy for the centre, read exactly as the game does).
// A radius under a quarter pixel is just the picture there.
vec3 DofAt(vec2 uv) {
  vec3 sum = texture(u_picture, uv).rgb;
  float centre = CircleOfConfusion(texture(u_depth, uv).r);
  vec2 max_radius = dof.pixel_size_max_radius.zw;
  float radius = abs((centre * 0.5 + 0.5) * max_radius.y - max_radius.x);
  if (radius < 0.25) return sum;
  vec2 step = dof.pixel_size_max_radius.xy * radius;
  float weights = 1.0;
  for (int i = 0; i < 7; ++i) {
    vec2 tap = uv + kTaps[i] * step;
    float coc = CircleOfConfusion(texture(u_depth, tap).r);
    float weight = coc >= centre ? 1.0 : abs(coc);
    sum += weight * texture(u_picture, tap).rgb;
    weights += weight;
  }
  return sum / weights;
}

// D3D clip position of a screen spot (uv 0..1 from the top left) with a
// stored depth (the game's depth is flipped by its viewport: stored = 1 - z).
vec4 ClipAt(vec2 uv, float stored) {
  return vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, 1.0 - stored, 1.0);
}

// Step c: 1 inside a player's capsule (a vertical line with a radius; fading
// out over the radius' outer quarter), 0 elsewhere.
float PlayerAt(vec2 uv, float stored) {
  vec4 world = dof.unproject * ClipAt(uv, stored);
  if (!(abs(world.w) > 1e-6)) return 0.0;
  vec3 p = world.xyz / world.w;
  float keep = 0.0;
  for (uint i = 0u; i < min(dof.flags.y, 4u); ++i) {
    vec3 bottom = dof.spheres[i].xyz;
    float r = dof.spheres[i].w;
    vec3 nearest = bottom + vec3(0.0, clamp(p.y - bottom.y, 0.0, dof.heights[i]), 0.0);
    keep = max(keep, 1.0 - smoothstep(0.75 * r, r, distance(p, nearest)));
  }
  return keep;
}

// Steps a-d: the motion blur of `sharp` (the game's result at this pixel).
vec3 MotionBlur(vec2 screen, vec3 sharp) {
  float stored = texture(u_depth, screen).r;
  vec4 last = dof.reproject * ClipAt(screen, stored);  // a.
  if (!(last.w > 1e-6)) return sharp;
  vec2 last_uv = vec2(0.5, -0.5) * (last.xy / last.w) + 0.5;
  vec2 smear = (screen - last_uv) * dof.motion.x;  // b.
  vec2 pixels = smear / dof.pixel_size_max_radius.xy;
  float length_pixels = length(pixels);
  if (length_pixels > dof.motion.y) {
    smear *= dof.motion.y / length_pixels;
    length_pixels = dof.motion.y;
  }
  float keep = PlayerAt(screen, stored);  // c.
  if (length_pixels < 0.5 || keep >= 1.0) return sharp;
  int samples = clamp(int(ceil(length_pixels / 1.5)), 2, 16);  // d.
  vec3 sum = vec3(0.0);
  float weights = 0.0;
  for (int i = 0; i < samples; ++i) {
    vec2 uv = screen + smear * ((float(i) + 0.5) / float(samples) - 0.5);
    float weight = 1.0 - PlayerAt(uv, texture(u_depth, uv).r);
    sum += weight * DofAt(uv);
    weights += weight;
  }
  vec3 blurred = weights > 1e-3 ? sum / weights : sharp;
  return mix(blurred, sharp, keep);
}

void main() {
  // The centre: colour at the UV, depth at the pixel's screen spot (the
  // same place for this full-screen quad; the game reads them that way).
  vec2 screen = vec2(0.5 + 0.5 * in_clip.x / in_clip.w, 0.5 - 0.5 * in_clip.y / in_clip.w);
  vec3 sum = texture(u_picture, in_uv).rgb;
  float weights = 1.0;
  float centre = CircleOfConfusion(texture(u_depth, screen).r);

  // Step 2: the radius, in pixels (PixelSizeHigh turns it into UV).
  vec2 max_radius = dof.pixel_size_max_radius.zw;
  float radius = abs((centre * 0.5 + 0.5) * max_radius.y - max_radius.x);
  vec2 step = dof.pixel_size_max_radius.xy * radius;

  // Step 3: the seven samples, weighted by their own blurriness.
  for (int i = 0; i < 7; ++i) {
    vec2 uv = in_uv + kTaps[i] * step;
    float coc = CircleOfConfusion(texture(u_depth, uv).r);
    float weight = coc >= centre ? 1.0 : abs(coc);
    sum += weight * texture(u_picture, uv).rgb;
    weights += weight;
  }

  // Step 4 (+ ours: the motion blur, when on).
  vec3 colour = sum / weights;
  if (DofFlag(kDofMotionBlur)) colour = MotionBlur(screen, colour);
  out_colour = vec4(colour, 1.0);
}
