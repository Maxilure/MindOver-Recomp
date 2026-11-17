// =============================================================================
// system/helper_threads.h -- LOADING ON GAME FRAMES: every game frame starts
// only once the game's helper threads have done all the work asked of them
// =============================================================================
//
// WHY (2026-11-17, toward online / LAN co-op). Besides its main thread, the
// game runs helper threads: two Radical workers (sub_8236A3F8), the audio
// loaders (audio::IOLoadTaskQueue, ClipLoadThread, two audio::Streamer), the
// audio HAL, D3D's... They load files and prepare data while the main thread
// keeps running frames, so WHEN a piece of data lands depends on the disk and
// the CPU. The game uses data the moment it's there: after a level change,
// Crash is created as soon as his data arrives and the loading screen ends when
// the loading thread is done (findings/29 s.6). Two exact runs at different
// speeds therefore saw the new level become playable one frame apart, and an
// online session's PCs would part the same way.
//
// HOW. Every wait the game makes goes through a kernel import (Ke/Nt waits,
// KeDelayExecutionThread, RtlEnterCriticalSection), and the Xbox calls meter's
// generated wrappers (CMakeLists.txt, Linux) already sit around each import.
// For these waits they also call BeforeWait / AfterWait here, which keep, per
// thread: "inside a wait right now" + when that last changed, and a global
// count of WAKES (a wait that ended for any reason but its timeout).
// While the fixed step is on (fixed_step.h) and loading_follows_game_frames is
// on, the main thread calls WaitUntilQuiet() at the start of every main-loop
// pass: it returns once every helper thread is inside a wait and no wake has
// happened for a short settle time. Everything the main thread asked for
// before this frame is then finished, on every run and every PC alike: data
// lands on the same frame everywhere.
//
// Not counted as work: a thread whose wait ended by its TIMEOUT before the
// game time reached it (see OnGameTime below) and a thread that has ENDED
// (logged once). A busy thread is waited for however long it works (a
// level's loader decompresses for seconds without a wait: the loading screen
// holds still meanwhile); a frame never waits longer than kMaxWait, 30 s
// (logged).
//
// Linux only for now: Windows' linker has no --wrap (no Xbox calls meter
// there); WaitUntilQuiet then returns at once.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace helper_threads {

// The wrappers of the game's wait imports (any thread). `timed_sleep` = the
// call is KeDelayExecutionThread (a plain sleep: its end is never a wake).
void BeforeWait();
void AfterWait(uint32_t status, bool timed_sleep);

// HELPER THREADS ON GAME TIME (2026-11-17). Waiting for idle helpers isn't
// enough on its own: a helper that wakes up because its TIMEOUT ran out (a
// worker that checks for work every N ms, a sleep) does so on the PC's real
// clock, and the main thread's "quiet" check counted a thread inside such a
// wait as idle. A load on one starved CPU core then took 154 frames instead
// of the normal 300 (= 5.0 s of real time at 60: loading still followed the
// wall clock). So, while the fixed step is on and helper_threads_on_game_time
// is on:
//  * the game's three clocks answer the FRAME'S time on helper threads too
//    (fixed_step.cpp; before, only the main thread got game time);
//  * a helper's wait / sleep with a RELATIVE timeout ends when GAME time has
//    passed it: the wait runs in 1 ms real slices until the object signals or
//    fixed_step::GameClockNs() reaches the deadline. A helper inside a wait
//    whose deadline the game time has reached counts as BUSY in
//    WaitUntilQuiet: the frame waits until its timeout work is done.
// Zero / absolute / no timeouts are untouched. Main thread and the port's
// own threads: untouched.
bool OnGameTime();

// A wait import's wrapper body (CMakeLists.txt generates the call):
// `real` = the SDK's import, `timeout_reg` = the argument register (3-10)
// holding the guest pointer to the big-endian 64-bit timeout (100 ns units,
// negative = relative), `timed_sleep` as above. Calls BeforeWait / AfterWait.
using RealImport = void (*)(PPCContext&, uint8_t*);
void Wait(PPCContext& ctx, uint8_t* base, RealImport real, int timeout_reg, bool timed_sleep);

// Main thread, start of a main-loop pass, fixed step on: see the file header.
void WaitUntilQuiet();

// HELPERS WORK ONLY BETWEEN FRAMES (2026-11-17; helper_threads_between_frames).
// Waiting for idle helpers at each frame start wasn't enough: on several CPU
// cores a helper also worked DURING the main thread's frame, and the level's
// loading task (main thread, sub_822E39A8) found its data ready in the same
// frame or the next depending on timing: a package section became ready at
// session frame 295 in a fast run and 296 in paced ones, and later a spawner
// spawned a frame apart (the Wumpa replay parted at 3928). Now a GATE: the
// pass hook opens it for the frame boundary (sound mixing + WaitUntilQuiet)
// and closes it before the frame's game code; a helper woken while it is
// closed is held (still counted inside its wait) until it opens. When the
// main thread itself waits (Wait() above, usually for a helper), the gate
// opens, and after the wait the helpers finish everything before it closes.
// Deadlock guard: a held helper is let go (logged) only if the main thread has
// been ASLEEP across two 100 ms checks (blocked, maybe on a lock the helper
// holds); a main thread that is busy keeps it held, however long.
void OpenGate();
void CloseGate();
// Main thread, inside a frame: open the gate, let the helpers finish all
// their work, close it again (nothing while it is open / gating is off).
// Called when the main thread SPINS on the clock (fixed_step.cpp: its
// 1,000th read in a frame and every 1,000 after): it is then polling for
// something, often a helper's result, and finds it at the same read on every
// PC.
void LetHelpersFinish();

}  // namespace helper_threads
