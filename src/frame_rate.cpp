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
//    vsync, one vblank per frame (60 fps); mode 2 presents right away (no
//    vsync; loading screens). The game uses mode 1 for title, menus and
//    gameplay.
//    -> CrashMomVsyncMode: a "midasm" hook (crash_mom_manifest.toml,
//       [[entrypoint.midasm_hook]] at 0x82433534) that codegen calls right
//       before `cmpwi cr6,r10,1`, passing r10 (the mode) by reference. With
//       --fps_cap other than 30 it turns 1 into 2: each frame is shown the
//       moment it's done, and the CLOCK PACER (1b) decides when frames start.
//
// 1b. THE GAME'S MINIMUM FRAME STEP, the real 60 fps ceiling: the frame
//    function sub_8227C5D8 skips frames until 1/60 s has passed (2026-09-30).
//    -> CrashMomFrameStep: midasm hook at 0x8227C670, where OUR CLOCK PACER
//       decides instead: a frame every 1/fps_cap s, by the clock, sleeping
//       in between (section 1b below).
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
//    The spin is why the main thread showed ~100% CPU (findings/05); with
//    --fps_cap the clock pacer sleeps instead (section 1b).
//    -> CrashMomFrameLimiter: midasm hook at 0x8227B0DC that replaces r29 by
//       a verdict for OUR frame time: 33334 ("late enough") or 0 ("too soon").
//
// 3. D3D's VBLANK FLIP QUEUE (Microsoft's library, linked into the game):
//    sub_82310728 picks the vblank each presented frame may appear at. The
//    game asks for interval 1 (60 Hz), or 0 (immediate) in mode 2; nothing
//    to change there. We wrap it only to LOG its settings (--debug_log_fps;
//    it used to count frames too, before section 4 measured each one). The
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
//   --fps_cap=30       original pacing (default; the hooks change nothing)
//   --fps_cap=N        any other rate (60, 144, 165, 45, ...): renderer mode 2
//                      (present right away) and our clock pacer (section 1b)
//                      starting a frame every 1/N s, asleep in between.
//                      Main-loop limiter (loading, movies) at N too.
//                      Above 60 is EXPERIMENTAL (anything in the game that
//                      counts frames instead of time could misbehave).
//   --fps_cap=0        no limit: every main-loop pass runs a frame, as fast as
//                      the game and the PC can go
//   Any cap: the virtual Xbox screen stays at the standard 60 Hz
//   (SyncGuestRefresh, SDK patch 0010), whatever video_mode_refresh_rate says.
//   --debug_log_fps    every 5 s: average fps, 1% / 0.1% lows and the worst
//                      frame, for those 5 s and for the whole session so far;
//                      plus pacing details (main-loop timing, D3D settings)
//                      and a second line: the main thread's average frame cut
//                      at SwapBuffers (game / waiting for its start time /
//                      frame-end work / SwapBuffers, pddi::FrameTiming)
//   --debug_fps_csv=<file>  every frame's time, one line each, for graphs
//                      (with the same parts; frame_end_ms is the
//                      PREVIOUS frame's; pace_wait_ms = the clock pacer's
//                      wait before this frame, included in game_ms)
//
// Measured: see docs/findings/07 (tables for 60, 144, uncapped, and the clock
// pacer). What high frame rates do to physics, animation and cutscenes is for
// playtesting to tell (docs/03-roadmap.md phase 3).
// =============================================================================

#include "frame_rate.h"

#include "cheats/cheats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>  // _mm_pause (CpuRelax)
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "pddi/intercept.h"

REXCVAR_DEFINE_INT32(fps_cap, 30, "CrashMoM",
                     "Frame-rate cap: 30 = original, any other rate (60, 144, 165, ...) = a frame "
                     "every 1/rate s by the clock (above 60 EXPERIMENTAL), 0 = as fast as possible")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(debug_log_fps, false, "CrashMoM",
                    "Log the frame rate every 5 s: average, 1% / 0.1% lows, worst frame "
                    "(last 5 s and whole session), plus pacing details");
REXCVAR_DEFINE_STRING(debug_fps_csv, "", "CrashMoM",
                      "Write every game frame's time to this CSV file (seconds, frame ms, game / swap / "
                      "frame-end ms, clock pacer's wait)");

// -----------------------------------------------------------------------------
// 1. The renderer's vsync mode (midasm hook at 0x82433534)
// -----------------------------------------------------------------------------

