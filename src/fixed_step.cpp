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

#include "audio/sound_out.h"
#include "guest_memory.h"
#include "system/helper_threads.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>  // _mm_pause
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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
// game_time::Clock (GameClockNs): the steady clock's count when the fixed
// step last came on; frames count from there.
std::atomic<int64_t> g_game_clock_origin_ns{0};
// The largest value GameClockNs handed out: switching the fixed step on or off
// never makes the clock run backwards.
std::atomic<int64_t> g_game_clock_last_ns{0};
int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

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
// The frame the clock's time counts from (RestartClockAtWholeMs): the clock at
// frame k = base + (k - epoch) / N s. 0 until a restart.
uint64_t g_epoch_frame = 0;
// HELPER THREADS' CLOCK (2026-11-17, helper_threads.h): what the game's clocks
// answer on every thread but the main one while the fixed step is on = the
// current frame's time (no creep), published by the main thread at each pass
// start. 0 = not known yet (before the first clock read of a fixed stretch):
// helpers then read the real clock.
std::atomic<uint64_t> g_helper_us{0};

// Microseconds since the epoch frame, for frame k (integer, like the guest's).
uint64_t FrameUs(uint64_t k, int32_t fps) {
  return k > g_epoch_frame ? (k - g_epoch_frame) * 1000000ull / uint64_t(fps) : 0;
}

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
  g_helper_us = 0;  // helpers: real clock until the new stretch's first read
  g_next_frame = 0;
  g_frame = 0;
  g_epoch_frame = 0;
  g_start = Clock::now();
  // game_time::Clock carries on from where it was (never backwards).
  g_game_clock_origin_ns = std::max(SteadyNs(), g_game_clock_last_ns.load());
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
  return uint32_t((g_clock_base_us + FrameUs(g_frame.load(), fps)) / 1000);
}
int32_t Fps() { return g_fps.load(std::memory_order_relaxed); }
uint64_t Frame() { return g_frame.load(std::memory_order_relaxed); }

// WHY (2026-11-17, first step toward online co-op: two runs must agree frame
// for frame): the clock hands out WHOLE milliseconds, and 1/60 s is 16.667 ms,
// so the ms reading of a frame depends on where that frame sits in a 3-frame
// cycle. The game polls the pads when its ms clock is more than 16 past the
// last poll (input_record.cpp), so a replay starting on frame 1056 polled on
// other frames than one starting on 1057: a joining player's START landed one
// frame later in every run whose start frame was a multiple of 3, and the
// runs parted from there (the "join race" of the 4-player tests). Restarting
// the count here makes every run's clock read the same from this frame on.
// Online lockstep needs the same thing at a session's first frame.
bool RestartClockAtWholeMs() {  // (name kept: it now restarts on a whole HOUR)
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  if (!t_main || fps <= 0 || g_need_base) return false;
  const uint64_t k = g_frame.load(std::memory_order_relaxed);
  const uint64_t now_us = g_clock_base_us + FrameUs(k, fps);
  // The next WHOLE HOUR of clock time: the same VALUE in every run, not
  // just the same rhythm (2026-11-17: a whole-ms restart still carried the
  // load's length, so the spawners' seed, taken from this clock 65 s later,
  // differed by 2.3 s between a normal and a fast run). Later than anything
  // handed out, so the clock never runs backwards; loads take minutes, not
  // hours, so every run lands on the same hour.
  constexpr uint64_t kHourUs = 3600ull * 1000000ull;
  const uint64_t next_hour_us = (std::max(now_us, g_last_virtual_us) / kHourUs + 1) * kHourUs;
  g_clock_base_us = next_hour_us;
  g_epoch_frame = k;
  g_reads_this_pass = 0;
  return true;
}

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
  if (!t_main) {
    // Every other thread (the game's helpers: loaders, audio, workers): the
    // frame's time while the fixed step is on and the option is on
    // (helper_threads.h). Their work then sees the same time on every run,
    // however fast the PC is; a timed wait of theirs ends on game frames.
    if (g_fps.load(std::memory_order_relaxed) <= 0 || !helper_threads::OnGameTime()) return false;
    const uint64_t t = g_helper_us.load(std::memory_order_relaxed);
    if (t == 0) return false;
    // CREEP, like the main thread's: past kReadsBeforeCreep reads of the same
    // frame's time, each further read adds 1 us, counted per thread and per
    // frame. The XMA decode-to-memory loop sub_8234C580 (a loader thread)
    // spins `t0 = clock; while (clock - t0 < 50);` between steps: with a
    // frozen clock it spun until the next frame, and the frame waited for
    // it to go idle (2 s per sound, 2026-11-17). The count is the thread's
    // own, so the times it sees still don't depend on the PC's speed.
    thread_local uint64_t t_reads_of = UINT64_MAX;  // the frame time they're for
    thread_local uint64_t t_reads = 0;
    if (t_reads_of != t) {
      t_reads_of = t;
      t_reads = 0;
    }
    const uint64_t reads = t_reads++;
    us = t + (reads > kReadsBeforeCreep ? reads - kReadsBeforeCreep : 0);
    return true;
  }
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  const uint64_t frame_us = fps > 0 ? FrameUs(g_frame.load(std::memory_order_relaxed), fps) : 0;
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
  g_helper_us.store(g_clock_base_us + frame_us, std::memory_order_relaxed);
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
  // Sound on game frames (sound_out.h OnFixedFrame): handed back to the
  // speakers once when the fixed step goes off.
  static bool sound_clocked = false;
  if (fps <= 0) {
    if (sound_clocked) {
      sound_clocked = false;
      sound_out::OnFixedFrame(0, 0, 0);
    }
    return;  // real time: the game's own delta
  }
  const uint64_t k = g_next_frame++;
  g_frame.store(k, std::memory_order_relaxed);
  if (auto listener = g_pass_listener.load(std::memory_order_relaxed)) listener();
  // The helper threads' clock = this frame's time (after the listener: a
  // replay's first frame / a session start may restart the clock there).
  if (!g_need_base) g_helper_us.store(g_clock_base_us + FrameUs(k, fps), std::memory_order_relaxed);
  // The frame's share of sound is mixed before it runs (after the listener:
  // a replay's first frame restarts the clock's epoch there).
  sound_clocked = true;
  sound_out::OnFixedFrame(k, g_epoch_frame, fps);
  // Loading on game frames (system/helper_threads.h): the helper threads
  // finish what they were asked (after the sound: its blocks wake them too).
  helper_threads::WaitUntilQuiet();
  if (!g_fast.load(std::memory_order_relaxed)) WaitForFrameStart(k, fps);
  // The guest computed f31 in single precision (fmuls): give the same kind
  // of number, 1/N rounded to a float.
  f31.f64 = double(float(1.0 / double(fps)));
}

