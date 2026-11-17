// =============================================================================
// system/helper_threads.cpp -- loading on game frames (helper_threads.h)
// =============================================================================
#include "system/helper_threads.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string_view>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"

#include "fixed_step.h"

REXCVAR_DEFINE_BOOL(loading_follows_game_frames, true, "CrashMoM",
                    "With the fixed step on (exact runs, online / LAN sessions): every game frame "
                    "waits until the game's helper threads (loading, audio) have finished what they "
                    "were asked, so data lands on the same frame on every run / PC");
REXCVAR_DEFINE_BOOL(helper_threads_between_frames, true, "CrashMoM",
                    "With the fixed step on: the game's helper threads work only between frames "
                    "(and while the main thread waits for them), so a frame never sees a helper's "
                    "result early on one PC and late on another (system/helper_threads.h)");
REXCVAR_DEFINE_BOOL(helper_threads_on_game_time, true, "CrashMoM",
                    "With the fixed step on: the game's helper threads read the frame's time and "
                    "their timed waits / sleeps end on game frames, not on the PC's clock "
                    "(system/helper_threads.h)");

namespace helper_threads {
namespace {

using Clock = std::chrono::steady_clock;

// Statuses that are not a wake (the wait ran out): STATUS_TIMEOUT.
constexpr uint32_t kStatusTimeout = 0x00000102;

// How long nothing may wake before the threads count as quiet, how long a
// frame waits at most, and when a thread that never waits stops counting.
constexpr auto kSettle = std::chrono::microseconds(250);
// (A level's loader can work for several seconds without a wait: the frame
// waits for it, the loading screen simply holds still meanwhile.)
constexpr auto kMaxWait = std::chrono::seconds(30);
// A busy thread is checked for having ended once it's been busy this long.
constexpr auto kCheckEnded = std::chrono::milliseconds(100);

constexpr int kSlots = 128;
struct Slot {
  std::atomic<bool> used{false};
  std::atomic<int> in_wait{0};             // nesting: >0 = inside a wait
  std::atomic<int64_t> changed_ns{0};      // when in_wait last changed (steady)
  std::atomic<bool> gave_up_logged{false};
  char name[32] = {};                      // the host thread's name (logs)
  std::atomic<int> tid{0};                 // its Linux thread id
  // Game-time deadline (fixed_step::GameClockNs) of the timed wait it is in,
  // or kNoDeadline. Reached while still inside = about to wake = busy.
  std::atomic<int64_t> deadline_ns{INT64_MAX};
  // D3D's interrupt thread (its waits are called from sub_82320E08): it
  // answers the GPU, and the main thread waits on it inside a frame (GPU
  // fences while a load streams): holding it at the gate only stalled the
  // frame 100 ms at a time. Render plumbing, no world state: never held.
  std::atomic<bool> render{false};
};
constexpr int64_t kNoDeadline = INT64_MAX;
std::array<Slot, kSlots> g_slots;
std::atomic<int> g_next_slot{0};
std::atomic<uint64_t> g_wakes{0};

// THE GATE (helper_threads.h "between frames"): open = helpers may work.
// Closed while the main thread runs a frame's game code; a helper whose wait
// ends meanwhile is held here (counted in g_held, still "inside its wait")
// until the gate opens. kMaxHold = the deadlock guard's check interval (see
// AfterWait: let go only while the main thread sleeps).
std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
std::atomic<bool> g_gate_open{true};
std::atomic<int> g_held{0};
constexpr auto kMaxHold = std::chrono::milliseconds(100);

std::atomic<int> g_main_tid{0};  // set when the main thread closes the gate

// Is the main thread asleep (state S/D in /proc), not running or ready?
bool MainThreadAsleep() {
  const int tid = g_main_tid.load();
  if (tid <= 0) return false;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  char stat[256] = {};
  if (FILE* f = std::fopen(path, "r")) {
    const size_t got = std::fread(stat, 1, sizeof(stat) - 1, f);
    stat[got] = 0;
    std::fclose(f);
  }
  const char* close = std::strrchr(stat, ')');
  return close && close[1] == ' ' && (close[2] == 'S' || close[2] == 'D');
}

bool Gating() {
  return fixed_step::Active() && REXCVAR_GET(loading_follows_game_frames) &&
         REXCVAR_GET(helper_threads_between_frames);
}
thread_local int t_slot = -1;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// This thread's slot (assigned at its first wait), or -1: the main thread
// and threads past kSlots aren't tracked.
Slot* MySlot() {
  if (fixed_step::OnMainThread()) {
    // Its waits before the main loop began gave it a slot: not a helper.
    if (t_slot >= 0 && t_slot < kSlots) g_slots[t_slot].used = false;
    return nullptr;
  }
  if (t_slot < 0) {
    const int s = g_next_slot.fetch_add(1);
    if (s >= kSlots) return nullptr;
    t_slot = s;
    pthread_getname_np(pthread_self(), g_slots[s].name, sizeof(g_slots[s].name));
    g_slots[s].tid = int(gettid());
    // The port's OWN threads that run game code (their pace is set by us:
    // Sound Out mixes on game frames already, VBlank ticks on the clock) and
    // the main thread itself are not helpers to wait for. They wait mostly
    // on our own locks, which these wrappers never see, so they would look
    // busy all the time (Sound Out held every frame 2 s: the first try).
    const std::string_view name(g_slots[s].name);
    for (std::string_view own : {"Sound Out", "VBlank", "Command Ring", "Main XThread"}) {
      if (name.substr(0, own.size()) == own) return nullptr;  // slot stays unused
    }
    g_slots[s].used = true;
  }
  return t_slot < kSlots ? &g_slots[t_slot] : nullptr;
}

// Is one of the GAME's threads runnable right now: a helper (tracked) or one
// that hasn't waited yet (so has no slot)? A thread the game just created for a load does its
// first work before its first wait, invisible to the wait tracking: on one
// CPU core it hadn't even started when the frame checked, and an exact run
// parted at the load (2026-11-17). Linux's own run state of every thread
// (/proc/self/task/<tid>/stat: R = running or ready to run) covers them.
// Game threads = the guest thread names: "XThread...", "audio::...",
// "ClipLoadThread"; the port's own and the SDK's host threads aren't.
bool GameThreadRunnable() {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return false;
  bool runnable = false;
  const int self = int(gettid());
  const int n = std::min(g_next_slot.load(), kSlots);
  while (dirent* e = readdir(dir)) {
    const int tid = std::atoi(e->d_name);
    if (tid <= 0 || tid == self) continue;
    // TRACKED helpers count too (2026-11-17): one that was just woken (an
    // event set, the mixer's blocks done) is still "inside its wait" until it
    // gets a CPU and returns, which on a busy PC took longer than the settle
    // time: a frame then started before it had worked, and a paced run parted
    // from a fast one now and then (the speech list at frame 3926 of the
    // Wumpa replay, 1 run in 3). Linux marks a woken thread runnable at once.
    bool tracked = false, helper = false;
    for (int i = 0; i < n && !tracked; ++i) {
      if (g_slots[i].tid.load() == tid) {
        tracked = true;
        helper = g_slots[i].used.load();
      }
    }
    if (tracked && !helper) continue;  // the port's own threads, main
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    char comm[32] = {};
    if (FILE* f = std::fopen(path, "r")) {
      if (!std::fgets(comm, sizeof(comm), f)) comm[0] = 0;
      std::fclose(f);
    }
    const std::string_view name(comm);
    const bool game = name.substr(0, 7) == "XThread" || name.substr(0, 7) == "audio::" ||
                      name.substr(0, 8) == "ClipLoad";
    if (!game && !helper) continue;
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    char stat[256] = {};
    if (FILE* f = std::fopen(path, "r")) {
      const size_t got = std::fread(stat, 1, sizeof(stat) - 1, f);
      stat[got] = 0;
      std::fclose(f);
    }
    // The state letter follows the LAST ')' (names can hold spaces / ')').
    const char* close = std::strrchr(stat, ')');
    if (close && close[1] == ' ' && close[2] == 'R') {
      runnable = true;
      break;
    }
  }
  closedir(dir);
  return runnable;
}

// Has this tracked thread ended (no /proc/self/task/<tid> any more)?
bool Ended(const Slot& s) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d", s.tid.load());
  return access(path, F_OK) != 0;
}

// Every tracked thread inside a wait (or ended)?
bool AllWaiting(int64_t now) {
  const int n = std::min(g_next_slot.load(), kSlots);
  const int64_t game_now = fixed_step::GameClockNs();
  for (int i = 0; i < n; ++i) {
    Slot& s = g_slots[i];
    if (!s.used.load()) continue;
    if (s.in_wait.load() > 0) {
      // Inside a timed wait whose game-time deadline has come: it wakes
      // within one real slice (1 ms) and then works = not quiet.
      if (s.deadline_ns.load() <= game_now) return false;
      continue;
    }
    // Busy. Only a thread that has ENDED stops counting (its /proc entry is
    // gone). Until 2026-11-17 a thread busy for 2 s without a wait was given
    // up on as well, "ended, or never sleeps": the level loader decompresses
    // for longer than that without waiting, and the frames then ran on while
    // it worked in real time = a load 271 frames long on one PC and 169 on
    // another. A thread that spins on the clock instead of sleeping no longer
    // needs it: its clock creeps (fixed_step.cpp), so its loop ends.
    if (std::chrono::nanoseconds(now - s.changed_ns.load()) > kCheckEnded && Ended(s)) {
      if (!s.gave_up_logged.exchange(true)) {
        REXLOG_INFO("Loading on game frames: helper thread {} ({}) has ended: not waited for any more",
                    i, s.name);
      }
      continue;
    }
    return false;
  }
  if (g_held.load() > 0) return false;  // released, about to work
  return !GameThreadRunnable();
}

}  // namespace

