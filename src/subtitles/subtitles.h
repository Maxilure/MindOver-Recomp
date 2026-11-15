// =============================================================================
// subtitles/subtitles.h -- subtitles for the game's spoken lines (findings/63)
// =============================================================================
//
// WHAT THE PLAYER SEES (Options -> Accessibility, or --subtitles=1; off by
// default like the original, which had none): the line being spoken,
// centred near the bottom of the picture, in the game's own font
// (Titans_Small, ui/game_font.h) with its dark outline, over a dark box
// (0-100 % opaque, default 70 %), with the speaker's name in front when it is
// known for sure. At most two lines at once (the newest at the bottom).
// Every setting takes effect at once (read each frame); the Accessibility
// tab shows a sample line while it is open (options_menu.h SubtitlePreview).
// Hidden while the pause menu or the Options screen is up. Moved up above
// the game's message bar (achievement card, mission messages) while that is
// on screen, so neither covers the other (findings/63 s.21).
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
// a comment. Developer options (never in the Options screen):
// --subtitles_placeholders shows a voice line without text as its speaker
// folder and file name; --debug_subtitle_clock shows the playing movie's
// name and clock in the top left corner (for timing lines). Neither is ever
// part of a subtitle.
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

// The voices and movies are being followed (there is a text file, or a
// developer option is on). Decided at start-up. Whether subtitles are SHOWN
// is the `subtitles` flag, read every frame.
bool On();

// The pause menu is up this frame (its decision, achievements_screen.cpp):
// the subtitles stay hidden while this keeps coming. Any thread.
void NoteMenuFrame();

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
