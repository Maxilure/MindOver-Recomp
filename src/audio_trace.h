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

#include <cstdint>
#include <filesystem>
#include <string>

namespace audio_trace {

// With --debug_audio_trace: reads the sound archives' file lists under
// `game_data_root` and starts logging. Once, before the game's code runs.
// Without the flag: nothing (the wrapped functions just call the originals).
void Start(const std::filesystem::path& game_data_root);

// For the voice trace (subtitles/voice_trace.*, findings/63). All of these
// need the archive index, i.e. --debug_audio_trace (on by default).
bool Indexed();                       // the index was built
std::string SoundName(uint64_t id);   // the designer's file name, or a "(...)" note

// The XMA header the sound's .rsd file carries at 0x80C (big-endian; the
// same bytes the game's XmaDecoder keeps as H, findings/54 s.5).
struct ClipHeader {
  bool ok = false;
  uint32_t lead_in = 0;  // H+4
  uint32_t end = 0;      // H+8: the end sample
  uint32_t rate = 0;     // H+0x0C
  uint32_t data = 0;     // 0x808: compressed data bytes (the decoder's +0x1C)
};
ClipHeader ClipHeaderOf(uint64_t id);

// Seconds recorded so far by --debug_audio_dump, or -1 when not recording.
double DumpSeconds();

}  // namespace audio_trace
