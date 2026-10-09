// =============================================================================
// input_record.h -- every controller state the game reads, to a file, and back
// =============================================================================
//
// RECORD (ON BY DEFAULT: one of the session's event logs, session_log.h):
// next to the session's log (--log_file), <log name>-inputs.txt gets one line
// each time a player's controller state CHANGES, exactly as the game reads it
// (whatever the device: pad, keyboard + mouse, a fake controller; after our
// own filters):
//     <ms> <player 1-4> <buttons hex> <LT> <RT> <LX> <LY> <RX> <RY>
//     <ms> <player> off          (no controller for that player)
//     <ms> fe <state>            (the front end took an exit: where we are)
// ms = since launch. A held stick changes a little every frame, so moving
// around costs ~2-3 KB/s; the logs budget (--logs_budget_mb) cleans old ones
// with the logs.
//
// Why: a playtest's bug ("Crash fell through the floor after I did X") could
// only be redone from the player's description. With the recording, the same
// presses can be played back into the game (REPLAY) from a save near the spot.
// The game runs on real time (frame steps vary), so a replay is CLOSE, not
// exact: good for "does this sequence of moves trigger it", not frame-perfect.
// EXACT REPLAYS: turn the FIXED STEP on first (fixed_step.h: console `clock
// fixed 30`): the replay then counts GAME FRAMES, and each recorded change
// lands on a 30 Hz grid of game time, the same in a 30, 60 or 180 fps run.
//
// REPLAY (the debug console, debug_console.h):
//     replay <file> [from_ms] [to_ms] [raw] [onlevel] [onspawn]   play that part of a recording, starting
//                                       now: from_ms in the recording = now
//     replay stop / replay              stop / status
//     wait replay [ms]                  until it ends
// While it plays, every player the recording has lines for gets the
// recorded state (real and fake controllers for that player are ignored);
// "off" lines unplug them. Players the recording doesn't mention are left
// alone.
//
// Where: the game's XInputGetState wrapper (sub_824742F0, r3 = player, r4 =
// XINPUT_STATE: u32 packet number + the 12-byte big-endian gamepad), wrapped
// in saves/save_library.cpp; it calls BeforeFilters / AfterFilters.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, debug_input_record);

namespace input_record {

// Game thread, inside the wrapper, right after the real read: a replay
// overrides the state here (`result` = the wrapper's r3: 0 connected).
void BeforeFilters(uint32_t player, uint8_t* gamepad, uint32_t& result, uint8_t* base);
// Game thread, at the wrapper's end: records what the game gets.
void AfterFilters(uint32_t player, const uint8_t* gamepad, uint32_t result);

// The debug console's `replay` (any thread). Returns the reply text; throws
// std::runtime_error on a bad file / range.
// raw: in a game-frame replay above 30 fps, hand changes over at once
// instead of aligned to when a 30 fps frame would act on them (input_record.cpp).
// on_level: a game-frame replay waits for the frame the level is PLAYABLE
// (front end GameRunning 489, faded in: the console's `wait level`) and
// starts there (close, but not the same frame of player 1's Crash in every
// run: the loading thread decides; input_record.cpp).
// on_spawn: start 1.5 s of game time after player 1's Crash is created
// instead (the same Crash-clock moment in every run; with on_level too, also
// not before the level is playable).
std::string StartReplay(const std::string& path, int64_t from_ms, int64_t to_ms, bool raw = false,
                        bool on_level = false, bool on_spawn = false);
std::string StopReplay();
std::string ReplayStatus();
bool ReplayRunning();

// A game-frame replay's own frame count (the frame it's on, from 0 at its
// first controller read), or -1 when no such replay runs. The console's
// `track` writes it, so two runs line up on the replay's start.
int64_t ReplayFrame();

}  // namespace input_record