// WHY MODE 2 ABOVE 30 (the clock pacer, 2026-09-30). In modes 0 and 1 D3D
// shows a finished frame only at the next vblank (the virtual Xbox screen's
// refresh), and the SDK fires vblanks at the video mode's rate, 60 Hz. The
// history of getting past that:
// * First, mode 1 -> 0 (plain vsync): 60 fps, one of TWO 60 fps walls (the
//   other is the game's minimum frame step, section 1b). Gameplay ~56-59:
//   a frame that took 17 ms waited for the vblank after next = 33 ms.
// * Then a faster virtual screen (SDK patch 0010, guest_refresh_hz = the
//   cap). Flaw, found in a playtest at --fps_cap=180 on Wumpa Island: stuck at
//   exactly 90. A frame there needed ~5.6 ms, a hair over one 180 Hz tick
//   (5.56 ms), so each frame waited for the NEXT tick: 2 ticks per frame =
//   half the cap. Uncapped, the same place ran at 120-200 fps. Any rate paced
//   by ticks falls to rate/2 the moment a frame runs a little long.
// * Even at 1000 Hz a frame waits up to 1 ms for its tick: measured on the
//   title with only our renderer drawing, uncapped: 597 fps, 1.2 ms of every
//   frame inside SwapBuffers (the game's own work: 0.3 ms).
// * Now: mode 2, "present right away" (D3D interval IMMEDIATE). No ticks to
//   wait for at all; WHEN frames start is the clock pacer's job (section
//   1b). Same title: 1736 fps, SwapBuffers 0.1 ms; the rate of the virtual
//   screen no longer matters (1705 fps at 60 Hz). With the emulated GPU
//   drawing, nothing changed that its own speed doesn't already limit.
// Mode 2 is the game's own path: its loading screens use it, and its mode 1
// shows a frame that finished LATE the same way (sub_82433510: the late
// branch at 0x82433594 skips `li r29,1`, leaving r29 = 0x80000000 =
// IMMEDIATE). r10 is only read by the mode compares at 0x82433534 (== 1) and
// 0x82433598 (== 0), so 2 simply takes neither branch.
// Why this is safe for the game's speed: its logic uses the REAL time between
// frames (sections 1b and 2), not vblank counts. The only vblank counter the
// game reads is this mode 1 wait, which mode 2 skips.
// The host window shows the frames at the MONITOR's rate: on a 60 Hz monitor
// 144 game fps still look like 60 (smoother input, some tearing).
//
// THE VIRTUAL SCREEN stays at the standard 60 Hz for every cap: mode 1's "2
// vblanks per frame" IS the original 30 fps pacing, and the game's own mode 0
// (used around the intro movies) is plain 60 Hz vsync. Pinned with patch
// 0010's flag, because the SDK's display setting video_mode_refresh_rate
// (what the game is told its TV runs at) also sets the vblank rate: 180 there
// would turn the original pacing into 90 fps. Left alone when the player set
// --guest_refresh_hz themselves.
static void SyncGuestRefresh() {
  static bool pinned = false;  // main thread only (SwapBuffers)
  if (pinned) {
    return;
  }
  pinned = true;
  if (rex::cvar::GetFlagSource("guest_refresh_hz") != rex::cvar::Source::kDefault &&
      rex::cvar::GetFlagSource("guest_refresh_hz") != rex::cvar::Source::kRuntime) {
    return;  // the player chose a rate on the command line / config: theirs
  }
  if (!rex::cvar::SetFlagByName("guest_refresh_hz", "60")) {
    REXLOG_WARN("frame_rate: can't hold the virtual Xbox screen at 60 Hz (needs SDK patch "
                "0010): the original 30 fps pacing follows video_mode_refresh_rate");
  }
}

// Plain C++ linkage and this exact signature: codegen declares
// `extern void CrashMomVsyncMode(PPCRegister& r10);` in the generated file.
void CrashMomVsyncMode(PPCRegister& r10) {
  const int32_t mode = r10.s32;
  const int32_t cap = REXCVAR_GET(fps_cap);
  SyncGuestRefresh();

  // Log the cap whenever it changes (start, or the F4 settings), and the
  // game's mode whenever it changes (menus, gameplay, loading...). Only the
  // main thread presents, so plain statics are enough.
  static int32_t last_cap = -1;
  if (cap != last_cap) {
    REXLOG_INFO("frame_rate: --fps_cap={}: {}", cap,
                cap == 30  ? "the original pacing (2 screen refreshes per frame)"
                : cap == 0 ? "no limit, frames shown right away"
                           : fmt::format("a frame every {:.2f} ms by the clock, shown right away",
                                         1000.0 / cap));
    last_cap = cap;
  }
  static int32_t last_mode = -12345;
  if (mode != last_mode) {
    REXLOG_INFO("frame_rate: renderer vsync mode {} ({}){}", mode,
                mode == 1   ? "2 vblanks per frame = 30 fps"
                : mode == 0 ? "1 vblank per frame = 60 fps"
                            : "no vsync",
                mode == 1 && cap != 30 ? ", using mode 2 (--fps_cap: the clock paces)" : "");
    last_mode = mode;
  }

  if (mode == 1 && cap != 30) {
    r10.u64 = 2;
  }
}

