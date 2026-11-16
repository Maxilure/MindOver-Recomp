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
// Not counted as work: a thread whose wait ended by its TIMEOUT (a periodic
// poll) and a thread busy for more than kGiveUpBusy without any wait (a thread
// that ended, or one that never sleeps: logged once). A frame never waits
// longer than kMaxWait (logged).
//
// Linux only for now: Windows' linker has no --wrap (no Xbox calls meter
// there); WaitUntilQuiet then returns at once.
// =============================================================================
#pragma once

#include <cstdint>

namespace helper_threads {

// The wrappers of the game's wait imports (any thread). `timed_sleep` = the
// call is KeDelayExecutionThread (a plain sleep: its end is never a wake).
void BeforeWait();
void AfterWait(uint32_t status, bool timed_sleep);

// Main thread, start of a main-loop pass, fixed step on: see the file header.
void WaitUntilQuiet();

}  // namespace helper_threads
