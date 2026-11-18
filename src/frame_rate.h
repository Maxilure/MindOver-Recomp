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

#include <cstdint>

namespace frame_rate {

// After setup (the renderer interception exists): starts listening to frame
// ends if --debug_log_fps or --debug_fps_csv asks for statistics.
void StartFrameStats();

// On shutdown: stops listening (waits for a frame end in progress), logs the
// whole session's numbers once more and closes the CSV file.
void StopFrameStats();

// V-SYNC (--present_vsync, Options -> Display -> V-sync): pictures wait for
// the monitor's refresh (the window's present mode FIFO: no tearing) and the
// game is paced to the monitor's refresh rate (a cap above it, or no cap,
// becomes the refresh rate; 30 stays the original pacing).
// The present-mode flags for the current --present_vsync (the SDK's
// vulkan_allow_present_mode_*): before the window's swapchain is made (end of
// OnPreSetup), and again on a change (then the swapchain is made again).
void ApplyVsyncPresentMode();
// The monitor's refresh rate, read from SDL on the UI thread (the window's
// display); the pacer reads the cached value.
void UpdateDisplayRefresh();
// The cap the pacer uses: --fps_cap, limited to the monitor's refresh rate
// with V-sync on.
int32_t EffectiveCap();

// How long the latest frame waited for its start time (the clock pacer: part
// of a frame's "game" time that isn't the game working), once per frame:
// the second call for the same frame gets 0. For the HUD counter.
double TakePaceWaitMs();

}  // namespace frame_rate
