// =============================================================================
// frame_rate.cpp -- the game's 30 fps pacing, and --fps_cap to lift it (60, 144, ...)
// =============================================================================
//
// Four things pace this game's frames (found 2026-09-25, docs/findings/07):
//
// 1. THE CAP IN MENUS AND GAMEPLAY: the vsync mode of Radical's renderer
//    (PDDI). Its "swap buffers" routine sub_82433510 reads a mode from
//    this+1660. Mode 1 waits on a vblank counter so each frame spans TWO
//    vblanks (30 fps, with catch-up when a frame runs late); mode 0 is plain
//    vsync, one vblank per frame (60 fps). The game uses mode 1 for title,
//    menus and gameplay.
//    -> CrashMomVsyncMode: a "midasm" hook (crash_mom_manifest.toml,
//       [[entrypoint.midasm_hook]] at 0x82433534) that codegen calls right
//       before `cmpwi cr6,r10,1`, passing r10 (the mode) by reference. With
//       --fps_cap other than 30 it turns 1 into 0: the game's own 60 fps path.
//
// 1b. THE GAME'S MINIMUM FRAME STEP, the real 60 fps ceiling: the frame
//    function sub_8227C5D8 skips frames until 1/60 s has passed (2026-09-30).
//    -> CrashMomFrameStep: midasm hook at 0x8227C670 lowers it above 60.
//
// 2. THE MAIN-LOOP LIMITER, on the loop's other path (loading screens,
//    movies). The main loop (sub_8227AEE0) reads a microsecond clock
//    (sub_8235AAE0 = QueryPerformanceCounter scaled to us) and computes
//    r29 = microseconds since the last frame:
//      0x8227B0DC  cmplwi cr6,r29,33333   ; 1/30 of a second
//      0x8227B0E0  ble    0x8227AFC0      ; too soon: spin, read the clock again
//    Each frame gets the real elapsed time as its delta (x 1e-6 -> seconds),
//    so game logic is time-based: moving at 60 fps covers the same distance
//    per second as at 30 (verified with the same inputs on the same save).
//    The spin is why the main thread always shows ~100% CPU (findings/05).
//    -> CrashMomFrameLimiter: midasm hook at 0x8227B0DC that replaces r29 by
//       a verdict for OUR frame time: 33334 ("late enough") or 0 ("too soon").
//
// 3. D3D's VBLANK FLIP QUEUE (Microsoft's library, linked into the game):
//    sub_82310728 picks the vblank each presented frame may appear at. The
//    game already asks for interval 1 (60 Hz), so nothing to change there.
//    We wrap it only to LOG its settings (--debug_log_fps; it used to count
//    frames too, before section 4 measured each one). The
//    generated code declares every `sub_X` as a *weak* alias of the original
//    `__imp__sub_X`, so our `sub_82310728` overrides it everywhere
//    (including the function-pointer call) and still calls the original.
//
// 4. FRAME STATISTICS (--debug_log_fps, --debug_fps_csv; section 4 below):
//    how long each game frame took, measured when the game finishes it
//    (after xnDisplay::SwapBuffers, the moment the native renderer hands its
//    picture to the window). Tools like MangoHud count the WINDOW's repaints
//    instead, which were not the game's frames: an SDK overlay that is always
//    open (the achievement toast) made the window repaint non-stop, 600+
//    times per second, each showing the same frame (fixed by SDK patch 0006).
//
// Flags:
//   --fps_cap=30       original pacing (default; every hook is a no-op)
//   --fps_cap=60       60 fps: renderer mode 0 + main-loop limiter at 60.
//                      Gameplay can't go faster at the standard refresh: its
//                      frames wait for the emulated 60 Hz vblank.
//   --fps_cap=120/144  EXPERIMENTAL (2026-09-30): as 60, plus the emulated
//                      vblank at that rate (SDK patch 0010, SyncGuestRefresh
//                      below), so gameplay can reach it too.
//   --fps_cap=0        no main-loop limiter, vblank at 1000 Hz: as fast as
//                      the game and the PC can go
//   --debug_log_fps    every 5 s: average fps, 1% / 0.1% lows and the worst
//                      frame, for those 5 s and for the whole session so far;
//                      plus pacing details (main-loop timing, D3D settings)
//                      and a second line: the main thread's average frame cut
//                      at SwapBuffers (game / frame-end work / SwapBuffers,
//                      pddi::FrameTiming)
//   --debug_fps_csv=<file>  every frame's time, one line each, for graphs
//                      (with the same three parts; frame_end_ms is the
//                      PREVIOUS frame's)
//
// Measured: title ~59 fps, gameplay ~56 fps (some frames miss a vblank).
// What 60 fps does to physics, animation and cutscenes is for playtesting to
// tell (docs/03-roadmap.md phase 3).
// =============================================================================

