// =============================================================================
// cheats/free_camera.h -- the cheat menu's free camera: fly the view anywhere
// =============================================================================
//
// WHAT: the game's camera follows Crash by itself (the player can't move it).
// With the free camera on, the view comes from us instead: it starts where
// the game's camera was and flies with the keyboard and mouse:
//   W / S        forward / back           A / D   left / right
//   E / Q        up / down                Shift   4x faster, Ctrl 4x slower
//   right mouse button held + move = look around (arrow keys look too)
// While it flies, the keyboard and mouse don't reach the game (Crash stands
// still; a controller still plays). "Keys move the camera" off = the view
// stays where it is and the keys play again (watch Crash from outside).
// Freeze (cheats.h) + free camera = a photo mode.
//
// HOW (findings/27): the game renders through ONE Pure3D camera object, a
// "VectorCamera" (vtable 0x8200BE84) with position +216, direction +228 and
// up +240 (3 floats each). Its matrices (world-to-camera +80, camera-to-world
// +144, position copy +192) are rebuilt lazily by sub_82373D18 whenever the
// byte +208 ("matrices are current") is 0. We wrap that rebuild: our
// position / direction / up go in just before it runs. The camera's getters
// (world-to-camera, camera-to-world, position, two transforms: Camera vtable
// slots 9, 13, 14, 17, 18) clear +208 first while we fly, so the view follows
// us even in frames where the game didn't move its camera. Everything that
// asks the camera (drawing, culling, sound, the water's reflection) sees the
// same view.
// =============================================================================
#pragma once

namespace rex::ui {
class Window;
}

namespace free_camera {

// On / off (any thread). Turning it on starts from the game camera's view.
void SetEnabled(bool on);
bool Enabled();

// Keys + mouse move the camera (else they play the game). Default on.
void SetKeysMoveCamera(bool on);
bool KeysMoveCamera();

// The free camera has the keys right now (on, and keys move the camera):
// CrashMomApp then keeps keyboard + mouse from the game.
bool Flying();

// Flying speed in game units per second (Crash runs ~10).
void SetSpeed(float units_per_second);
float Speed();

// Mouse / arrow look speed (1 = default).
void SetLookSpeed(float scale);
float LookSpeed();

// Back to where the game's camera is now (next frame).
void ResetToGameCamera();

// Where the camera is (ours while on, the game's otherwise), for the menu.
struct Pose {
  float position[3] = {};
  float direction[3] = {};
};
Pose CurrentPose();
// The GAME's camera (its last rebuild before ours), on or off.
Pose GameCameraPose();

// Mouse movement for looking around: listens to the main window (UI thread).
void AttachWindow(rex::ui::Window* window);

}  // namespace free_camera