void BeforeWait() {
  if (Slot* s = MySlot()) {
    s->in_wait.fetch_add(1);
    s->changed_ns = NowNs();
  }
}

void AfterWait(uint32_t status, bool timed_sleep) {
  if (Slot* s = MySlot()) {
    // Between frames only: woken while the main thread runs game code =
    // wait here, still counted as inside the wait, until the gate opens.
    if (!g_gate_open.load() && !s->render.load() && Gating()) {
      g_held.fetch_add(1);
      bool timed_out = false;
      {
        std::unique_lock<std::mutex> lock(g_gate_mutex);
        // Held as long as the main thread RUNS (a frame may take long: the
        // boot's data patches ran 100+ ms on the main thread, and letting the
        // helper go after a fixed 100 ms released it mid-frame). Let go only
        // when the main thread has been ASLEEP across two checks: blocked on
        // something the helper may hold (a lock the wait wrappers don't see).
        int asleep_checks = 0;
        while (!g_gate_cv.wait_for(lock, kMaxHold, [] { return g_gate_open.load(); })) {
          asleep_checks = MainThreadAsleep() ? asleep_checks + 1 : 0;
          if (asleep_checks >= 2) {
            timed_out = true;
            break;
          }
        }
      }
      if (timed_out) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20) {
          REXLOG_WARN("Loading on game frames: helper thread {} let go while the frame runs: the "
                      "main thread is blocked (it may need it)",
                      s->name);
        }
      }
      s->in_wait.fetch_sub(1);
      s->changed_ns = NowNs();
      if (!timed_sleep && status != kStatusTimeout) g_wakes.fetch_add(1);
      g_held.fetch_sub(1);
      s->gave_up_logged = false;
      return;
    }
    s->in_wait.fetch_sub(1);
    s->changed_ns = NowNs();
    s->gave_up_logged = false;
    // A wait that ended because something happened (an object signalled, a
    // lock handed over) = the thread has work: not quiet yet.
    if (!timed_sleep && status != kStatusTimeout) g_wakes.fetch_add(1);
  }
}

