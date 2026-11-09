// =============================================================================
// fixed_step.cpp -- every game frame exactly 1/N s (fixed_step.h)
// =============================================================================
//
// The hook (manifest CrashMomPassDelta, main loop sub_8227AEE0 at 0x8227B04C):
//   0x8227B020  bl     0x8235aae0       ; clock, us
//   0x8227B030  subf   r29,r11,r30      ; - last pass's clock (+164)
//   0x8227B048  fmuls  f31,f12,f30      ; x 1e-6 -> f31 = this pass's delta
//   0x8227B04C  fmr    f1,f31           ; <- here
// f31 is what the rest of the pass is told: sub_8227aa18, the subsystems
// (sub_8227CE08), and on gameplay's path the frame function (slot 40), which
// hands it to the time manager and adds it to its accumulated step (+160):
// with every pass running its frame (frame_rate.cpp CrashMomFrameStep), the
// frame's dt = this f31 exactly.
//
// The loading-screen / movie path (the main-loop limiter, frame_rate.cpp
// section 2) gets the same delta; the limiter is told "late enough" every
// pass, so there too one pass = one frame of 1/N s.
//
// THE GAME'S CLOCKS. The frame's dt isn't the only time the game reads: three
// small functions turn QueryPerformanceCounter (sub_824731D8) into
//   sub_8235AAE0  microseconds, u32   (the main loop's clock, a few others)
//   sub_8235AB58  microseconds, u64
//   sub_8235ABC8  MILLISECONDS, u32   (dozens of gameplay callers, among them
//                 the controller code around the spin detector sub_82273030)
// With only the dt fixed, two 180 fps runs of the same replay still parted:
// Crash's body pops into place one frame after the first button press, and
// that happened one frame earlier in one run than the other (2026-10-09,
// 5.6 ms frames show it; 33 ms frames at 30 fps hid it). So while the fixed
// step is on, these three answer FIXED-STEP TIME on the game's main thread:
// the moment its pass started, frame k = base + k/N s (past 1,000 reads in
// one pass, each further read adds 1 us: a wait-with-timeout in a pass still
// ends). Other threads (sound, streaming) keep the real clock. With
// --fixed_step (from launch) the base is a FIXED value, not the PC's clock:
// the game polls the pads on a 20 ms timer from this clock, and a base that
// differed per run put the polls on different frames (2026-10-09).
// When the fixed step goes off, the main thread's clock carries on from
// where the fixed step left it (an offset), so it never runs backwards.
// =============================================================================

#include "fixed_step.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>  // _mm_pause
#endif

#include <atomic>
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(fixed_step, 0, "CrashMoM",
                     "EXACT REPEATABLE RUNS (testing): every game frame lasts exactly 1/N s, "
                     "whatever the PC does; 0 = off (real time). Debug console: clock fixed N")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(fixed_step_fast, false, "CrashMoM",
                    "With --fixed_step: don't wait between frames (runs as fast as the PC can)");