#include "frame_rate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "pddi/intercept.h"

REXCVAR_DEFINE_INT32(fps_cap, 30, "CrashMoM",
                     "Frame-rate cap: 30 = original, 60 = 60 fps, above 60 (e.g. 120, 144) = "
                     "EXPERIMENTAL: the virtual Xbox screen refreshes that often so gameplay can "
                     "follow, 0 = as fast as possible (1000 Hz refresh, no limiter)")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(debug_log_fps, false, "CrashMoM",
                    "Log the frame rate every 5 s: average, 1% / 0.1% lows, worst frame "
                    "(last 5 s and whole session), plus pacing details");
REXCVAR_DEFINE_STRING(debug_fps_csv, "", "CrashMoM",
                      "Write every game frame's time to this CSV file (seconds, frame ms, game / swap / frame-end ms)");

// -----------------------------------------------------------------------------
// 1. The renderer's vsync mode (midasm hook at 0x82433534)
// -----------------------------------------------------------------------------

// ABOVE 60 FPS (2026-09-30, work in progress): in mode 0 every frame waits for the next vblank, and the SDK
// fires the guest's vblanks at the video mode's 60 Hz: one of TWO 60 fps
// walls (the other is the game's minimum frame step, section 1b; checked: with
// 1b lifted but --guest_refresh_hz=60, gameplay stays at 60.0).
// SDK patch 0010 adds the GPU flag guest_refresh_hz; with --fps_cap above 60
// we set it to the cap, so the virtual Xbox "screen" refreshes 120 / 144 /
// ... times a second and the game's own vsync paces it there. --fps_cap=0 =
// 1000 Hz (as fast as the game can go). 30 / 60 leave the SDK at 60 Hz.
// Checked every swap (cheap: only a change calls into the flag registry), so
// changing --fps_cap in the F4 settings applies right away. Left alone when
// the user set --guest_refresh_hz themselves.
// Why this is safe for the game's speed: its logic uses the REAL time between
// frames (section 2), not vblank counts. What else might count vblanks
// (animations? audio?) is exactly what playtesting above 60 is for.
// The host window shows the frames at the MONITOR's rate: on a 60 Hz monitor
// 144 game fps still look like 60 (smoother input, some tearing).
static void SyncGuestRefresh() {
  static int32_t applied_cap = -1;
  const int32_t cap = REXCVAR_GET(fps_cap);
  if (cap == applied_cap) {
    return;
  }
  applied_cap = cap;
  if (rex::cvar::GetFlagSource("guest_refresh_hz") != rex::cvar::Source::kDefault &&
      rex::cvar::GetFlagSource("guest_refresh_hz") != rex::cvar::Source::kRuntime) {
    return;  // the user chose a rate on the command line / config: theirs
  }
  const int32_t hz = cap == 0 ? 1000 : cap > 60 ? cap : 0;  // 0 = the SDK's 60 Hz
  if (!rex::cvar::SetFlagByName("guest_refresh_hz", std::to_string(hz))) {
    if (hz) {
      REXLOG_WARN("frame_rate: --fps_cap={} needs SDK patch 0010 (guest_refresh_hz); "
                  "gameplay stays at 60",
                  cap);
    }
    return;
  }
  REXLOG_INFO("frame_rate: virtual Xbox screen refresh {} (--fps_cap={})",
              hz ? std::to_string(hz) + " Hz" : std::string("60 Hz (standard)"), cap);
}

// Plain C++ linkage and this exact signature: codegen declares
// `extern void CrashMomVsyncMode(PPCRegister& r10);` in the generated file.
void CrashMomVsyncMode(PPCRegister& r10) {
  const int32_t mode = r10.s32;

  // Log the game's mode whenever it changes (menus, gameplay, loading...).
  // Only the main thread presents, so a plain static is enough.
  static int32_t last_mode = -12345;
  if (mode != last_mode) {
    REXLOG_INFO("frame_rate: renderer vsync mode {} ({}){}", mode,
                mode == 1   ? "2 vblanks per frame = 30 fps"
                : mode == 0 ? "1 vblank per frame = 60 fps"
                            : "no vsync",
                mode == 1 && REXCVAR_GET(fps_cap) != 30 ? ", using mode 0 (--fps_cap)" : "");
    last_mode = mode;
  }

  if (mode == 1 && REXCVAR_GET(fps_cap) != 30) {
    r10.u64 = 0;
  }
  SyncGuestRefresh();
}

