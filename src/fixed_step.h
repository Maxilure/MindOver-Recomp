// =============================================================================
// fixed_step.h -- EXACT, REPEATABLE RUNS: every game frame lasts exactly 1/N s
// =============================================================================
//
// The game is time-based: each frame moves the world by the REAL time since
// the previous frame (findings/07). That's right for playing, but it makes
// two runs of the same presses differ: one frame takes 16.4 ms, the next
// 17.1, and the physics lands Crash a hair elsewhere every time. Comparing a
// 30 fps run with a 180 fps run to find what depends on the frame rate
// (the climbs, the bumps, titans stuck at edges: notes/plans.md section 2)
// was guesswork.
//
// FIXED STEP: the main loop's time delta (the ONE place every clock-driven
// part of a frame gets its time from: manifest CrashMomPassDelta, 0x8227B04C)
// is answered as exactly 1/N s, and every main-loop pass runs one frame. Game
// time is then simply frame number / N, and the controller replay
// (input_record.h) counts in game frames instead of milliseconds: the same
// recording gives the same presses at the same GAME moment in a 30 fps run
// and a 180 fps run.
//
//   --fixed_step=N           (0 = off, default) from launch
//   --fixed_step_fast        don't wait between frames: as fast as the PC can
//                            (game time no longer follows the wall clock;
//                            sound is off pitch, the game logic doesn't care)
//   debug console: `clock fixed <N> [fast]`, `clock real`, `clock` (status)
//
// While it's on, --fps_cap is ignored (the fixed step paces: one frame
// every 1/N s of wall clock, or none with fast).
//
// What isn't fixed: work on other threads (level streaming, sound) finishes
// whenever it finishes. Two runs may still part where the game waits for
// one of those; the compare tool (tools/compare_runs.py) shows where.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, fixed_step);
REXCVAR_DECLARE(bool, fixed_step_fast);

namespace fixed_step {

// Main thread (the hooks) and any thread (atomics).
bool Active();
// Is this the game's main thread (the one running the main loop)?
bool OnMainThread();
int32_t Fps();  // 0 while off

// Main-loop passes since the fixed step was (last) turned on. While on,
// every pass runs exactly one game frame, so this IS the game frame number:
// frame k starts at game time k / Fps() s. Counted at the START of a pass
// (before its controller reads), so the reads of a pass see its frame.
uint64_t Frame();

// The game's millisecond clock (sub_8235ABC8) as the main thread sees it this
// frame (fixed step on, after its first clock read; else 0).
uint32_t ClockMs();

// Called on the main thread at the start of every main-loop pass while the
// fixed step is on, after the frame number is set (input_record's replay
// watches for player 1's Crash there: every frame, not only on pad reads).
void SetPassListener(void (*listener)());

// The debug console's `clock` (any thread; applied at the next pass).
// fps 0 = back to the real clock. Returns the reply text.
std::string Set(int32_t fps, bool fast);
std::string Status();

}  // namespace fixed_step
