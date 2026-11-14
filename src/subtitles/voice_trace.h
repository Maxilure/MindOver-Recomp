// =============================================================================
// subtitles/voice_trace.h -- when each voice really plays (--debug_subtitle_trace)
// =============================================================================
//
// Milestone 1 of the subtitles (docs/findings/63 s.12): a log only, nothing on
// screen. For every sound object the game creates (Radical's XmaDecoder) it
// says which sound it is, when its first samples go out, whether it stalls
// (pause), when it reaches its end and when it is destroyed; for every Bink
// movie, when it opens and closes and a few words of its Bink handle (to find
// the current frame's offset). The point is to CHECK the assumptions of
// findings/63 s.6 on the real game before any subtitle is drawn:
//   * can a decoder object be tied to its sound ID (same thread, just
//     before; confirmed by the clip's length in the archive)?
//   * does every voice line go through an XmaDecoder when it plays?
//   * what does a pause, a skip or an interrupted line look like?
//
// Off by default (a developer setting, English-only log lines). The
// functions below are called from the existing wrappers (audio_trace.cpp,
// audio/xma_native.cpp, language/language.cpp);
// the decoder's constructor and Read are wrapped in voice_trace.cpp.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace voice_trace {

// Is --debug_subtitle_trace on? (Read once at Start.)
bool On();

// Once at start-up, after audio_trace::Start (which indexes the archives).
void Start();

// A sound ID was turned into its .rsd path (audio_trace.cpp's wrapper of
// sub_82336430). `from` = the caller's return address.
void NoteSoundId(uint64_t id, uint32_t from);

// XmaDecoder destructor (sub_8234BC58, xma_native.cpp), before it frees anything.
void OnDecoderDestroyed(uint32_t object, uint8_t* base);

// A Bink movie opened (PlayerBink slot 6, language.cpp's handler, after the
// open). Its frames and its close are then watched at each frame's end.
void OnMovieOpen(uint32_t player, const std::string& name, uint8_t* base);

}  // namespace voice_trace
