// =============================================================================
// system/helper_threads.cpp -- loading on game frames (helper_threads.h)
// =============================================================================
#include "system/helper_threads.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "fixed_step.h"

REXCVAR_DEFINE_BOOL(loading_follows_game_frames, true, "CrashMoM",
                    "With the fixed step on (exact runs, online / LAN sessions): every game frame "
                    "waits until the game's helper threads (loading, audio) have finished what they "
                    "were asked, so data lands on the same frame on every run / PC");

namespace helper_threads {
namespace {

using Clock = std::chrono::steady_clock;

// Statuses that are not a wake (the wait ran out): STATUS_TIMEOUT.
constexpr uint32_t kStatusTimeout = 0x00000102;

// How long nothing may wake before the threads count as quiet, how long a
// frame waits at most, and when a thread that never waits stops counting.
constexpr auto kSettle = std::chrono::microseconds(250);
constexpr auto kMaxWait = std::chrono::seconds(2);
constexpr auto kGiveUpBusy = std::chrono::seconds(2);

constexpr int kSlots = 128;
struct Slot {
  std::atomic<bool> used{false};
  std::atomic<int> in_wait{0};             // nesting: >0 = inside a wait
  std::atomic<int64_t> changed_ns{0};      // when in_wait last changed (steady)
  std::atomic<bool> gave_up_logged{false};
  char name[32] = {};                      // the host thread's name (logs)
  std::atomic<int> tid{0};                 // its Linux thread id
};
std::array<Slot, kSlots> g_slots;
std::atomic<int> g_next_slot{0};
std::atomic<uint64_t> g_wakes{0};
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

// Is one of the GAME's threads that hasn't waited yet (so has no slot)
// runnable right now? A thread the game just created for a load does its
// first work before its first wait, invisible to the wait tracking: on one
// CPU core it hadn't even started when the frame checked, and an exact run
// parted at the load (2026-11-17). Linux's own run state of every thread
// (/proc/self/task/<tid>/stat: R = running or ready to run) covers them.
// Game threads = the guest thread names: "XThread...", "audio::...",
// "ClipLoadThread"; the port's own and the SDK's host threads aren't.
bool UntrackedGameThreadRunnable() {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return false;
  bool runnable = false;
  const int self = int(gettid());
  const int n = std::min(g_next_slot.load(), kSlots);
  while (dirent* e = readdir(dir)) {
    const int tid = std::atoi(e->d_name);
    if (tid <= 0 || tid == self) continue;
    bool tracked = false;
    for (int i = 0; i < n && !tracked; ++i) tracked = g_slots[i].tid.load() == tid;
    if (tracked) continue;
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
    if (!game) continue;
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

// Every tracked thread inside a wait (or given up on)?
bool AllWaiting(int64_t now) {
  const int n = std::min(g_next_slot.load(), kSlots);
  for (int i = 0; i < n; ++i) {
    Slot& s = g_slots[i];
    if (!s.used.load() || s.in_wait.load() > 0) continue;
    if (std::chrono::nanoseconds(now - s.changed_ns.load()) > kGiveUpBusy) {
      if (!s.gave_up_logged.exchange(true)) {
        REXLOG_INFO("Loading on game frames: helper thread {} ({}) has been busy for over 2 s "
                    "without waiting: not waited for any more (ended, or never sleeps)",
                    i, s.name);
      }
      continue;
    }
    return false;
  }
  return !UntrackedGameThreadRunnable();
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
    s->in_wait.fetch_sub(1);
    s->changed_ns = NowNs();
    s->gave_up_logged = false;
    // A wait that ended because something happened (an object signalled, a
    // lock handed over) = the thread has work: not quiet yet.
    if (!timed_sleep && status != kStatusTimeout) g_wakes.fetch_add(1);
  }
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
      REXLOG_WARN("Loading on game frames: the helper threads didn't go quiet within 2 s; "
                  "the frame goes on");
      return;
    }
  }
}

}  // namespace helper_threads
