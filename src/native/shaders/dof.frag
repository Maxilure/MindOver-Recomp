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

  // Step 4.
  out_colour = vec4(sum / weights, 1.0);
}
