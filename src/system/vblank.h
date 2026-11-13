// vblank.h -- the game's interrupt callback, shared with the command ring
//
// vblank.cpp keeps the callback the game registered with
// VdSetGraphicsInterruptCallback (graphics driver step 2). command_ring.cpp
// (step 4) raises the command stream's interrupts (source 1) through it.
#pragma once

#include <cstdint>

namespace vblank {

// The game's interrupt callback and its argument; false = none registered yet
// (or --vblank_native=false: the SDK keeps it then).
bool GameCallback(uint32_t& callback, uint32_t& argument);

}  // namespace vblank
