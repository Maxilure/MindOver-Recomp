// =============================================================================
// gameplay/reduce_motion.h -- the Reduce Motion setting (Options ->
// Accessibility -> Motion), asked by the parts of the port it changes
// =============================================================================
// What the setting does, and how: reduce_motion.cpp, docs/findings/67.
#pragma once

#include <cstdint>

namespace reduce_motion {

// The setting (`reduce_motion`, off by default: the original).
bool On();

// The port's motion blur level to use (native/recorder.cpp): the
// `motion_blur` setting, or 0 while Reduce Motion is on.
int MotionBlurLevel();

// CTimeManager::GetScale's result (cheats/cheats.cpp wraps that function):
// with the setting on, the game's slow-motion factor (manager +4, when
// between 0 and 1) is taken back out of `scale`.
double GameTimeScale(const uint8_t* base, uint32_t manager, double scale);

}  // namespace reduce_motion
