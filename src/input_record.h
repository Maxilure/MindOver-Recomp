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
//
// REPLAY (the debug console, debug_console.h):
//     replay <file> [from_ms] [to_ms]   play that part of a recording, starting
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
void BeforeFilters(uint32_t player, uint8_t* gamepad, uint32_t& result);
// Game thread, at the wrapper's end: records what the game gets.
void AfterFilters(uint32_t player, const uint8_t* gamepad, uint32_t result);

// The debug console's `replay` (any thread). Returns the reply text; throws
// std::runtime_error on a bad file / range.
std::string StartReplay(const std::string& path, int64_t from_ms, int64_t to_ms);
std::string StopReplay();
std::string ReplayStatus();
bool ReplayRunning();

}  // namespace input_record
