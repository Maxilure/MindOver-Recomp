// =============================================================================
// vblank.cpp -- the virtual TV's vertical blank, ticked by the port
// =============================================================================
//
// WHAT THIS IS
//   GRAPHICS DRIVER STEP 2 (README "Going native", plans.md s.9, docs/findings/
//   32 s.10). A TV redraws its picture 60 times a second; the moment between
//   two pictures is the VERTICAL BLANK. The Xbox 360's graphics driver tells
//   the game about each one by calling an INTERRUPT CALLBACK the game
//   registered with VdSetGraphicsInterruptCallback. The game (Direct3D inside
//   it) counts them, flips finished pictures on them and, at 30 fps, waits for
//   two of them per frame: they are the original game's frame clock.
//
//   Until now the SDK's fake Xbox did this (its "GPU VSync" thread). Now the
//   port does: the game's registration is answered at its two call sites
//   (manifest "GRAPHICS DRIVER STEP 2") and a "VBlank" thread of ours calls
//   the game's callback at the virtual TV's rate.
//
// THE CALLBACK (found 2026-11-13: Xbox calls meter, xexdis of the two sites)
//   Registered once in D3D's engine start sub_82311A80 (0x82311AEC: r3 =
//   0x82308798 the callback, r4 = the D3D device = its argument) and cleared
//   in its shutdown sub_82311E58 (0x82311F54: r3 = 0). The callback gets
//   r3 = SOURCE, r4 = the argument:
//     source 0 = vertical blank (-> the vblank handler 0x82310628: vblank
//                count for PDDI 0x8258E460 via the game's own callback
//                0x82433380, flip queue sub_82310728)
//     source 1 = an interrupt the GPU COMMAND STREAM asked for (PM4
//                INTERRUPT packet: D3D's fences / callbacks), cmplwi r3,1 at
//                0x823087A8.
//   Source 1 comes from the emulated GPU, which still exists (until step 4
//   replaces the command ring): it is told the callback's address by us, so
//   those keep arriving. Source 0 is ours alone: SDK patch 0023 flag
//   guest_vblank_interrupt = false stops the SDK's vblank thread from calling
//   it too (it still counts vblanks for its own command processor).
//
// HOW IT TICKS
//   Rate = the virtual TV's 60 Hz (console_settings: the game hears a 60 Hz
//   NTSC TV), or guest_refresh_hz when someone set it (SDK patch 0010's flag,
//   which the SDK's thread obeyed: kept for the same experiments). Ticks are
//   placed on an absolute schedule (start + k / rate, sleep_until), so the
//   rate doesn't drift; the SDK's thread polled every 1 ms and could deliver a
//   tick up to ~1 ms late. A tick that comes late is still delivered (a real
//   TV never skips a blank, and D3D counts them); more than 4 behind (the
//   machine stalled) = start the schedule again from now instead of firing a
//   burst. The interrupt runs the way the SDK ran it: on "CPU 2", through the
//   SDK's ExecuteInterrupt (global lock held, TLS pointer 0 like a real
//   interrupt), so it never overlaps a source-1 interrupt.
//
// SWITCH: --vblank_native=false = the SDK's vblank thread again (A/B); the
// registration then goes to the SDK as before.
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(vblank_native, true, "CrashMoM",
                    "The port ticks the virtual TV's vertical blank (the game's 60 Hz frame "
                    "clock) itself; false = the SDK's emulated Xbox does (for comparisons)");

