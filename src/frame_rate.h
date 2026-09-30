// =============================================================================
// frame_rate.h -- frame statistics (the rest of frame_rate.cpp needs no calls)
// =============================================================================
//
// frame_rate.cpp holds the game's frame pacing hooks (--fps_cap) and, with
// --debug_log_fps and/or --debug_fps_csv, measures how long every game frame
// took: average frame rate, 1% and 0.1% lows, worst frame. The pacing hooks
// are wired in by the recompiler (manifest); the statistics listen to the
// game's frame ends, which is what these two calls start and stop.
// =============================================================================

#pragma once

namespace frame_rate {

// After setup (the renderer interception exists): starts listening to frame
// ends if --debug_log_fps or --debug_fps_csv asks for statistics.
void StartFrameStats();

// On shutdown: stops listening (waits for a frame end in progress), logs the
// whole session's numbers once more and closes the CSV file.
void StopFrameStats();

}  // namespace frame_rate