void OpenGate() {
  {
    std::lock_guard<std::mutex> lock(g_gate_mutex);
    g_gate_open = true;
  }
  g_gate_cv.notify_all();
}

void CloseGate() {
  if (!Gating()) return;
  g_main_tid = int(gettid());
  std::lock_guard<std::mutex> lock(g_gate_mutex);
  g_gate_open = false;
}

void LetHelpersFinish() {
  if (g_gate_open.load() || !Gating()) return;
  OpenGate();
  WaitUntilQuiet();
  CloseGate();
}

bool OnGameTime() { return fixed_step::Active() && REXCVAR_GET(helper_threads_on_game_time); }

namespace {
constexpr uint32_t kStatusSuccess = 0;
// One real slice of a game-time wait: 1 ms relative (100 ns units).
constexpr int64_t kSliceTicks = -10000;

uint64_t& Register(PPCContext& ctx, int r) {
  switch (r) {
    case 3: return ctx.r3.u64;
    case 4: return ctx.r4.u64;
    case 5: return ctx.r5.u64;
    case 6: return ctx.r6.u64;
    case 7: return ctx.r7.u64;
    case 8: return ctx.r8.u64;
    case 9: return ctx.r9.u64;
    default: return ctx.r10.u64;  // (PPCContext registers aren't in number order: no &r0 indexing)
  }
}
int64_t LoadI64(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
  return int64_t(v);
}
void StoreI64(uint8_t* base, uint32_t a, int64_t value) {
  uint8_t* p = GuestPtr(base, a);
  const uint64_t v = uint64_t(value);
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}
}  // namespace