namespace {

using Clock = std::chrono::steady_clock;

// The virtual TV's rate when nobody overrides it (console_settings: 60 Hz NTSC).
constexpr int kTvRateHz = 60;
// More ticks behind than this = the machine stalled: restart the schedule.
constexpr int kMaxCatchUpTicks = 4;
// The "CPU" the interrupt runs on (the SDK's choice: "We're going to guess 2").
constexpr uint8_t kInterruptCpu = 2;
// The callback's first argument for a vertical blank.
constexpr uint64_t kSourceVblank = 0;

struct State {
  // The game's callback and its argument; 0 = none registered (no ticks).
  std::atomic<uint32_t> callback{0};
  std::atomic<uint32_t> argument{0};
  rex::system::object_ref<rex::system::XHostThread> thread;
  // Counters for the once-a-minute log line.
  std::atomic<uint64_t> ticks{0};
  std::atomic<uint64_t> late{0};      // delivered more than half a period late
  std::atomic<uint64_t> restarts{0};  // schedule restarted after a stall
};
State g;

int RateHz() {
  const int32_t wanted = rex::cvar::Query<int32_t>("guest_refresh_hz");
  return wanted > 0 ? std::clamp(int(wanted), 1, 1000) : kTvRateHz;
}

// Tell the SDK's vblank thread to stop calling the game (patch 0023).
void SilenceSdkVblank() {
  if (!rex::cvar::SetFlagByName("guest_vblank_interrupt", "false")) {
    REXLOG_WARN("VBlank: can't stop the SDK's own vblank interrupts (needs SDK patch 0023): "
                "the game would get every vblank twice, so the SDK keeps ticking alone");
  }
}

int ThreadMain() {
  auto* kernel = REX_KERNEL_STATE();
  auto* dispatcher = kernel->function_dispatcher();
  auto* self = rex::system::XThread::GetCurrentThread();
  self->SetActiveCpu(kInterruptCpu);

  int rate = RateHz();
  auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
  auto next = Clock::now() + period;
  auto last_report = Clock::now();
  uint64_t reported_ticks = 0, reported_late = 0, reported_restarts = 0;

  for (;;) {
    std::this_thread::sleep_until(next);
    const auto now = Clock::now();
    if (now - next > period * kMaxCatchUpTicks) {
      // Stalled (debugger, machine swapping): one tick now, schedule from here.
      g.restarts.fetch_add(1, std::memory_order_relaxed);
      next = now;
    } else if (now - next > period / 2) {
      g.late.fetch_add(1, std::memory_order_relaxed);
    }

    const uint32_t callback = g.callback.load(std::memory_order_acquire);
    if (callback != 0) {
      uint64_t args[] = {kSourceVblank, g.argument.load(std::memory_order_relaxed)};
      dispatcher->ExecuteInterrupt(self->thread_state(), callback, args, 2);
      g.ticks.fetch_add(1, std::memory_order_relaxed);
    }

    // The rate can change while running (guest_refresh_hz is hot-reload).
    const int now_rate = RateHz();
    if (now_rate != rate) {
      rate = now_rate;
      period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
      REXLOG_INFO("VBlank: virtual TV now at {} Hz", rate);
    }
    next += period;

    // Once a minute: ticks delivered, late ones, restarts (a steady stream of
    // late ticks = this thread isn't getting the CPU in time).
    if (now - last_report > std::chrono::seconds(60)) {
      const uint64_t t = g.ticks.load(), l = g.late.load(), r = g.restarts.load();
      REXLOG_INFO("VBlank: last minute {} ticks at {} Hz, {} late, {} restarts", t - reported_ticks,
                  rate, l - reported_late, r - reported_restarts);
      reported_ticks = t;
      reported_late = l;
      reported_restarts = r;
      last_report = now;
    }
  }
  return 0;
}

}  // namespace

// -----------------------------------------------------------------------------
// VdSetGraphicsInterruptCallback call sites (manifest "GRAPHICS DRIVER STEP
// 2"): r3 = callback (0 = none), r4 = its argument. Returns nothing.
// -----------------------------------------------------------------------------
bool CrashMomDriverInterruptCallback(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(vblank_native)) return false;
  const uint32_t callback = r3.u32;
  const uint32_t argument = r4.u32;

  // The emulated GPU still raises command-stream interrupts (source 1): it
  // must know where the game's callback is, as long as it exists (step 4).
  if (auto* gpu = REX_KERNEL_STATE()->emulator()->graphics_system()) {
    gpu->SetInterruptCallback(callback, argument);
  }

  g.argument.store(argument, std::memory_order_relaxed);
  g.callback.store(callback, std::memory_order_release);
  if (callback == 0) {
    REXLOG_INFO("VBlank: the game removed its interrupt callback (graphics shut down)");
    return true;
  }

  SilenceSdkVblank();
  if (!g.thread) {
    g.thread = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(REX_KERNEL_STATE(), 128 * 1024, 0, [] { return ThreadMain(); }));
    g.thread->set_name("VBlank");
    g.thread->Create();
  }
  REXLOG_INFO("VBlank: the port ticks the virtual TV at {} Hz (game callback {:08X}, argument "
              "{:08X})",
              RateHz(), callback, argument);
  return true;
}
