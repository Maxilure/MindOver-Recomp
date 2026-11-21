// command_ring.h -- the GPU command ring, read by the port (graphics driver step 4)
//
// See command_ring.cpp. ON BY DEFAULT since 2026-11-21: the port reads D3D's
// command ring itself; the emulated GPU never sees a command, so the session
// has no emulated picture (Native Only, the renderer switch and the dual
// window locked). --renderer=emulated (launch option) = the SDK's GPU reads
// the ring as before.
#pragma once

#include <cstdint>

namespace command_ring {

// The launch setting is on (and --renderer isn't "emulated", --emulated_only
// isn't on, and the port ticks the
// vertical blank itself: the ring's interrupts need its callback). Decided
// from flags only, so the renderer can ask before the game starts.
bool Wanted();

// The port really took the ring (the game's ring set-up was answered).
bool Active();

// One vertical blank (vblank.cpp): the GPU's frame counter, which "write the
// counter" fence packets put into memory, counts vblanks and swaps.
void OnVblank();

}  // namespace command_ring