// -----------------------------------------------------------------------------
// 1b. The game's minimum frame step (midasm hook at 0x8227C670)
// -----------------------------------------------------------------------------
//
// ABOVE 60 FPS, the real wall (found 2026-09-30, manifest comment): the game's
// frame function sub_8227C5D8 adds up the time since its last frame and skips
// the frame until at least 1/60 s has built up (f30 = the constant at
// 0x8201FB80). The main loop meanwhile spins (clock reads, subsystem
// updates). So the game itself never runs more than 60 frames a second,
// whatever the vsync mode or refresh rate. With --fps_cap above 60 we lower
// that minimum to 1/fps_cap (0 for fps_cap=0 = every pass of the main loop).
// The frame then gets the real, smaller time step (the game's logic is
// time-based; each step stays clamped to 0.1 s by the game).
//
// Codegen declares `extern void CrashMomFrameStep(PPCRegister& f30);`.
void CrashMomFrameStep(PPCRegister& f30) {
  const int32_t cap = REXCVAR_GET(fps_cap);
  if (cap > 60) {
    f30.f64 = 1.0 / double(cap);
  } else if (cap == 0) {
    f30.f64 = 0.0;
  }
}

// -----------------------------------------------------------------------------
// 2. The main-loop limiter (midasm hook at 0x8227B0DC)
// -----------------------------------------------------------------------------

// Codegen declares `extern void CrashMomFrameLimiter(PPCRegister& r29);`.
void CrashMomFrameLimiter(PPCRegister& r29) {
  const int32_t cap = REXCVAR_GET(fps_cap);
  const uint32_t elapsed_us = r29.u32;
  if (cap != 30) {
    // The game's own threshold is 33333 us: it runs a frame when r29 > 33333.
    // Answer that comparison for our threshold instead.
    const uint32_t threshold_us = cap > 0 ? uint32_t(1000000 / cap) : 0;
    r29.u64 = elapsed_us > threshold_us ? 33334 : 0;
  }

  if (REXCVAR_GET(debug_log_fps)) {
    // Time between frames on this path, measured where the game decides to
    // run the next one. (This is how we saw that menus and gameplay never
    // come through here: they're paced by the renderer, part 1.)
    // Only the game's main thread runs this loop, so plain statics are fine.
    static uint32_t checks = 0, runs = 0;
    static uint64_t run_elapsed_sum = 0;
    static uint32_t run_elapsed_min = UINT32_MAX, run_elapsed_max = 0;
    static auto window_start = std::chrono::steady_clock::now();
    ++checks;
    if (r29.u32 > 33333) {  // this pass runs a frame
      ++runs;
      run_elapsed_sum += elapsed_us;
      run_elapsed_min = std::min(run_elapsed_min, elapsed_us);
      run_elapsed_max = std::max(run_elapsed_max, elapsed_us);
    }
    auto now = std::chrono::steady_clock::now();
    if (now - window_start >= std::chrono::seconds(5)) {
      REXLOG_INFO(
          "frame_rate: main-loop limiter path ran {} frames ({} checks); time between "
          "frames min/avg/max {:.1f}/{:.1f}/{:.1f} ms",
          runs, checks, runs ? run_elapsed_min / 1000.0 : 0.0,
          runs ? run_elapsed_sum / 1000.0 / runs : 0.0, run_elapsed_max / 1000.0);
      checks = runs = 0;
      run_elapsed_sum = 0;
      run_elapsed_min = UINT32_MAX;
      run_elapsed_max = 0;
      window_start = now;
    }
  }
}

// -----------------------------------------------------------------------------
// 3. D3D's per-frame vblank choice (function override, counting only)
// -----------------------------------------------------------------------------

// The original recompiled function (generated/default, DEFINE_REX_FUNC).
extern "C" REX_FUNC(__imp__sub_82310728);

