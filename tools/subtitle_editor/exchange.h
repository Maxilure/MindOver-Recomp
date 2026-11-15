// =============================================================================
// tools/subtitle_editor/exchange.h -- to and from other subtitle programs
// =============================================================================
//
// So a line or a cartoon can be timed in another program (Aegisub, Subtitle
// Composer, Gaupol, Subtitle Edit; or drafted by Whisper) and brought back:
//
// EXPORT writes, into <game folder>/user/subtitles/exchange/<voice>/:
//   <name>.srt  the line's pieces as SubRip: number, "HH:MM:SS,mmm -->
//               HH:MM:SS,mmm", the text (a speaker in front as "Name: ...",
//               the usual way to keep speakers in .srt);
//   <name>.wav  the sound as the editor plays it (its mixer's parts), 16-bit
//               stereo at the sound's own rate, for the other program to
//               time against.
// <name> = a cartoon's name (Crash_02_51), or a voice line's file name and
// sound ID (tutorial08_263_all_ad4ca2f1255a9876). Times: seconds from the
// start of that sound / movie, as in the editor and the game.
//
// IMPORT reads such a .srt back (UTF-8, with or without a BOM, CRLF or LF,
// "," or "." before the milliseconds; formatting tags such as <i> and {\an8}
// dropped). The editor turns its entries into pieces (main.cpp).
// =============================================================================
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "subtitle_texts.h"
#include "voice_archive.h"

namespace subtitle_editor {

struct SrtEntry {
  double start = 0, end = 0;
  std::string text;  // rows joined with "\n"
};

// The pieces as .srt text; a whole line (no times) spans 0 .. `length`.
std::string WriteSrt(const std::vector<Piece>& pieces, double length);
bool ReadSrt(const std::filesystem::path& file, std::vector<SrtEntry>* out, std::string* error);

// 16-bit PCM stereo .wav of `pcm`.
bool WriteWav(const std::filesystem::path& file, const Pcm& pcm, std::string* error);

// "Coco: Oh, stupid thing!" -> speaker "Coco", text "Oh, stupid thing!" when
// the name before ": " is one of `speakers` (case as given there); else
// the text stays whole and the speaker empty.
void SplitSpeaker(const std::string& line, const std::vector<std::string>& speakers, std::string* speaker,
                  std::string* text);

}  // namespace subtitle_editor
