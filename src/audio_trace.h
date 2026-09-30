// =============================================================================
// audio_trace.h -- which sounds the game asks for, by name (--debug_audio_trace)
// =============================================================================
//
// With --debug_audio_trace, every sound the game's audio code asks for gets
// a log line with the sound designer's own file name, e.g.
//
//   Audio: stream character\english\coco\ingame02_coco_001.wav (a1b2...)
//
// so a playtest log says which voice lines the game requested, and when.
// The why and the how (the game's sound files, the functions we wrap) are
// in audio_trace.cpp.
// =============================================================================

#pragma once

#include <filesystem>

namespace audio_trace {

// With --debug_audio_trace: reads the sound archives' file lists under
// `game_data_root` and starts logging. Once, before the game's code runs.
// Without the flag: nothing (the wrapped functions just call the originals).
void Start(const std::filesystem::path& game_data_root);

}  // namespace audio_trace
