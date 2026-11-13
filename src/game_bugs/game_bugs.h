// =============================================================================
// game_bugs/game_bugs.h -- fixes for bugs of the ORIGINAL game (not the port's)
// =============================================================================
//
// WHAT: bugs that the Xbox 360 release has too: mistakes in Radical's own
// data or code, found by players, not caused by the recompilation. Each fix
// is small, keeps the game's look and feel otherwise untouched, and has its
// own switch (default on) so the original behaviour can be compared:
//
//   fix_spike_jack_sound   Spike (and Spike in the Hero levels) played the
//                          jacking sound twice; every other titan plays it
//                          once (spike_jack_sound.cpp, findings/61).
//
// debug_sound_track_trace (off by default): one log line per sound a fight
// tree starts ("Sound track: <cue> ..."), the tool that showed the double.
//
// Findings: docs/findings/61-original-game-bugs.md.
// =============================================================================
#pragma once

namespace game_bugs {

// Registers the data patches (call before data_patcher::Install()).
void Register();

}  // namespace game_bugs
