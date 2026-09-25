// =============================================================================
// frame_rate.cpp -- the game's 30 fps pacing, and --fps_cap=60 to lift it
// =============================================================================
//
// Three things pace this game's frames (found 2026-09-25, docs/findings/07):
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
//    We wrap it only to COUNT presented frames (--debug_log_fps). The
//    generated code declares every `sub_X` as a *weak* alias of the original
//    `__imp__sub_X`, so our `sub_82310728` overrides it everywhere
//    (including the function-pointer call) and still calls the original.
//
// Flags:
//   --fps_cap=30       original pacing (default; every hook is a no-op)
//   --fps_cap=60       60 fps: renderer mode 0 + main-loop limiter at 60
//   --fps_cap=0        no main-loop limiter; renderer still vsyncs at 60 Hz
//   --debug_log_fps    every 5 s: presented fps, main-loop timing, D3D settings
//
// Measured: title ~59 fps, gameplay ~56 fps (some frames miss a vblank).
// What 60 fps does to physics, animation and cutscenes is for playtesting to
// tell (docs/03-roadmap.md phase 3).
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(fps_cap, 30, "CrashMoM",
                     "Frame-rate cap: 30 = original, 60 = 60 fps, "
                     "0 = no main-loop limiter (vsync still applies)")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(debug_log_fps, false, "CrashMoM",
                    "Debug: log frame rate and pacing details every 5 s");

// -----------------------------------------------------------------------------
// 1. The renderer's vsync mode (midasm hook at 0x82433534)
// -----------------------------------------------------------------------------

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

    // This runs once per presented frame, so counting calls counts the real
    // fps. Only ever called from the GPU interrupt, one frame at a time, so
    // plain statics are enough.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static uint32_t frames = 0;
    ++frames;
    double seconds = std::chrono::duration<double>(Clock::now() - window_start).count();
    if (seconds >= 5.0) {
      REXLOG_INFO("frame_rate: {:.1f} fps (last {:.1f} s)", frames / seconds, seconds);
      frames = 0;
      window_start = Clock::now();
    }
  }
  __imp__sub_82310728(ctx, base);
}
