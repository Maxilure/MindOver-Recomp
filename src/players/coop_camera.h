#pragma once

#include <cstdint>

namespace coop_camera {
// The game has just written its camera's position / direction / up (r3 of the
// VectorCamera rebuild sub_82373D18, free camera off): called before the
// matrices are rebuilt from them.
void OnGameCameraRebuilt(uint32_t camera);

// A CUTSCENE'S CAMERA SHOT ONLINE (2026-11-20): the render's view set-up
// (sub_823793C8 -> sub_82381B38 -> the camera's SetView sub_82381E78, lr
// 0x82381B64) hands the camera its field of view every frame; during a shot
// that is the game's VOLUME camera's (it keeps running), and with each PC's
// own camera the guest's volume camera had followed player 2: the guest saw
// the shot zoomed out (N. Gin's lab shot: 1.31 against the host's 1.01, same
// position and direction). True (and `fov`) = use this one instead: the field
// of view the game focus player's PC sent with its input (what one shared
// camera would show). False = leave f1 alone (offline, no shot, this PC's
// player is the focus, nothing sent yet, another caller).
bool SharedShotFov(uint32_t lr, double& fov);
}  // namespace coop_camera