extern "C" REX_FUNC(sub_82310728) {
  if (REXCVAR_GET(debug_log_fps)) {
    // r3 packs the frontbuffer address (bits 12-31), the presentation
    // interval (bits 8-11, vblanks per frame; 0 = immediate) and the
    // "immediate threshold" (bits 0-7, % of a refresh a late frame may still
    // flip in). Log them when they change.
    const uint32_t packed = ctx.r3.u32;
    static std::atomic<uint32_t> last_settings{0xFFFFFFFF};
    if (last_settings.exchange(packed & 0xFFF) != (packed & 0xFFF)) {
      REXLOG_INFO("frame_rate: D3D presents with interval {}, immediate threshold {}%",
                  (packed >> 8) & 0xF, packed & 0xFF);
    }

  }
  __imp__sub_82310728(ctx, base);
}

// -----------------------------------------------------------------------------
// 4. Frame statistics (--debug_log_fps, --debug_fps_csv)
// -----------------------------------------------------------------------------
//
// One sample per game frame: the time since the previous frame ended, taken
// when the game ends a frame (a pddi frame-end listener, right after
// xnDisplay::SwapBuffers, on the game's main thread). With the game's own
// vsync wait inside SwapBuffers, that's its real frame-to-frame time.
//
// The numbers, as frame-rate tools usually define them:
//   average   frames / seconds (not the mean of per-frame fps values)
//   1% low    the frame rate 99% of frames reach: 1000 / the 99th
//             percentile of frame times (ms). 0.1% low: the 99.9th.
//   worst     the longest single frame
// Loading screens count too (their frames run without vsync, and a level
// load shows up as one long frame), so compare lows from the same place.

