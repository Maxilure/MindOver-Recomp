// =============================================================================
// xbox_calls.cpp -- the "Xbox calls meter" (see xbox_calls.h)
// =============================================================================
//
// Per import: a call counter + up to kCallers distinct return addresses, all
// lock-free (the imports are called from every guest thread, some of them
// hundreds of thousands of times a second: RtlEnterCriticalSection,
// KeWaitForSingleObject...). Return addresses are turned into "function+offset"
// only when the report is made (FunctionContaining is a table search: too slow
// for the hot path).
// =============================================================================
#include "debug/xbox_calls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "debug/guest_stack.h"

REXCVAR_DEFINE_BOOL(xbox_calls_meter, true, "CrashMoM",
                    "Count every Xbox service (xboxkrnl/xam import) the game calls: first calls in the "
                    "log, the table in <log name>-xbox-calls.txt and the debug console's `xbox` (Linux)");

namespace xbox_calls {
namespace {

constexpr int kCallers = 8;  // distinct calling places remembered per import

struct Service {
  std::atomic<uint64_t> count{0};
  std::atomic<uint32_t> callers[kCallers] = {};  // guest return addresses, 0 = free slot
};

const char* const* g_names = nullptr;
uint32_t g_count = 0;
Service* g_services = nullptr;  // g_count entries, made in SetNames (never freed)

std::once_flag g_writer_started;

// "sub_823687A0+0x5C" for a guest return address (or the bare address).
std::string Where(uint32_t lr) {
  const uint32_t fn = guest_stack::FunctionContaining(lr);
  if (!fn) return fmt::format("0x{:08X}", lr);
  return fmt::format("sub_{:08X}+0x{:X}", fn, lr - fn);
}

// <log name>-xbox-calls.txt beside the session log, rewritten every 30 s
// (the window's close button ends the process with _Exit: no "at exit" hook).
void WriterThread() {
  const std::string log = rex::cvar::GetFlagByName("log_file");
  if (log.empty()) return;  // no session log: the console's `xbox` still works
  std::filesystem::path path(log);
  path.replace_filename(path.stem().string() + "-xbox-calls.txt");
  REXLOG_INFO("Xbox calls: table in {}", path.string());
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    const std::string text = Report();
    // Temp file + rename: a reader never sees half a table.
    const std::string tmp = path.string() + ".tmp";
    if (std::FILE* f = std::fopen(tmp.c_str(), "w")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
    }
  }
}

}  // namespace

void SetNames(const char* const* names, uint32_t count) {
  g_names = names;
  g_services = new Service[count];
  g_count = count;  // last: Hit() checks it
}

void Hit(uint32_t index, uint32_t lr) {
  if (index >= g_count) return;
  if (!REXCVAR_GET(xbox_calls_meter)) return;
  Service& s = g_services[index];
  const uint64_t before = s.count.fetch_add(1, std::memory_order_relaxed);
  // Remember the calling place if it's new and there's a free slot.
  for (auto& slot : s.callers) {
    uint32_t have = slot.load(std::memory_order_relaxed);
    if (have == lr) break;
    if (have == 0) {
      if (slot.compare_exchange_strong(have, lr, std::memory_order_relaxed)) break;
      if (have == lr) break;  // another thread stored the same one
    }
  }
  if (before == 0) {
    REXLOG_INFO("Xbox calls: first {} from {}", g_names[index], Where(lr));
    std::call_once(g_writer_started, [] { std::thread(WriterThread).detach(); });
  }
}

std::string Report() {
  struct Row {
    uint32_t index;
    uint64_t count;
  };
  std::vector<Row> used;
  std::vector<uint32_t> unused;
  for (uint32_t i = 0; i < g_count; ++i) {
    const uint64_t c = g_services[i].count.load(std::memory_order_relaxed);
    if (c) used.push_back({i, c});
    else unused.push_back(i);
  }
  // Most called first.
  std::sort(used.begin(), used.end(), [](const Row& a, const Row& b) { return a.count > b.count; });

  std::string out = fmt::format(
      "# Mind over Recomp: Xbox services the game called this session (xbox_calls.h)\n"
      "# {} of {} imports used. <count> <service> <calling places, up to {}>\n",
      used.size(), g_count, kCallers);
  for (const Row& r : used) {
    out += fmt::format("{:>12} {:<40}", r.count, g_names[r.index]);
    for (const auto& slot : g_services[r.index].callers) {
      const uint32_t lr = slot.load(std::memory_order_relaxed);
      if (lr) out += " " + Where(lr);
    }
    out += "\n";
  }
  out += fmt::format("# never called ({}):\n", unused.size());
  for (uint32_t i : unused) out += fmt::format("{:>12} {}\n", 0, g_names[i]);
  return out;
}

}  // namespace xbox_calls
