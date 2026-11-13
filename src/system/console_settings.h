// console_settings.h -- the console's TV mode, shared with the graphics driver
//
// console_settings.cpp answers the game's XGetVideoMode; graphics_driver.cpp
// answers D3D's VdQueryVideoMode with the SAME TV (1280x720, progressive,
// 16:9, hi-def, 60 Hz, NTSC), so every part of the game hears one console.
#pragma once

#include <cstdint>

namespace console_settings {

// Fills the 48-byte X_VIDEO_MODE at guest address `mode` (big-endian):
// width, height, interlaced, widescreen, hi-def, refresh (float), video
// standard, two unknown words.
void WriteTvMode(uint32_t mode);

// The same TV's numbers, for answers that need them one by one.
constexpr uint32_t kTvWidth = 1280;
constexpr uint32_t kTvHeight = 720;
constexpr float kTvRefreshHz = 60.0f;

}  // namespace console_settings