namespace frame_rate {
namespace {

using Clock = std::chrono::steady_clock;

// Whole-session frame times as a histogram: 0.05 ms buckets up to 250 ms,
// longer frames (loading hitches) in the last one. Constant memory for any
// session length, and percentiles come straight from the counts.
constexpr double kBucketMs = 0.05;
constexpr size_t kBuckets = 5000;
constexpr double kWindowMs = 5000.0;  // one log line per 5 s of frames

struct Stats {
  bool running = false;
  bool log = false;
  std::FILE* csv = nullptr;
  bool have_previous = false;
  Clock::time_point previous, first;
  std::vector<float> window;  // frame times (ms) of the current 5 s
  double window_ms = 0;
  // The same 5 s, split the way pddi::FrameTiming cuts the main thread:
  // sums (for averages) and the worst frame-end work.
  double window_game_ms = 0, window_swap_ms = 0, window_listeners_ms = 0;
  double window_worst_listeners_ms = 0;
  std::array<uint32_t, kBuckets + 1> histogram{};
  uint64_t session_frames = 0;
  double session_ms = 0, session_worst_ms = 0;
};
Stats g_stats;  // main thread only (Start/Stop: before/after the listener runs)

// The frame time at percentile `p` (0..1) of a window's frames.
double WindowPercentile(std::vector<float> times, double p) {
  const size_t index = size_t(std::ceil(p * double(times.size()))) - 1;
  std::nth_element(times.begin(), times.begin() + index, times.end());
  return times[index];
}

// The same over the session's histogram (the bucket's middle; the worst
// frame if it falls among the hitches beyond the last bucket).
double SessionPercentile(double p) {
  const uint64_t wanted = uint64_t(std::ceil(p * double(g_stats.session_frames)));
  uint64_t seen = 0;
  for (size_t i = 0; i < kBuckets; ++i) {
    seen += g_stats.histogram[i];
    if (seen >= wanted) {
      return (double(i) + 0.5) * kBucketMs;
    }
  }
  return g_stats.session_worst_ms;
}

std::string Summary(uint64_t frames, double total_ms, double p99_ms, double p999_ms,
                    double worst_ms) {
  return fmt::format("{:.1f} fps average, 1% low {:.1f}, 0.1% low {:.1f}, worst frame {:.1f} ms",
                     frames * 1000.0 / total_ms, 1000.0 / p99_ms, 1000.0 / p999_ms, worst_ms);
}

std::string SessionSummary() {
  return fmt::format("session ({} frames, {:.0f} s): {}", g_stats.session_frames,
                     g_stats.session_ms / 1000.0,
                     Summary(g_stats.session_frames, g_stats.session_ms, SessionPercentile(0.99),
                             SessionPercentile(0.999), g_stats.session_worst_ms));
}

void OnFrameEnd(void*) {
  const Clock::time_point now = Clock::now();
  if (!g_stats.have_previous) {
    g_stats.have_previous = true;
    g_stats.previous = g_stats.first = now;
    return;
  }
  const double ms = std::chrono::duration<double, std::milli>(now - g_stats.previous).count();
  g_stats.previous = now;

  ++g_stats.session_frames;
  g_stats.session_ms += ms;
  g_stats.session_worst_ms = std::max(g_stats.session_worst_ms, ms);
  ++g_stats.histogram[std::min(size_t(ms / kBucketMs), kBuckets)];

  // Where the main thread's time went (pddi/intercept.h, FrameTiming).
  const pddi::FrameTiming& timing = pddi::LastFrameTiming();

  if (g_stats.csv) {
    std::fprintf(g_stats.csv, "%.4f,%.3f,%.3f,%.3f,%.3f\n",
                 std::chrono::duration<double>(now - g_stats.first).count(), ms, timing.game_ms,
                 timing.swap_ms, timing.listeners_ms);
  }

  if (g_stats.log) {
    g_stats.window.push_back(float(ms));
    g_stats.window_ms += ms;
    g_stats.window_game_ms += timing.game_ms;
    g_stats.window_swap_ms += timing.swap_ms;
    g_stats.window_listeners_ms += timing.listeners_ms;
    g_stats.window_worst_listeners_ms =
        std::max(g_stats.window_worst_listeners_ms, timing.listeners_ms);
    if (g_stats.window_ms >= kWindowMs) {
      const auto worst = *std::max_element(g_stats.window.begin(), g_stats.window.end());
      REXLOG_INFO("frame_rate: last {:.1f} s: {} | {}", g_stats.window_ms / 1000.0,
                  Summary(g_stats.window.size(), g_stats.window_ms,
                          WindowPercentile(g_stats.window, 0.99),
                          WindowPercentile(g_stats.window, 0.999), worst),
                  SessionSummary());
      // A second line: the average frame's main thread, cut in three. "game"
      // high = the game (or the recorder, or D3D waiting for the emulated
      // GPU) is the limit; "frame-end work" = our renderer's OnFrameEnd
      // (its own breakdown: the NativeRenderer lines); "SwapBuffers" = time
      // left over, spent waiting for the next screen refresh.
      const double n = double(g_stats.window.size());
      REXLOG_INFO("frame_rate: last {:.1f} s, main thread per frame: game {:.1f} ms, frame-end "
                  "work {:.1f} ms (worst {:.1f}), SwapBuffers (mostly waiting for the screen) "
                  "{:.1f} ms",
                  g_stats.window_ms / 1000.0, g_stats.window_game_ms / n,
                  g_stats.window_listeners_ms / n, g_stats.window_worst_listeners_ms,
                  g_stats.window_swap_ms / n);
      g_stats.window.clear();
      g_stats.window_ms = 0;
      g_stats.window_game_ms = g_stats.window_swap_ms = g_stats.window_listeners_ms = 0;
      g_stats.window_worst_listeners_ms = 0;
    }
  }
}

}  // namespace

void StartFrameStats() {
  const std::string& csv_path = REXCVAR_GET(debug_fps_csv);
  g_stats.log = REXCVAR_GET(debug_log_fps);
  if (!csv_path.empty()) {
    g_stats.csv = std::fopen(csv_path.c_str(), "w");
    if (g_stats.csv) {
      std::fputs("seconds,frame_ms,game_ms,swap_ms,frame_end_ms\n", g_stats.csv);
    } else {
      REXLOG_WARN("frame_rate: can't write {}", csv_path);
    }
  }
  if (!g_stats.log && !g_stats.csv) {
    return;
  }
  g_stats.running = true;
  pddi::AddFrameEndListener(&OnFrameEnd, nullptr);
}

void StopFrameStats() {
  if (!g_stats.running) {
    return;
  }
  pddi::RemoveFrameEndListener(&OnFrameEnd, nullptr);  // waits for a call in progress
  g_stats.running = false;
  if (g_stats.log && g_stats.session_frames) {
    REXLOG_INFO("frame_rate: at exit, {}", SessionSummary());
  }
  if (g_stats.csv) {
    std::fclose(g_stats.csv);
    g_stats.csv = nullptr;
  }
}

}  // namespace frame_rate