namespace fixed_step {
namespace {

using Clock = std::chrono::steady_clock;

// A request from the console (or the flags, at the first pass), applied by
// the main thread at the start of its next pass. `generation` changes with
// every request, so the same numbers asked twice still restart the count.
std::atomic<int32_t> g_req_fps{0};
std::atomic<bool> g_req_fast{false};
std::atomic<uint32_t> g_req_generation{0};

// The state in force (written by the main thread only; read anywhere).
std::atomic<int32_t> g_fps{0};
std::atomic<bool> g_fast{false};
std::atomic<uint64_t> g_frame{0};

std::atomic<void (*)()> g_pass_listener{nullptr};

// Main thread only.
uint32_t g_applied_generation = 0;
bool g_flags_read = false;
uint64_t g_next_frame = 0;
Clock::time_point g_start;  // wall-clock time of frame 0 (paced mode)

constexpr auto kSpin = std::chrono::milliseconds(1);  // spin, don't sleep, this close

// The main thread's view of the game's clocks (microseconds). See the file
// header: fixed-step time while on; real time + g_clock_offset_us otherwise.
thread_local bool t_main = false;  // set by the main loop's hook
// All in the GAME's microseconds (sub_8235AB58's numbers, its QPC origin):
// the base is taken at the first clock read of a fixed stretch, from the
// game's own clock, so its time just carries on.
uint64_t g_clock_base_us = 0;      // fixed-step time of frame 0
bool g_need_base = false;          // a fixed stretch began: take the base at the next read
bool g_need_offset = false;        // one ended: take the offset at the next read
int64_t g_clock_offset_us = 0;     // real -> main-thread time after a fixed stretch
uint64_t g_reads_this_pass = 0;
// The fixed step came from --fixed_step (on from the first pass): its clock
// starts at kLaunchClockUs instead of the PC's clock reading of the moment,
// which differs every run (a 20 ms pad-poll timer then fell on different
// frames in two runs of the same replay, 2026-10-09).
bool g_from_launch = false;
constexpr uint64_t kLaunchClockUs = 1000000000ull;  // 1000 s: any fixed value
// Clock reads in one pass that return the SAME time; past this many, each
// further read adds 1 us, so a wait-with-timeout inside one frame still ends.
// (Every read adding 1 us made the time depend on how often loading code
// polls, which varies run to run.)
constexpr uint64_t kReadsBeforeCreep = 1000;
uint64_t g_last_virtual_us = 0;    // the last time handed out while fixed

void ApplyRequests() {
  if (!g_flags_read) {
    g_flags_read = true;
    if (REXCVAR_GET(fixed_step) > 0 && g_req_generation.load() == 0) {
      g_from_launch = true;
      g_req_fps = REXCVAR_GET(fixed_step);
      g_req_fast = REXCVAR_GET(fixed_step_fast);
      g_req_generation.fetch_add(1);
    }
  }
  const uint32_t generation = g_req_generation.load(std::memory_order_acquire);
  if (generation == g_applied_generation) return;
  g_applied_generation = generation;
  const int32_t fps = g_req_fps.load();
  const bool fast = g_req_fast.load();
  // The clocks (file header) carry on from what the main thread saw last:
  // settled at the next clock read, which has the game's own clock at hand.
  if (g_fps.load() > 0 && fps <= 0) g_need_offset = true;
  if (fps > 0) g_need_base = true;
  g_reads_this_pass = 0;
  g_fps = fps;
  g_fast = fast;
  g_next_frame = 0;
  g_frame = 0;
  g_start = Clock::now();
  if (fps > 0) {
    REXLOG_INFO("Fixed step: ON, every frame exactly 1/{} s ({:.3f} ms){}", fps, 1000.0 / fps,
                fast ? ", not waiting between frames (fast)" : "");
  } else {
    REXLOG_INFO("Fixed step: off (real time)");
  }
}

// Paced mode: frame k starts at g_start + k/N on the wall clock. A frame that
// ran over a whole period moves the schedule (no burst of catch-up frames;
// the game time stays exact either way, only the wall clock slips).
void WaitForFrameStart(uint64_t k, int32_t fps) {
  const auto period = std::chrono::duration<double>(1.0 / fps);
  auto start = g_start + std::chrono::duration_cast<Clock::duration>(period * double(k));
  const auto now = Clock::now();
  if (start + std::chrono::duration_cast<Clock::duration>(period) < now) {
    g_start = now - std::chrono::duration_cast<Clock::duration>(period * double(k));
    return;
  }
  if (start - now > kSpin) std::this_thread::sleep_until(start - kSpin);
  while (Clock::now() < start) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  }
}

}  // namespace

bool Active() { return g_fps.load(std::memory_order_relaxed) > 0; }
bool OnMainThread() { return t_main; }
void SetPassListener(void (*listener)()) { g_pass_listener = listener; }
uint32_t ClockMs() {
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  if (fps <= 0 || g_need_base) return 0;
  return uint32_t((g_clock_base_us + g_frame.load() * 1000000ull / uint64_t(fps)) / 1000);
}
int32_t Fps() { return g_fps.load(std::memory_order_relaxed); }
uint64_t Frame() { return g_frame.load(std::memory_order_relaxed); }

std::string Set(int32_t fps, bool fast) {
  if (fps < 0 || fps > 1000) return "fps must be 0..1000\n";
  g_req_fps = fps;
  g_req_fast = fast;
  g_req_generation.fetch_add(1, std::memory_order_release);
  return fps ? fmt::format("fixed step 1/{} s{} from the next frame (frame count restarts at 0)\n",
                           fps, fast ? ", fast" : "")
             : std::string("real time from the next frame\n");
}

