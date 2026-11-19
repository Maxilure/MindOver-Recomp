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
REXCVAR_DECLARE(int32_t, logic_rate);

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

// Main thread, fixed step on: from THIS frame on, the game's clocks count
// from the next whole HOUR of clock time (frame k = that + (k - this) / N s).
// Two runs that call it on their own first frame then read identical clock
// VALUES frame for frame, whatever frame number they reached it on and however
// long their loading took (the ms rounding of 1/N s otherwise depends on the
// frame number, and the value on the load's length). The replay's first frame
// (input_record.cpp) and, later, an online session's first frame. False = not
// ready (off, or before the first clock read).
bool RestartClockAtWholeMs();

// A SESSION STARTS (2026-11-17, toward online / LAN co-op): the moment a
// session's save starts loading. Every copy of the game reaches it with its
// own history (boot, menus, how many frames its loads took), and the game's
// random generators carry that history: the enemy AI's generator (0x8259F928)
// already differed at a replay's first frame between a normal run and one
// starved on one CPU core. StartSession restarts the clock
// (RestartClockAtWholeMs) and sets every random generator the port knows to
// a value made from SessionSeed, so from here on the history doesn't matter:
//   0x825A14F4  the main generator (findings/66)
//   0x825A4FC4  the spawners' (CHeuristicSpawnerBehaviour; its "seeded"
//               flag 0x8259AD9E set: the clock seed is never taken after)
//   0x8259F928  the enemy AI's (code around the AI manager, 0x820BAxxx)
//   0x8257FA04  Pure3D's particle emitters' (3 pairs; view only, but alike
//               everywhere keeps the pictures comparable)
// Main thread, fixed step on (else nothing). Quick load calls it when the
// save is loaded and the level starts; an online session will at its start.
void StartSession();

// The seed for the game's random generators that it would otherwise take from
// its clock (the spawners' generator, manifest CrashMomSpawnerSeed): with the
// fixed step on, every run (and, later, every PC of an online session) must
// start them alike. Default = a fixed value; SetSessionSeed changes it (an
// online session's host picks one and sends it). Any thread.
// game_time::Clock's source (game_time.h), in nanoseconds, any thread:
// fixed step off = std::chrono::steady_clock's own count; on = the steady
// clock's count at the moment it came on + frame number / N s (game time).
int64_t GameClockNs();

uint32_t SessionSeed();
void SetSessionSeed(uint32_t seed);

// Called on the main thread at the start of every main-loop pass while the
// fixed step is on, after the frame number is set (input_record's replay
// watches for player 1's Crash there: every frame, not only on pad reads).
void SetPassListener(void (*listener)());

// The debug console's `clock` (any thread; applied at the next pass).
// fps 0 = back to the real clock. Returns the reply text.
std::string Set(int32_t fps, bool fast);
std::string Status();


}  // namespace fixed_step