// -----------------------------------------------------------------------------
// 1b. When a frame may start: the game's minimum frame step, and our clock
//     pacer (midasm hook at 0x8227C670)
// -----------------------------------------------------------------------------
//
// ABOVE 60 FPS, the real wall (found 2026-09-30, manifest comment): the game's
// frame function sub_8227C5D8 adds up the time since its last frame and skips
// the frame until at least 1/60 s has built up (f30 = the constant at
// 0x8201FB80). The main loop meanwhile spins (clock reads, subsystem
// updates). So the game itself never runs more than 60 frames a second,
// whatever the vsync mode or refresh rate.
//
// How the game keeps time there (disassembly of sub_8227C5D8):
//   0x8227C658  lfs  f0,160(r31)   ; the time added up since the last frame
//   0x8227C65C  fadds f0,f31,f0    ;   + this main-loop pass's real delta
//   0x8227C664  stfs f0,160(r31)   ;   (kept, frame or not)
//   0x8227C670  fcmpu cr6,f0,f30   ; <- this hook runs right before
//   0x8227C674  blt  -> return     ; too soon: no frame this pass
//   ...the frame runs with f0 (clamped to 0.1 s) as its time step...
//   0x8227C7DC  stfs 0.0 -> 160(r31) ; reset AFTER the frame
// So whenever a frame runs, its step is exactly the real time since the
// previous frame started. That's what lets us choose freely WHEN frames run
// (the game's speed stays right) by setting f30 to 0 ("run now": f0 >= 0
// always) or to something huge ("not yet"). f30 isn't used again in the
// function (only restored at exit).
//
// THE CLOCK PACER (--fps_cap other than 30 and 0): frame k may start at
// start_0 + k/fps_cap. On the time, not "1/fps_cap after the last frame", so the
// average is exactly the cap: a frame that started a little late leaves the
// next one a little less wait. A frame that ran over a whole period is
// forgiven (the schedule restarts from now) rather than followed by a burst.
// There are no ticks to miss (frames are shown right away: mode 2, section 1):
// a frame that takes 5.8 ms at a 180 cap (5.56 ms) runs at 1/5.8 ms = 172 fps,
// where the old tick pacing fell to 90.
//
// Between frames the main thread SLEEPS until 1 ms before the next start, then
// spins the last millisecond in a tight clock loop, all inside the first "not
// yet" pass after a frame. The very next main-loop pass then reads the game's
// clock right at the start time and runs the frame, so every frame's time
// step is measured from the same moment. (First try: return "not yet" after
// the sleep and let the game's own loop spin the last millisecond. Frame
// starts then wandered by up to one pass, ~1 ms: each pass also updates the
// engine's subsystems before asking again. Title at 144, only our renderer:
// start-to-start 5.8-8.0 ms for 1-99% of frames.) The original game doesn't spin between
// frames either: at 30 fps it waits inside SwapBuffers (Sleep(0) until the
// vblank), one main-loop pass per frame. Before this, the main thread burned
// a whole CPU core at any cap. Measured on Linux (2026-09-30): a sleep wakes
// ~6 us late (99% within 40 us, rare outliers up to 0.8 ms), so 1 ms of spin
// keeps frame starts exact. (Windows port: plain Sleep() is coarse there;
// needs a high-resolution waitable timer or a bigger spin margin.)
//
// Where the wait happens matters for input lag: here, BEFORE the frame reads
// the controller, so a frame's input is as fresh as it can be; a wait at
// SwapBuffers (like the original's) would sit between input and picture.
//
// --fps_cap=0: f30 = 0, every pass runs a frame, no sleeping.
// --fps_cap=30: untouched (the game's 1/60 s minimum; mode 1 makes it 30).
namespace {

using PaceClock = std::chrono::steady_clock;

// Spin (don't sleep) this close to a frame's start time.
constexpr auto kPacerSpin = std::chrono::milliseconds(1);

struct Pacer {
  int32_t cap = -1;            // the --fps_cap the schedule is for (F4 can change it)
  PaceClock::time_point next;  // when the next frame may start
  bool waiting = false;        // a "not yet" pass happened since the last frame
  PaceClock::time_point wait_start;
  double last_wait_ms = 0;     // how long the latest frame waited for its start time
};
Pacer g_pacer;  // the game's main thread only (the frame function runs there)

// Inside a spin-wait: tell the CPU we're waiting (lets the other hyperthread
// of the core run faster, saves a little power).
inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#endif
}

