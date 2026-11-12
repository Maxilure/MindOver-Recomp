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

#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
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
constexpr int kDepth = 128;                // host frames kept per sample (deep: inclusive counts need the callers)
constexpr int kMaxSamples = 2400;          // slow-load runs (4 s every 2 ms)
constexpr int kMaxProfileSamples = 60000;  // profile runs: 60 s every 1 ms
const int kSignal = SIGRTMIN + 12;         // (the runtime itself uses SIGRTMIN + 1)

struct Sample {
  int depth = 0;
  void* frames[kDepth];
};
// Sized BEFORE a run starts (never allocated in the signal handler); g_capacity = its size.
std::vector<Sample> g_samples;
std::atomic<int> g_capacity{0};
std::atomic<int> g_count{0};
std::atomic<bool> g_running{false};

// A profile run's frame timings (pddi::LastFrameTiming, summed per game frame while it samples).
struct FrameSums {
  int frames = 0;
  double game_ms = 0, swap_ms = 0, listeners_ms = 0, worst_game_ms = 0;
};
std::atomic<bool> g_profiling{false};
FrameSums g_sums;  // main thread writes while g_profiling, the sampler reads after it's cleared
std::atomic<pid_t> g_main_tid{0};
// A profile run's thread: the main thread unless `profile` named another one.
std::atomic<pid_t> g_profile_tid{0};
std::string g_profile_thread = "the game's main thread";
int g_slow_in_a_row = 0;
std::chrono::steady_clock::time_point g_last_run{};

// In the game's main thread, at the timer thread's signal: its stack.
void OnSignal(int, siginfo_t*, void*) {
  const int i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= g_capacity.load(std::memory_order_relaxed)) return;
  g_samples[size_t(i)].depth = backtrace(g_samples[size_t(i)].frames, kDepth);
}

bool LoadingState(int32_t state) { return state >= 51 && state <= 53; }

void Report(int32_t state, double worst_ms) {
  const int n = std::min(g_count.load(), g_capacity.load());
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
  if (g_profiling.load(std::memory_order_acquire)) {
    const pddi::FrameTiming& t = pddi::LastFrameTiming();
    ++g_sums.frames;
    g_sums.game_ms += t.game_ms;
    g_sums.swap_ms += t.swap_ms;
    g_sums.listeners_ms += t.listeners_ms;
    g_sums.worst_game_ms = std::max(g_sums.worst_game_ms, t.game_ms);
  }
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
  bool expected = false;  // (a console profile may have claimed the sampler meanwhile)
  if (!g_running.compare_exchange_strong(expected, true)) return;
  g_last_run = now;
  g_samples.resize(kMaxSamples);  // (main thread, before the sampler thread exists)
  g_capacity = kMaxSamples;
  std::thread(Run, state, ms).detach();
}

// --- PROFILE RUNS (console `profile <seconds> [file]`, 2026-11-09) -----------
//
// Same signal + stack walk, any time (gameplay), every 1 ms, and a fuller
// report in a file:
//   SELF      = the innermost game function of each sample: where the time is
//               actually spent (leaves: memcpy-like loops, math).
//   INCLUSIVE = every distinct game function on the sample's stack, once: the
//               big systems that CONTAIN the time (AI update, the renderer...).
//   HOST      = innermost host function (ours, the SDK's, libc), and our / the
//               SDK's own functions counted inclusively (the recompiled game's
//               __imp__sub_X frames left out).
// A sample with no game function and no host frames past the signal = idle.

std::string g_profile_path;

// "name+0x12" -> "name", demangled (report time only: allocates).
std::string FunctionName(uint64_t address) {
  char raw[512];
  host_symbols::Describe(address, raw, sizeof(raw));
  std::string name(raw);
  if (const size_t plus = name.rfind("+0x"); plus != std::string::npos) name.resize(plus);
  const size_t bang = name.find('!');  // "libfoo.so!symbol"
  std::string file = bang == std::string::npos ? "" : name.substr(0, bang + 1);
  std::string symbol = bang == std::string::npos ? name : name.substr(bang + 1);
  int status = 0;
  if (char* d = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status)) {
    if (status == 0) symbol = d;
    std::free(d);
  }
  // Templates and argument lists make names unreadable: keep "Class::Function".
  // "(anonymous namespace)" is a parenthesis too: shortened first, else the
  // name would be cut there (a file's private functions all became "ns::").
  for (size_t at; (at = symbol.find("(anonymous namespace)")) != std::string::npos;) {
    symbol.replace(at, 21, "{anon}");
  }
  if (const size_t paren = symbol.find('('); paren != std::string::npos) symbol.resize(paren);
  if (symbol.empty()) symbol = raw;  // (never an empty label)
  return file + symbol;
}

