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
#include <string>
#include <vector>

namespace sound_out {

// Speakers (setting sound_speakers, Options -> Audio):
//   kAuto       the device's own layout: 5.1 handed over as is, the system
//               fits it to the speakers (a 1-2 channel device: our stereo mix)
//   kStereo     our stereo mix (ITU fold, soft knee instead of a flat cut)
//   kHeadphones the stereo mix + crossfeed (a little of each side, low and
//               late, reaches the other ear, like speakers in a room)
//   k51 / k71   6 / 8 channels (7.1: the surround pair on side + back)
enum class Speakers { kAuto = 0, kStereo = 1, kHeadphones = 2, k51 = 3, k71 = 4 };

// Names of the playback devices connected now (for Options -> Audio ->
// Output Device; the setting sound_device holds one of them, "" = default).
std::vector<std::string> PlaybackDevices();
// The device playing now ("" = none yet).
std::string CurrentDevice();
// Open the device again with the current sound_device / sound_speakers
// (after Options changed them). Any thread; nothing until the game's mixer
// has started.
void Reopen();

// Statistics for the debug console / log: blocks played, blocks the speakers
// wanted but the game hadn't mixed yet (= a gap of silence, heard as a click).
struct Stats {
  uint64_t played = 0;
  uint64_t underruns = 0;
  int device_channels = 0;
  int queued = 0;
};
Stats GetStats();

// SOUND ON GAME FRAMES (2026-11-17, toward online / LAN co-op). Normally the
// speakers set the pace: each block they take lets the game mix the next one,
// so sound (and everything the game times by it) runs in REAL time. The game
// times things by its sound: an in-engine cutscene (the N. Gin lab scene, its
// 5.1 dialogue track) lasted ~6 s of real time in both a normal-speed and a
// fast exact run, so it ended on different frames and the runs parted.
// While the fixed step is on (fixed_step.h) and sound_follows_game_frames is
// on, every game frame instead lets the game mix exactly its share of sound
// (48000 / N samples = 3.125 blocks per frame at 60), and the frame waits
// until that is mixed: sound is then on GAME time, the same on every run and
// every PC. The speakers just play what's ready (fast runs: extra is dropped).
// Called by the fixed step at the start of every main-loop pass (game's main
// thread), with the frame number and the frame its counting starts from
// (fixed_step's clock epoch).
void OnFixedFrame(uint64_t frame, uint64_t epoch_frame, int32_t fps);

}  // namespace sound_out
