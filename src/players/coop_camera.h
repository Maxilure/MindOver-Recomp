#pragma once

#include <cstdint>

namespace coop_camera {
// The game has just written its camera's position / direction / up (r3 of the
// VectorCamera rebuild sub_82373D18, free camera off): called before the
// matrices are rebuilt from them.
void OnGameCameraRebuilt(uint32_t camera);
}  // namespace coop_camera