void Wait(PPCContext& ctx, uint8_t* base, RealImport real, int timeout_reg, bool timed_sleep) {
  // The MAIN thread waits inside a frame (often for a helper's result): the
  // gate opens for it, and before it goes on the helpers finish everything
  // they have (the same work done on every PC), then it closes again.
  if (fixed_step::OnMainThread() && !g_gate_open.load() && Gating()) {
    OpenGate();
    real(ctx, base);
    WaitUntilQuiet();
    CloseGate();
    return;
  }
  Slot* s = MySlot();
  if (s && uint32_t(ctx.lr) >= 0x82320E08 && uint32_t(ctx.lr) < 0x82320F80) s->render = true;
  const uint32_t timeout_ptr = uint32_t(Register(ctx, timeout_reg));
  const int64_t timeout = timeout_ptr ? LoadI64(base, timeout_ptr) : 0;
  // Untouched: not a helper, option off / fixed step off, no timeout
  // (infinite), a poll (0) or an absolute time (positive).
  if (!s || !OnGameTime() || !timeout_ptr || timeout >= 0) {
    BeforeWait();
    real(ctx, base);
    AfterWait(ctx.r3.u32, timed_sleep);
    return;
  }
  const int64_t deadline = fixed_step::GameClockNs() + (-timeout) * 100;
  s->deadline_ns = deadline;
  BeforeWait();
  // The slices' timeout lives on this thread's guest stack, well below its
  // stack pointer (free while the thread is inside this host call): the
  // game's own timeout may be a constant shared by other threads.
  const uint32_t slice_ptr = (uint32_t(ctx.r1.u64) - 0x400) & ~7u;
  StoreI64(base, slice_ptr, kSliceTicks);
  PPCContext c = ctx;
  // A slice that ends the normal way: a wait = STATUS_TIMEOUT, a sleep =
  // STATUS_SUCCESS. Anything else (signalled, an APC, alerted) = done.
  const uint32_t slice_done = timed_sleep ? kStatusSuccess : kStatusTimeout;
  while (true) {
    c = ctx;
    Register(c, timeout_reg) = slice_ptr;
    real(c, base);
    if (c.r3.u32 != slice_done) break;
    if (fixed_step::GameClockNs() >= deadline || !OnGameTime()) break;
  }
  s->deadline_ns = kNoDeadline;
  // The caller sees its own arguments back, the import's result.
  const uint64_t keep = Register(ctx, timeout_reg);
  ctx = c;
  Register(ctx, timeout_reg) = keep;
  AfterWait(ctx.r3.u32, timed_sleep);
}

void WaitUntilQuiet() {
  if (!REXCVAR_GET(loading_follows_game_frames)) return;
  const auto start = Clock::now();
  while (true) {
    const uint64_t wakes = g_wakes.load();
    if (AllWaiting(NowNs())) {
      std::this_thread::sleep_for(kSettle);
      if (g_wakes.load() == wakes && AllWaiting(NowNs())) return;
    } else {
      std::this_thread::yield();
    }
    if (Clock::now() - start > kMaxWait) {
      REXLOG_WARN("Loading on game frames: the helper threads didn't go quiet within 30 s; "
                  "the frame goes on");
      return;
    }
  }
}

}  // namespace helper_threads
