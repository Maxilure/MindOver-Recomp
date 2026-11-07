// =============================================================================
// native/widescreen.h -- a picture as wide as the window ("Hor+" widescreen)
// =============================================================================
//
// The Xbox 360 game draws a 16:9 picture. With widescreen on, a window wider
// than 16:9 (21:9, 32:9, a window stretched across two monitors) gets a WIDER
// PICTURE: the camera sees more to the left and right, the top and bottom
// stay exactly as the game shows them, and nothing is stretched. A narrower
// window keeps the game's 16:9 with black bars (findings/50 s.5).
//
// HOW (findings/56, research 2026-11-07):
// - The game asks its display object (xnDisplay, pointer at 0x82571B78) for
//   the screen's shape. Floats at +1680 / +1684 = width / height (1 and
//   0.5625 = 16:9; xnDisplay slot 0x54 returns their ratio, which the views'
//   cameras take as their aspect), +1688 / +1692 = the same pair again
//   (slot 0x50: the default projection and the menus' projection), +1696 /
//   +1700 = 0.9 x height / 0.9 (a 90% safe area). We set both heights =
//   1 / aspect, +1688 = 1 and +1680 = (16 / 9) / aspect (slot 0x54, read by
//   the views, keeps answering 16:9). The menu library (Scrooby) fits its 640 x 480 canvas
//   by min(width / 640, height / 480), so its pages stay the same size and
//   centred: the extra room is at the sides.
// - Every projection goes through the context's SET PROJECTION
//   (pddiBaseContext::v29, sub_8243E4B8: near, far, HORIZONTAL field of view,
//   aspect). The world's arrives as 16:9 (the camera's view keeps the aspect
//   it got at boot); there we put our aspect and widen the field of view so
//   the VERTICAL one stays the game's: tan(new / 2) = tan(game / 2) x
//   aspect / (16 / 9). The menus' arrives with the display's aspect (already
//   ours): untouched. The camera's own +28 is written every frame from the
//   display's wide slot 0x50 (sub_82381E78, found with `who`): we divide the
//   wide factor back out right after, so everything the GAME does with its
//   camera (aiming, "is it on screen", markers, the co-op camera's fit) sees
//   the middle 16:9 of our picture, exactly what the original shows there.
// - The native renderer draws every surface wider by the same factor
//   (RenderTargets' sideways scale): the game still works in its own
//   1280 x 720 pixels; its viewports, clears and resolves are scaled.
//
// ONLY WITH THE NATIVE PICTURE ALONE (native only, main window on our
// picture, no dual window): the emulated GPU draws 1280 x 720 and would show a
// squashed picture otherwise.
//
// The shape used for a frame is the one the GAME used for it: the renderer
// takes AspectForThisFrame() (set one frame earlier), then Update() picks
// the next frame's from the window.
// =============================================================================
#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace widescreen {

constexpr float kGameAspect = 16.0f / 9.0f;

// The main window (its size decides the shape). UI thread, once at setup.
void AttachWindow(rex::ui::Window* window);

// Native renderer, at each frame end (game's main thread): `allowed` = our
// picture is the only one drawn. Returns the aspect the frame just recorded
// was drawn with (kGameAspect when off), then writes the next frame's shape
// into the game's display object.
float Update(bool allowed);

// The aspect the game currently draws with (any thread).
float CurrentAspect();

// WHERE THE PICTURE IS in a window of `window_w` x `window_h` pixels: the
// presenter fits it whole and centred (bars at the sides or top / bottom).
// Picture() = the whole picture (CurrentAspect wide); Game() = its middle
// 16:9 part, where the game's own screen is (menus, HUD laid out in 16:9,
// the mouse's 0..1 coordinates). At 16:9 both are the same. Any thread.
struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
};
Rect Picture(float window_w, float window_h);
// The picture's height in the main window, in pixels (0 = no window yet):
// what "match the window" draws at (native/render_targets.cpp).
int WindowPictureHeight();
Rect Game(float window_w, float window_h);

// The pddi handler on the context's SET PROJECTION (widescreen.cpp
// OnSetProjection: the world's 16:9 projection -> ours). Once at startup.
void Install();

}  // namespace widescreen
