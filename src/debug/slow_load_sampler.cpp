// =============================================================================
// debug/slow_load_sampler.cpp -- see slow_load_sampler.h
// =============================================================================
#include "slow_load_sampler.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(debug_slow_load_sampler, true, "CrashMoM",
                    "When the title's loading crawls (front end states 51-53, frames over 60 ms), sample "
                    "the game's main thread for a few seconds and log where it was (Linux)");

#if defined(__linux__)

#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "../data/fight_tree.h"
#include "../pddi/intercept.h"
#include "host_symbols.h"

namespace slow_load_sampler {
namespace {

constexpr double kSlowMs = 60.0;           // a frame this slow (the game's own time) counts
constexpr int kSlowFramesInARow = 3;
constexpr auto kSampleFor = std::chrono::seconds(4);
constexpr auto kEvery = std::chrono::milliseconds(2);
constexpr auto kCooldown = std::chrono::seconds(60);
constexpr int kDepth = 48;                 // host frames kept per sample
constexpr int kMaxSamples = 2400;
const int kSignal = SIGRTMIN + 12;         // (the runtime itself uses SIGRTMIN + 1)

struct Sample {
  int depth = 0;
  void* frames[kDepth];
};
std::array<Sample, kMaxSamples> g_samples;
std::atomic<int> g_count{0};
std::atomic<bool> g_running{false};
std::atomic<pid_t> g_main_tid{0};
int g_slow_in_a_row = 0;
std::chrono::steady_clock::time_point g_last_run{};

// In the game's main thread, at the timer thread's signal: its stack.
void OnSignal(int, siginfo_t*, void*) {
  const int i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxSamples) return;
  g_samples[size_t(i)].depth = backtrace(g_samples[size_t(i)].frames, kDepth);
}

bool LoadingState(int32_t state) { return state >= 51 && state <= 53; }

void Report(int32_t state, double worst_ms) {
  const int n = std::min(g_count.load(), kMaxSamples);
  std::map<uint32_t, int> guest;    // innermost game function per sample
  std::map<std::string, int> host;  // innermost host function (past the signal frames)
  std::map<std::string, int> caller;  // innermost host function outside libc (who made that call)
  for (int s = 0; s < n; ++s) {
    const Sample& sm = g_samples[size_t(s)];
    // frames[0] = OnSignal, [1] = the signal trampoline, [2] = where the thread was.
    if (sm.depth > 2) {
      char name[256];
      host_symbols::Describe(reinterpret_cast<uint64_t>(sm.frames[2]), name, sizeof(name));
      ++host[name];
      for (int f = 2; f < sm.depth; ++f) {
        host_symbols::Describe(reinterpret_cast<uint64_t>(sm.frames[f]), name, sizeof(name));
        if (std::string(name).rfind("libc.so", 0) != 0) {
          ++caller[name];
          break;
        }
      }
    }
    for (int f = 2; f < sm.depth; ++f) {
      if (const uint32_t g = host_symbols::GuestFunction(reinterpret_cast<uint64_t>(sm.frames[f]))) {
        ++guest[g];
        break;
      }
    }
  }
  auto top = [&](auto& counts, auto label) {
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, c] : counts) v.push_back({c, label(k)});
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
      out += fmt::format("{}{} {:.0f}%", i ? ", " : "", v[i].second, 100.0 * v[i].first / std::max(n, 1));
    }
    return out;
  };
  REXLOG_WARN("Slow load sampler: front end state {}, frames up to {:.0f} ms, {} samples over {} s", state,
              worst_ms, n, std::chrono::duration_cast<std::chrono::seconds>(kSampleFor).count());
  REXLOG_WARN("Slow load sampler: game functions: {}",
              top(guest, [](uint32_t g) { return fmt::format("sub_{:08X}", g); }));
  REXLOG_WARN("Slow load sampler: host functions: {}", top(host, [](const std::string& s) { return s; }));
  REXLOG_WARN("Slow load sampler: outside libc: {}", top(caller, [](const std::string& s) { return s; }));
}

void Run(int32_t state, double worst_ms) {
  const pid_t pid = getpid(), tid = g_main_tid.load();
  g_count = 0;
  const auto end = std::chrono::steady_clock::now() + kSampleFor;
  while (std::chrono::steady_clock::now() < end && g_count.load() < kMaxSamples) {
    syscall(SYS_tgkill, pid, tid, kSignal);
    std::this_thread::sleep_for(kEvery);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // (a last signal lands)
  Report(state, worst_ms);
  g_running = false;
}

// Every frame, on the game's main thread.
void OnFrameEnd(void*) {
  if (!g_main_tid.load(std::memory_order_relaxed)) g_main_tid = pid_t(syscall(SYS_gettid));
  if (g_running.load(std::memory_order_relaxed) || !REXCVAR_GET(debug_slow_load_sampler)) return;
  const double ms = pddi::LastFrameTiming().game_ms;
  const int32_t state = fight_tree::CurrentFrontEnd().state;
  if (!LoadingState(state) || ms < kSlowMs) {
    g_slow_in_a_row = 0;
    return;
  }
  if (++g_slow_in_a_row < kSlowFramesInARow) return;
  const auto now = std::chrono::steady_clock::now();
  if (g_last_run != std::chrono::steady_clock::time_point{} && now - g_last_run < kCooldown) return;
  g_last_run = now;
  g_running = true;
  std::thread(Run, state, ms).detach();
}

}  // namespace

void Install() {
  host_symbols::Load();  // (names read now, not in the middle of sampling)
  void* prime[2];
  backtrace(prime, 2);  // (loads the unwinder before any signal needs it)
  struct sigaction sa {};
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = OnSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(kSignal, &sa, nullptr);
  pddi::AddFrameEndListener(&OnFrameEnd, nullptr);
}

}  // namespace slow_load_sampler

#else

namespace slow_load_sampler {
void Install() {}
}  // namespace slow_load_sampler

#endif