void ProfileReport(double seconds) {
  const int n = std::min(g_count.load(), g_capacity.load());
  const FrameSums sums = g_sums;
  struct Resolved {
    uint32_t guest = 0;   // game function, or 0
    std::string host;     // host function name (also for game functions: the __imp__ wrapper's)
  };
  std::unordered_map<uint64_t, Resolved> cache;  // host address -> what it is (resolved once)
  auto resolve = [&](void* frame) -> const Resolved& {
    const uint64_t a = reinterpret_cast<uint64_t>(frame);
    auto it = cache.find(a);
    if (it != cache.end()) return it->second;
    Resolved r;
    r.guest = host_symbols::GuestFunction(a);
    if (!r.guest) r.host = FunctionName(a);
    return cache.emplace(a, std::move(r)).first->second;
  };
  std::map<uint32_t, int> self, inclusive;
  std::map<std::string, int> host_self, host_outside_libc, host_inclusive;
  // "Called from": for each sample's innermost-outside-libraries function, the
  // next DIFFERENT function up the stack (its caller).
  std::map<std::string, std::map<std::string, int>> called_from;
  // BY AREA (2026-11-12): which PART of the game's code the time belongs to,
  // by address: the Xbox graphics stack (Microsoft's D3D, Radical's PDDI Xbox
  // back end xn*) vs everything else. Per area: samples whose innermost game
  // function is there, split into "the CPU was in that game code itself" and
  // "in host code it called" (our renderer's handlers, the SDK); plus
  // inclusive (an area function anywhere on the stack).
  enum Area { kD3D, kXn, kOther, kAreas };
  auto area_of = [](uint32_t g) {
    if ((g >= 0x82305000 && g < 0x82323000) || (g >= 0x82476000 && g < 0x82478000)) return kD3D;
    if (g >= 0x82420000 && g < 0x82460000) return kXn;
    return kOther;
  };
  int area_game[kAreas] = {}, area_host[kAreas] = {}, area_incl[kAreas] = {};
  std::map<std::string, int> raw_frames;  // "library offset" of unnamed library frames
  std::map<uint32_t, int> own;  // samples with the CPU in that game function's own code
  int idle = 0;
  for (int i = 0; i < n; ++i) {
    const Sample& sm = g_samples[size_t(i)];
    if (sm.depth <= 2) {
      ++idle;
      continue;
    }
    bool have_self = false, have_outside = false;
    std::string outside_name;  // the innermost-outside function, while its caller is looked for
    bool want_caller = false;
    std::set<uint32_t> seen_guest;
    std::set<std::string> seen_host;
    bool area_counted = false, area_seen[kAreas] = {};
    bool have_raw = false;  // the innermost unnamed library frame of this sample is counted
    for (int f = 2; f < sm.depth; ++f) {  // [0] = OnSignal, [1] = the signal trampoline
      const Resolved& r = resolve(sm.frames[f]);
      if (want_caller) {
        const std::string name = r.guest ? fmt::format("(game) sub_{:08X}", r.guest) : r.host;
        if (name != outside_name) {
          ++called_from[outside_name][name];
          want_caller = false;
        }
      }
      if (f == 2) ++host_self[r.guest ? fmt::format("(game) sub_{:08X}", r.guest) : r.host];
      // The CPU was in this game function's OWN code (not in host code it
      // called): the "ALL GAME FUNCTIONS" appendix at the end of the report.
      if (f == 2 && r.guest) ++own[r.guest];
      // A frame in a shared library with no symbol name (the SDK's GPU
      // plugin exports none): kept as library+offset for llvm-symbolizer.
      if (!r.guest && r.host.find('!') == std::string::npos && r.host.find(".so") != std::string::npos &&
          !have_raw) {
        Dl_info info{};
        if (dladdr(sm.frames[f], &info) && info.dli_fname) {
          std::string lib = info.dli_fname;
          ++raw_frames[fmt::format("{} 0x{:x}", lib,
                                   reinterpret_cast<uint64_t>(sm.frames[f]) -
                                       reinterpret_cast<uint64_t>(info.dli_fbase))];
          have_raw = true;
        }
      }
      if (r.guest) {
        const Area a = area_of(r.guest);
        if (!area_counted) {
          ++(f == 2 ? area_game : area_host)[a];
          area_counted = true;
        }
        if (!area_seen[a]) {
          area_seen[a] = true;
          ++area_incl[a];
        }
        if (!have_self) {
          ++self[r.guest];
          have_self = true;
        }
        if (seen_guest.insert(r.guest).second) ++inclusive[r.guest];
        if (!have_outside) {  // a game function called the library routine
          outside_name = fmt::format("(game) sub_{:08X}", r.guest);
          ++host_outside_libc[outside_name];
          have_outside = want_caller = true;
        }
        continue;
      }
      // A system library (libc, libm, the GPU driver, Vulkan's loader...): the
      // "who called it" table skips past it to our / the SDK's caller.
      const bool libc = (r.host.find(".so") != std::string::npos && r.host.rfind("librex", 0) != 0) ||
                        r.host.rfind("linux-vdso", 0) == 0;
      if (!libc && !have_outside) {
        outside_name = r.host;
        ++host_outside_libc[r.host];
        have_outside = want_caller = true;
      }
      if (!libc && seen_host.insert(r.host).second) ++host_inclusive[r.host];
    }
  }
  auto pct = [&](int c) { return 100.0 * c / std::max(n, 1); };
  std::string out;
  out += fmt::format("PROFILE of {}: {} samples over {:.1f} s (every 1 ms)\n", g_profile_thread, n, seconds);
  if (sums.frames) {
    out += fmt::format(
        "game frames: {} ({:.1f} fps); per frame: game {:.2f} ms (worst {:.1f}), SwapBuffers {:.2f} ms, "
        "frame-end listeners {:.2f} ms\n",
        sums.frames, sums.frames / std::max(seconds, 0.001), sums.game_ms / sums.frames, sums.worst_game_ms,
        sums.swap_ms / sums.frames, sums.listeners_ms / sums.frames);
  }
  out += fmt::format("idle samples (no stack): {} ({:.1f}%)\n", idle, pct(idle));
  auto table = [&](const char* title, auto& counts, auto label, size_t rows) {
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, c] : counts) v.push_back({c, label(k)});
    std::sort(v.rbegin(), v.rend());
    out += fmt::format("\n== {} ==\n", title);
    for (size_t i = 0; i < v.size() && i < rows; ++i) {
      out += fmt::format("{:6.2f}%  {:6}  {}\n", pct(v[i].first), v[i].first, v[i].second);
    }
  };
  {
    static const char* kNames[kAreas] = {"Xbox Direct3D (Microsoft, 0x82305000-0x82323000 + draws 0x82476000-0x82478000)",
                                         "PDDI Xbox back end (Radical's xn* classes, 0x82420000-0x82460000)",
                                         "everything else"};
    out += "\n== BY AREA (innermost game function's area: in its own code / in host code it called; inclusive) ==\n";
    for (int a = 0; a < kAreas; ++a) {
      out += fmt::format("{:6.2f}% own code  {:6.2f}% host below  {:6.2f}% inclusive  {}\n", pct(area_game[a]),
                         pct(area_host[a]), pct(area_incl[a]), kNames[a]);
    }
  }
  auto sub = [](uint32_t g) { return fmt::format("sub_{:08X}", g); };
  auto same = [](const std::string& x) { return x; };
  table("GAME FUNCTIONS, SELF (innermost game function of each sample)", self, sub, 60);
  table("GAME FUNCTIONS, INCLUSIVE (anywhere on the stack)", inclusive, sub, 80);
  table("HOST, INNERMOST FRAME (where the CPU actually was)", host_self, same, 40);
  table("HOST, INNERMOST OUTSIDE SYSTEM LIBRARIES (libc, libm, GPU driver...: who called them)", host_outside_libc, same, 40);
  table("HOST, INCLUSIVE (our code + the SDK; recompiled game frames left out)", host_inclusive, same, 60);
  {
    // The 15 biggest "outside libraries" entries, each with its top 5 callers.
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, c] : host_outside_libc) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    out += "\n== CALLED FROM (the biggest entries of the 'outside system libraries' table) ==\n";
    for (size_t i = 0; i < v.size() && i < 15; ++i) {
      out += fmt::format("{:6.2f}%  {}\n", pct(v[i].first), v[i].second);
      std::vector<std::pair<int, std::string>> callers;
      for (auto& [k, c] : called_from[v[i].second]) callers.push_back({c, k});
      std::sort(callers.rbegin(), callers.rend());
      for (size_t j = 0; j < callers.size() && j < 5; ++j) {
        out += fmt::format("          {:6.2f}%  <- {}\n", pct(callers[j].first), callers[j].second);
      }
    }
  }

  table("UNNAMED LIBRARY FRAMES (innermost per sample: 'file offset', name them with llvm-symbolizer --obj=file)",
        raw_frames, same, 400);
  {
    // APPENDIX: every game function seen, with its own-code samples (the CPU
    // was in that function's instructions), self (innermost game function,
    // host code below it included) and inclusive counts, sorted by address:
    // for summing whole address ranges or a list of functions afterwards
    // (e.g. all material set-ups, findings/60). Columns are sample counts.
    out += "\n== ALL GAME FUNCTIONS (address, own code, self, inclusive; sample counts) ==\n";
    for (auto& [g, c] : inclusive) {
      const auto o = own.find(g);
      const auto s2 = self.find(g);
      out += fmt::format("sub_{:08X} {} {} {}\n", g, o == own.end() ? 0 : o->second,
                         s2 == self.end() ? 0 : s2->second, c);
    }
  }
  bool written = false;
  if (FILE* f = std::fopen(g_profile_path.c_str(), "w")) {
    written = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
  }
  // A short version in the log too (top 8 self).
  std::vector<std::pair<int, uint32_t>> top;
  for (auto& [g, c] : self) top.push_back({c, g});
  std::sort(top.rbegin(), top.rend());
  std::string brief;
  for (size_t i = 0; i < top.size() && i < 8; ++i) {
    brief += fmt::format("{}sub_{:08X} {:.1f}%", i ? ", " : "", top[i].second, pct(top[i].first));
  }
  REXLOG_INFO("Profile: {} samples, top game functions (self): {}", n, brief);
  REXLOG_INFO("Profile: report {} {}", written ? "written to" : "COULDN'T be written to", g_profile_path);
}


