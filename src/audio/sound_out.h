// =============================================================================
// audio/sound_out.h -- the game's mixed sound, played by OUR code (going native)
// =============================================================================
//
// The game (Radical's audio engine on top of Microsoft's XAudio library,
// statically linked) mixes all its sounds itself into blocks of 256 samples x
// 6 channels (5.1) at 48 kHz and hands each block to the Xbox's "render
// driver". Until now the SDK's fake Xbox played them. With --sound_out_native
// (on by default) our own SDL output plays them instead: the SDK's audio
// device is never even opened.
//
// The why and the how (the three game functions we cut into, the pacing,
// the stereo fold) are in sound_out.cpp; findings/54 tells the story.
// =============================================================================

#pragma once

#include <cstdint>

namespace sound_out {

// Statistics for the debug console / log: blocks played, blocks the speakers
// wanted but the game hadn't mixed yet (= a gap of silence, heard as a click).
struct Stats {
  uint64_t played = 0;
  uint64_t underruns = 0;
  int device_channels = 0;
  int queued = 0;
};
Stats GetStats();

}  // namespace sound_out
