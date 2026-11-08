// =============================================================================
// debug/slow_load_sampler.h -- when the title's loading crawls, name what the
// game's main thread is doing
// =============================================================================
//
// WHY (2026-10-30): once, after a long play session, the way back to the
// title after the attract movie took 23 s instead of 3-6 s. The front end
// sat in its loading states (51 -> 52 -> 53) while the game's own time per
// frame CLIMBED (13 -> 30 -> 48 -> 88 -> 153 ms): something piled up from
// frame to frame. It didn't happen again in tests (3.3-6.9 s), so the log
// couldn't say what it was.
//
// WHAT: while the front end is in one of those states and three frames in a
// row take the game more than kSlowMs, the game's main thread is sampled
// for a few seconds: a timer thread sends it a signal every 2 ms, the
// handler keeps its host call stack (backtrace), and at the end the log gets
// the game functions it was in most often (the innermost recompiled one of
// each sample: sub_XXXXXXXX) and the innermost host functions (ours or the
// SDK's). Once per minute at most. Linux only; --debug_slow_load_sampler.
//
// Log lines: "Slow load sampler: ...".
//
// PROFILE RUNS (2026-11-09): the same sampler on demand, for frame-rate work:
// console `profile <seconds> [file]` samples the main thread every 1 ms (any
// state, gameplay included) and writes a report: game functions by SELF time
// (innermost) and INCLUSIVE time (anywhere on the stack), host functions the
// same way, plus the game frames' average timings over the run. Measure with
// --fps_cap=0 (uncapped), or the frame pacer's sleep shows up as time.
// =============================================================================
#pragma once

#include <string>

namespace slow_load_sampler {

// After setup (CrashMomApp::OnPostSetup): the frame-end listener + the signal.
void Install();

// Console `profile`: starts a run on its own thread (returns at once with a
// line for the console, or why it can't). The report goes to `path`.
std::string StartProfile(double seconds, const std::string& path);
// A profile (or slow-load) run is still sampling or writing its report.
bool ProfileRunning();

}  // namespace slow_load_sampler