// One main-loop pass asks: may a frame run now? Sleeps when there's time.
bool PacerAllowsFrame(int32_t cap) {
  PaceClock::time_point now = PaceClock::now();
  if (cap != g_pacer.cap) {  // a new cap: start the schedule now
    g_pacer.cap = cap;
    g_pacer.next = now;
    g_pacer.waiting = false;
  }
  if (now < g_pacer.next) {
    if (!g_pacer.waiting) {
      g_pacer.waiting = true;
      g_pacer.wait_start = now;
    }
    if (g_pacer.next - now > kPacerSpin) {
      std::this_thread::sleep_until(g_pacer.next - kPacerSpin);
    }
    while (PaceClock::now() < g_pacer.next) {
      CpuRelax();
    }
    return false;  // the next pass reads the game's clock now, and runs the frame
  }
  g_pacer.last_wait_ms =
      g_pacer.waiting ? std::chrono::duration<double, std::milli>(now - g_pacer.wait_start).count()
                      : 0.0;
  g_pacer.waiting = false;
  const auto period = std::chrono::duration_cast<PaceClock::duration>(
      std::chrono::duration<double>(1.0 / double(cap)));
  g_pacer.next += period;
  if (g_pacer.next <= now) {
    g_pacer.next = now + period;  // over a whole period late: forgive, no burst
  }
  return true;
}

}  // namespace

// Codegen declares `extern void CrashMomFrameStep(PPCRegister& f30);`.
void CrashMomFrameStep(PPCRegister& f30) {
  const int32_t cap = REXCVAR_GET(fps_cap);
  if (cap == 30) {
    // The original: the game's 1/60 s minimum stands. EXCEPT while frozen by
    // the cheat menu (cheats.h): then each frame adds only 1e-7 s and would
    // never reach the minimum (no frame = no picture, no free camera); the
    // swap's two-vblank wait still paces it at 30.
    if (cheats::Frozen()) f30.f64 = 0.0;
    return;
  }
  if (cap == 0) {
    f30.f64 = 0.0;  // no limit: this pass runs a frame
    return;
  }
  f30.f64 = PacerAllowsFrame(cap) ? 0.0 : 1e30;
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
  // sums (for averages) and the worst frame-end work. pace_wait = the clock
  // pacer's wait before each frame (section 1b), which FrameTiming counts as
  // "game" (it happens in the main loop): taken out of it for the log.
  double window_game_ms = 0, window_swap_ms = 0, window_listeners_ms = 0;
  double window_pace_wait_ms = 0;
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

  // Where the main thread's time went (pddi/intercept.h, FrameTiming), and
  // how much of its "game" part was the pacer waiting for this frame's start
  // time (read once: frames that don't come through the pacer count 0).
  const pddi::FrameTiming& timing = pddi::LastFrameTiming();
  const double pace_wait_ms = std::min(g_pacer.last_wait_ms, timing.game_ms);
  g_pacer.last_wait_ms = 0;

  if (g_stats.csv) {
    std::fprintf(g_stats.csv, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                 std::chrono::duration<double>(now - g_stats.first).count(), ms, timing.game_ms,
                 timing.swap_ms, timing.listeners_ms, pace_wait_ms);
  }

  if (g_stats.log) {
    g_stats.window.push_back(float(ms));
    g_stats.window_ms += ms;
    g_stats.window_game_ms += timing.game_ms - pace_wait_ms;
    g_stats.window_pace_wait_ms += pace_wait_ms;
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
      // A second line: the average frame's main thread, cut in four. "game"
      // high = the game (or the recorder, or D3D waiting for the emulated
      // GPU) is the limit; "waiting for its start time" = the clock pacer
      // (--fps_cap, section 1b): spare time, mostly asleep; "frame-end work"
      // = our renderer's OnFrameEnd (its own breakdown: the NativeRenderer
      // lines); "SwapBuffers" = presenting: at the original 30 fps mostly
      // waiting for the next screen refresh; with --fps_cap (shown right
      // away) ~0, unless D3D waits for the emulated GPU to free a buffer.
      const double n = double(g_stats.window.size());
      REXLOG_INFO("frame_rate: last {:.1f} s, main thread per frame: game {:.1f} ms, waiting "
                  "for its start time {:.1f} ms, frame-end work {:.1f} ms (worst {:.1f}), "
                  "SwapBuffers (waiting for the screen or the GPU) {:.1f} ms",
                  g_stats.window_ms / 1000.0, g_stats.window_game_ms / n,
                  g_stats.window_pace_wait_ms / n, g_stats.window_listeners_ms / n,
                  g_stats.window_worst_listeners_ms, g_stats.window_swap_ms / n);
      g_stats.window.clear();
      g_stats.window_ms = 0;
      g_stats.window_game_ms = g_stats.window_swap_ms = g_stats.window_listeners_ms = 0;
      g_stats.window_pace_wait_ms = 0;
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
      std::fputs("seconds,frame_ms,game_ms,swap_ms,frame_end_ms,pace_wait_ms\n", g_stats.csv);
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
