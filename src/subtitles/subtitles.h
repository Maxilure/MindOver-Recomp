// =============================================================================
// subtitles/subtitles.h -- subtitles for the game's spoken lines (findings/63)
// =============================================================================
//
// WHAT THE PLAYER SEES (with --subtitles=1, off by default like the original):
// the line being spoken, centred near the bottom of the picture, in the
// game's own font (Titans_Small, ui/game_font.h) with its dark outline, over
// an optional dark translucent box, with the speaker's name in front when it
// is known for sure. At most two lines at once (the newest at the bottom).
//
// WHERE THE TIMING COMES FROM (findings/63 s.16): voice_trace.cpp ties every
// sound the game decodes to its sound ID and reports
//   * the start: the decoder's first Read (10-40 ms after the line is asked
//     for, the moment its samples go to the mixer),
//   * a cut: the decoder destroyed before all of the line was decoded (a
//     skipped cutscene; only visible for lines longer than 4.52 s).
// A line then stays for its own length (the clip's end sample / rate) plus a
// short hold, or until it is cut. The game's pause menu does not stop the
// voices (findings/63 s.16.3), so the subtitle keeps its clock too.
//
// WHICH LINES (findings/63 s.11, s.13): the in-scope groups of the voice
// archive, by the sound designer's file name: Aku Aku, the tutorial voice,
// the NPC / story lines (`ingameNN_...`), Cortex's loudspeaker lines and the
// in-engine cutscene tracks. Combat lines are not subtitled.
//
// THE TEXT: `en.tsv`, then `<code>.tsv` (<code> = the text language's code,
// "pt-br"...) over it key by key. Each file is the player's own
// `<user folder>/subtitles/<name>` if there is one, else the shipped copy
// next to the exe (the repository's subtitles/ folder: the story cartoons'
// lines in English, transcribed, findings/63 s.20). TAB-separated:
//   <16 hex digits of a sound ID>  <text>            a sound's whole line
//   <key>  <start>  <end>  <speaker>  <text>         a timed line (cue)
// <key> = a movie's file name (Crash_02_51: its lines follow the movie's
// own clock, findings/63 s.15.1) or a sound ID (a sound with several lines:
// an in-engine cutscene track, timed from its start); times "mm:ss.cc" or
// seconds; the speaker may be empty. "\n" in a text breaks the line; # starts
// a comment. With --subtitles_placeholders, a line without text shows its
// speaker folder and file name, and a movie without lines its name and
// clock (for checking the timing and noting down the times of lines).
//
// Drawn by us on the layer over the finished picture (an ImGui foreground
// draw list textured with the font's pages, like fps_overlay.h): it shows in
// both renderers, during movies and menu transitions, but not in F10 photos.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

REXCVAR_DECLARE(int32_t, subtitles);

namespace subtitles {

// Subtitles are on (--subtitles=1). Read at start-up.
bool On();

// Once at start-up, after the text language is known: reads the text file.
void Start();

// From voice_trace.cpp (the audio thread): a sound began playing / was cut
// off. `name` = the sound designer's file name (audio_trace), `seconds` = its
// length.
void OnVoiceStart(uint64_t id, const std::string& name, double seconds);
void OnVoiceCut(uint64_t id);

// From voice_trace.cpp's frame-end watcher (the main thread): the Bink movie
// playing (its file name, d:\movies\Crash_02_51.bik) and its clock in seconds
// (the current frame / frame rate, findings/63 s.15.1); then its close.
void OnMovie(const std::string& name, double seconds);
void OnMovieEnd();

// The drawing (CrashMomApp::OnCreateDialogs, UI thread).
void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);

}  // namespace subtitles
