// =============================================================================
// bink.vert -- the game's movie material (xnBinkShader), vertex side
// =============================================================================
// The game's own vertex shader for movies is "passthru" (its debug info says
// so): no matrix, no colour, the position goes straight to the GPU. The
// positions the movie player writes are SCREEN PIXELS (a quad from (0,0) to
// (1280,720), seen by logging the vertices): the Xbox GPU can skip its
// viewport transform for such "pretransformed" vertices. Vulkan has no such
// switch, so we do that step here: pixels -> clip space (-1..1, y down).
// Pixel centres: no half-pixel shift, as for the simple material.
// In a WIDER picture (widescreen): see main().
// =============================================================================

#version 450

// Same block as in bink.frag (BinkParams in native_renderer.cpp).
layout(push_constant) uniform BinkParams {
  vec4 c[4];
  vec4 screen_size;  // xy: the game's screen in pixels (1280x720); z: WIDESCREEN's
                     // aspect / 16:9 (1 at 16:9)
} params;

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;

void main() {
  vec2 clip = in_position.xy / params.screen_size.xy * 2.0 - 1.0;
  // WIDESCREEN (native/widescreen.h): the game's movie player fits the movie
  // to the display's shape, which says "wider" (its height 1 / aspect): it
  // places the quad in the middle 16:9 sideways by itself (x 320..960 at
  // 32:9, measured 2026-11-07) but ALSO shrinks it by the same factor in
  // height (y 180..540). The height is scaled back up around the centre, so
  // the movie fills the picture's height, in the middle 16:9, bars at the
  // sides.
  clip.y *= params.screen_size.z;
  gl_Position = vec4(clip, in_position.z, 1.0);
  out_uv = in_uv;
}
