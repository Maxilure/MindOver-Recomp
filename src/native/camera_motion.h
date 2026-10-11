// =============================================================================
// native/camera_motion.h -- how the camera moved since the last frame, for
// the port's MOTION BLUR (an option; the Xbox 360 game has none)
// =============================================================================
//
// WHAT FOR
//   Motion blur smears each pixel along the way it moved on screen since the
//   last picture, the way a camera's shutter, open for a moment, smears what
//   moves. The Xbox 360 renderer has it switched off (findings/21: the
//   engine's CMotionBlurRenderer exists, but nothing ever creates it and the
//   backend's blur functions are empty), so this is OUR effect, drawn by the
//   native renderer inside the game's depth of field pass (shaders/dof.frag:
//   that pass runs every frame, menus included, and already has the picture
//   and the scene's depth). docs/findings/34.
//
// HOW WE KNOW THE CAMERA
//   Every 3D draw hands its vertex shader the model-view-projection matrix
//   (VS c4-c7) and the model's world matrix (VS c12-c15, "matWorld"). The
//   camera's view-projection is MVP x inverse(world): measured 2026-10-11,
//   every draw of a frame (world, props, lit characters) gives the same one,
//   except the water's planar reflection pass, which has its own (mirrored)
//   camera. So per frame we collect those matrices, and the one most draws
//   share is THE camera. With last frame's we can tell, for any pixel, where
//   the same spot of the world was on screen one frame ago (the shader turns
//   the pixel + its depth back into a world position with this frame's
//   matrix and projects it with last frame's).
//
// WHAT STAYS SHARP: THE PLAYERS
//   That only knows the CAMERA's movement; it treats everything as standing
//   still. The camera follows Crash, so he barely moves on screen while the
//   maths would say he moved like the scenery behind him and smear him. So
//   each player's body (their Crash, or the titan they ride) gets a sphere:
//   pixels whose world position falls inside stay sharp, and samples landing
//   on them don't bleed into the blurred background around them.
//
// THREAD: the game's main thread (the recorder's handlers).
// =============================================================================

#pragma once

#include <cstdint>

namespace native {
namespace camera_motion {

// One 3D draw's matrices (column-major, as DrawCommand::mvp), for the vote on
// this frame's camera. Draws without depth test (2D, the HUD) are left out
// by the caller.
void NoteDraw(const float mvp[16], const float world[16]);

// What the motion blur needs for this frame's picture.
struct Motion {
  bool valid = false;        // false = no blur this frame (first frame, a camera cut...)
  float reproject[16] = {};  // this frame's D3D clip position -> last frame's (column-major)
  float unproject[16] = {};  // this frame's D3D clip position -> world position
  // Up to 4 capsules around the players' bodies, in world units: xyz the
  // bottom of its vertical line, w the radius; `height` the line's length.
  // `spheres` of them are used.
  float sphere[4][4] = {};
  float height[4] = {};
  uint32_t spheres = 0;
  // How much of one frame's movement the blur covers (the "shutter": the
  // setting's share of 1/30 s, divided by this frame's real length, so the
  // smear is as long at 60 or 144 fps as at 30).
  float scale = 0;
};

// Called by the depth of field draw (the post-processing, after the 3D
// scene): this frame's camera from the draws so far, compared with last
// frame's. `strength` = the motion_blur setting (1 low .. 3 high; 0 = off,
// then only the bookkeeping runs).
Motion AtPostPass(const uint8_t* base, int strength);

// The frame is over (recorder::TakeFrame): this frame's camera becomes
// "last frame's". `recorded` = false when the next frame isn't recorded (the
// emulated picture is shown): then nothing carries over.
void EndFrame(bool recorded);

}  // namespace camera_motion
}  // namespace native
