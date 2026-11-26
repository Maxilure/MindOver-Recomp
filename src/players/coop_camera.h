#pragma once

#include <cstdint>

namespace coop_camera {
// The game has just written its camera's position / direction / up (r3 of the
// VectorCamera rebuild sub_82373D18, free camera off): called before the
// matrices are rebuilt from them.
void OnGameCameraRebuilt(uint32_t camera);

// Horizontal (XZ) unit forward vector of the local camera from the most recent
// camera rebuild. Used by online state sync (net/state_sync.cpp) to rotate a
// remote player's world movement direction into this PC's camera-relative left
// stick so locomotion plays full run animations regardless of camera angle.
bool GetForwardXZ(float& out_x, float& out_z);
}  // namespace coop_camera