// =============================================================================
// The spawners' random seed (manifest CrashMomSpawnerSeed, fixed_step.h)
// =============================================================================
namespace fixed_step {
namespace {
// Any fixed number will do; this one spells "MoR!" in ASCII.
std::atomic<uint32_t> g_session_seed{0x4D6F5221u};
}  // namespace
int64_t GameClockNs() {
  const int32_t fps = g_fps.load(std::memory_order_relaxed);
  int64_t ns;
  if (fps > 0) {
    // Integer nanoseconds per frame, like the guest's integer microseconds.
    ns = g_game_clock_origin_ns.load(std::memory_order_relaxed) +
         int64_t(g_frame.load(std::memory_order_relaxed)) * 1000000000ll / fps;
  } else {
    ns = SteadyNs();
  }
  // Never backwards (only matters right after the fixed step is switched).
  int64_t last = g_game_clock_last_ns.load(std::memory_order_relaxed);
  while (ns > last && !g_game_clock_last_ns.compare_exchange_weak(last, ns, std::memory_order_relaxed)) {
  }
  return std::max(ns, last);
}

uint32_t SessionSeed() { return g_session_seed.load(std::memory_order_relaxed); }
void SetSessionSeed(uint32_t seed) { g_session_seed.store(seed, std::memory_order_relaxed); }
}  // namespace fixed_step

// Codegen declares `extern void CrashMomSpawnerSeed(PPCRegister& r4);`.
// r4 = the game's millisecond clock reading it is about to seed the spawners'
// generator with (0x82148334). Fixed step on: the session's seed instead.
void CrashMomSpawnerSeed(PPCRegister& r4) {
  if (!fixed_step::Active()) return;
  const uint32_t seed = fixed_step::SessionSeed();
  REXLOG_INFO("Fixed step: spawners' random generator seeded with {:08X} (the game asked its clock: {:08X})",
              seed, r4.u32);
  r4.u64 = seed;
}

// =============================================================================
// A session starts (fixed_step.h StartSession)
// =============================================================================
namespace fixed_step {
namespace {
// A generator word made from the session seed: never 0 (a multiply-with-carry
// half stuck at 0 stays 0), different for every (generator, word).
uint32_t SeedWord(uint32_t seed, uint32_t index) {
  uint64_t z = (uint64_t(seed) << 32 | index) + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  const uint32_t w = uint32_t(z ^ (z >> 31));
  return w ? w : 0x6D6F5221u;
}
void Store32(uint8_t* base, uint32_t address, uint32_t v) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
struct Generator {
  uint32_t address;
  uint32_t words;
  const char* what;
};
constexpr Generator kGenerators[] = {
    {0x825A14F4, 2, "main"},
    {0x825A4FC4, 2, "spawners"},
    {0x8259F928, 2, "enemy AI"},
    {0x8257FA04, 6, "particles"},
};
constexpr uint32_t kSpawnerSeededFlag = 0x8259AD9E;
}  // namespace

void StartSession() {
  if (!Active() || !t_main) return;
  RestartClockAtWholeMs();
  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t seed = SessionSeed();
  uint32_t index = 0;
  for (const Generator& g : kGenerators) {
    for (uint32_t w = 0; w < g.words; ++w) Store32(base, g.address + 4 * w, SeedWord(seed, index++));
  }
  *GuestPtr(base, kSpawnerSeededFlag) = 1;
  REXLOG_INFO("Fixed step: SESSION START at frame {}: clock restarted, {} random generators set "
              "from seed {:08X}",
              Frame(), sizeof(kGenerators) / sizeof(kGenerators[0]), seed);
}
}  // namespace fixed_step