std::string Status() {
  const int32_t fps = Fps();
  if (!fps) return "real time (fixed step off)\n";
  const uint64_t frame = Frame();
  return fmt::format("fixed step 1/{} s{}: frame {} = game time {:.3f} s\n", fps,
                     g_fast.load() ? " (fast)" : "", frame, double(frame) / fps);
}

}  // namespace fixed_step

namespace fixed_step {
namespace {
// What one of the game's clocks answers on this thread, in microseconds;
// false = not ours to answer (other threads, or never fixed). `real` = the
// game's own 64-bit microsecond clock (sub_8235AB58's original).
template <typename RealClock>
bool MainThreadClockUs(uint64_t& us, RealClock real) {
  if (!t_main) return false;
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  const uint64_t frame_us = fps > 0 ? g_frame.load(std::memory_order_relaxed) * 1000000ull / uint64_t(fps) : 0;
  if (g_need_base && fps > 0) {
    g_need_base = false;
    g_clock_base_us = g_from_launch ? kLaunchClockUs - frame_us
                                    : uint64_t(int64_t(real()) + g_clock_offset_us) - frame_us;
    g_from_launch = false;  // a later `clock fixed` continues from the real clock
  }
  if (g_need_offset && fps <= 0) {
    g_need_offset = false;
    g_clock_offset_us = int64_t(g_last_virtual_us + 1) - int64_t(real());
  }
  if (fps <= 0) {
    if (g_clock_offset_us == 0) return false;  // never fixed: untouched
    us = uint64_t(int64_t(real()) + g_clock_offset_us);
    return true;
  }
  const uint64_t reads = g_reads_this_pass++;
  us = g_clock_base_us + frame_us + (reads > kReadsBeforeCreep ? reads - kReadsBeforeCreep : 0);
  g_last_virtual_us = us;
  return true;
}
}  // namespace
}  // namespace fixed_step

// The game's three clocks (file header). Originals stay callable.
extern "C" REX_FUNC(__imp__sub_8235AAE0);
extern "C" REX_FUNC(__imp__sub_8235AB58);
extern "C" REX_FUNC(__imp__sub_8235ABC8);

// The game's real 64-bit microsecond clock (calls the original; it only
// touches volatile registers and its own stack frame, like any call here).
static uint64_t RealGameUs(PPCContext& ctx, uint8_t* base) {
  __imp__sub_8235AB58(ctx, base);
  return ctx.r3.u64;
}

extern "C" REX_FUNC(sub_8235AAE0) {  // microseconds, 32 bits
  uint64_t us;
  if (!fixed_step::MainThreadClockUs(us, [&] { return RealGameUs(ctx, base); })) {
    return __imp__sub_8235AAE0(ctx, base);
  }
  ctx.r3.u64 = uint32_t(us);
}
extern "C" REX_FUNC(sub_8235AB58) {  // microseconds, 64 bits
  uint64_t us;
  if (!fixed_step::MainThreadClockUs(us, [&] { return RealGameUs(ctx, base); })) {
    return __imp__sub_8235AB58(ctx, base);
  }
  ctx.r3.u64 = us;
}
extern "C" REX_FUNC(sub_8235ABC8) {  // milliseconds, 32 bits
  uint64_t us;
  if (!fixed_step::MainThreadClockUs(us, [&] { return RealGameUs(ctx, base); })) {
    return __imp__sub_8235ABC8(ctx, base);
  }
  ctx.r3.u64 = uint32_t(us / 1000);
}

// Codegen declares `extern void CrashMomPassDelta(PPCRegister& f31);`.
// Game's main thread, once per main-loop pass.
void CrashMomPassDelta(PPCRegister& f31) {
  using namespace fixed_step;
  t_main = true;
  g_reads_this_pass = 0;
  ApplyRequests();
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  if (fps <= 0) return;  // real time: the game's own delta
  const uint64_t k = g_next_frame++;
  g_frame.store(k, std::memory_order_relaxed);
  if (auto listener = g_pass_listener.load(std::memory_order_relaxed)) listener();
  if (!g_fast.load(std::memory_order_relaxed)) WaitForFrameStart(k, fps);
  // The guest computed f31 in single precision (fmuls): give the same kind
  // of number, 1/N rounded to a float.
  f31.f64 = double(float(1.0 / double(fps)));
}
