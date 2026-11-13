// command_ring.h -- the GPU command ring, read by the port (graphics driver step 4)
//
// See command_ring.cpp. Off by default: --command_ring_native=true at launch
// makes the port read D3D's command ring itself; the emulated GPU then never
// sees a command, so that session has no emulated picture (Native Only, the
// renderer switch and the dual window locked).
#pragma once

#include <cstdint>

namespace command_ring {

// The launch setting is on (and --emulated_only isn't, and the port ticks the
// vertical blank itself: the ring's interrupts need its callback). Decided
// from flags only, so the renderer can ask before the game starts.
bool Wanted();

// The port really took the ring (the game's ring set-up was answered).
bool Active();

// One vertical blank (vblank.cpp): the GPU's frame counter, which "write the
// counter" fence packets put into memory, counts vblanks and swaps.
void OnVblank();

}  // namespace command_ring