// The first thread of this process whose name (/proc/self/task/<tid>/comm,
// 15 characters at most) starts with `prefix`; 0 = none.
pid_t ThreadNamed(const std::string& prefix) {
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char path[64], comm[64] = {};
      std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
      if (FILE* f = std::fopen(path, "r")) {
        if (std::fgets(comm, sizeof(comm), f) && std::string(comm).rfind(prefix, 0) == 0) {
          std::fclose(f);
          closedir(d);
          return pid_t(std::atoi(e->d_name));
        }
        std::fclose(f);
      }
    }
    closedir(d);
  }
  return 0;
}

void ProfileRun(double seconds) {
  const pid_t pid = getpid(), tid = g_profile_tid.load();
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end && g_count.load() < g_capacity.load()) {
    syscall(SYS_tgkill, pid, tid, kSignal);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  g_profiling.store(false, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // (a last signal + frame end land)
  ProfileReport(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  g_running = false;
}

}  // namespace

std::string StartProfile(double seconds, const std::string& path, const std::string& thread) {
  if (!g_main_tid.load()) return "the game's main thread hasn't finished a frame yet\n";
  if (g_running.load()) return "a sampler run is already under way\n";
  if (thread.empty()) {
    g_profile_tid = g_main_tid.load();
    g_profile_thread = "the game's main thread";
  } else {
    const pid_t t = ThreadNamed(thread);
    if (!t) return "no thread named '" + thread + "...'\n";
    g_profile_tid = t;
    g_profile_thread = "thread '" + thread + "' (tid " + std::to_string(t) + ")";
  }
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true)) return "a sampler run is already under way\n";
  seconds = std::clamp(seconds, 0.5, 60.0);
  g_profile_path = path;
  g_samples.resize(size_t(kMaxProfileSamples));
  g_capacity = kMaxProfileSamples;
  g_count = 0;
  g_sums = FrameSums{};  // (the main thread only sums while g_profiling is set)
  g_profiling.store(true, std::memory_order_release);
  std::thread(ProfileRun, seconds).detach();
  return fmt::format("profiling the main thread for {:.1f} s -> {} (\"wait profile\" waits)\n", seconds, path);
}

bool ProfileRunning() { return g_profiling.load() || g_running.load(); }

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

#include <string>

namespace slow_load_sampler {
void Install() {}
std::string StartProfile(double, const std::string&, const std::string&) { return "profiling needs Linux\n"; }
bool ProfileRunning() { return false; }
}  // namespace slow_load_sampler

#endif
