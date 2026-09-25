// =============================================================================
// pddi/trace.cpp -- log every renderer call of chosen frames
// =============================================================================
//
// A research tool for the native renderer (docs/04-native-renderer.md,
// docs/findings/08). OFF unless you pass --debug_pddi_trace_dir=<folder>.
//
// WHAT IT RECORDS
//   Every call that goes through the interception layer (intercept.h): ~460
//   PDDI methods and the ~90 D3D functions they call. For each traced frame
//   it writes one text file with every call, in order:
//     - which thread (M = the game's main thread, T1, T2... = others)
//     - nesting (a D3D call made by a PDDI method is indented under it)
//     - the function name (see functions.inc)
//     - `this` (r3) and the real class of that object, read from its vtable:
//       a call to xnShader::v4 on an xnBloomShader object says so
//     - arguments r4-r8 and f1-f2, the return value (r3, f1)
//     - the caller (guest return address): where in the game the call came from
//   and ends with a per-function call count for the frame.
//
// WHEN A FRAME IS TRACED
//   Frames are delimited by xnDisplay::SwapBuffers (sub_82433510): a traced
//   frame is everything from the end of one swap to the end of the next.
//     --debug_pddi_trace_ms=16000,40000   trace the first frame after each of
//                                         these times (ms since the game started)
//     --debug_pddi_trace_trigger=<file>   trace the next frame whenever <file>
//                                         appears (it's deleted); for live
//                                         use: `touch <file>` at the moment you want
//     --debug_pddi_trace_frames=N         trace N consecutive frames per trigger
//                                         (default 1), all in one file
//   Output: <dir>/pddi_f<frame number>_<ms>ms.txt
//
// The traces hold addresses and numbers only, but they describe the game's
// internals: keep them in logs/ or the scratchpad, like other debug output.
// =============================================================================

#include "trace.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(debug_pddi_trace_dir, "", "CrashMoM",
                      "Debug: write renderer (PDDI/D3D) call traces into this folder "
                      "(empty = off)");
REXCVAR_DEFINE_STRING(debug_pddi_trace_ms, "", "CrashMoM",
                      "Debug: comma-separated times (ms since start); trace the first "
                      "frame after each");
REXCVAR_DEFINE_STRING(debug_pddi_trace_trigger, "", "CrashMoM",
                      "Debug: trace the next frame whenever this file appears (it is "
                      "deleted)");
REXCVAR_DEFINE_INT32(debug_pddi_trace_frames, 1, "CrashMoM",
                     "Debug: consecutive frames per PDDI trace")
    .range(1, 600);

namespace pddi::trace {

namespace detail {
std::atomic<bool> g_recording{false};
}

namespace {

// One call. Filled at entry; the return values are added at exit.
struct Record {
  uint16_t fn;
  uint8_t depth;       // nesting on that thread
  uint8_t thread;      // 0 = main (the thread that swaps), 1.. = others
  uint32_t self;       // r3 at entry (`this` for methods)
  uint32_t vtable;     // *(this), 0 if `this` isn't readable memory
  uint32_t args[5];    // r4..r8
  double fargs[2];     // f1, f2
  uint32_t caller;     // guest return address (lr at entry)
  uint32_t ret = 0;    // r3 at exit
  double fret = 0;     // f1 at exit
  bool done = false;   // exit seen (false = still running when the frame ended)
};

std::mutex g_mutex;  // guards the records and the generation
std::vector<Record> g_records;
uint32_t g_generation = 0;  // bumped per traced frame (stale exits are ignored)
std::atomic<uint8_t> g_next_thread{1};

thread_local uint8_t t_depth = 0;
thread_local uint8_t t_thread = 0xFF;  // assigned on first traced call

// Main-thread-only state (OnFrameEnd only runs after SwapBuffers).
uint64_t g_frame = 0;
int g_frames_left = 0;
std::vector<int64_t> g_pending_ms;  // parsed --debug_pddi_trace_ms, sorted
bool g_config_parsed = false;
const auto g_start = std::chrono::steady_clock::now();

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               g_start)
      .count();
}

// Read a big-endian guest word, only if that page is readable (a bad `this`
// must never fault inside a debug tool).
uint32_t SafeLoad32(uint32_t address) {
  if (address < 0x10000 || (address & 3)) {
    return 0;
  }
  auto* memory = rex::system::kernel_memory();
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  uint32_t protect = 0;
  if (!heap || !heap->QueryProtect(address, &protect) ||
      !(protect & rex::memory::kMemoryProtectRead)) {
    return 0;
  }
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(memory->TranslateVirtual(address)));
}

void WriteTrace(const std::vector<Record>& records, uint64_t first_frame, int64_t ms) {
  std::filesystem::path dir = REXCVAR_GET(debug_pddi_trace_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char name[80];
  std::snprintf(name, sizeof(name), "pddi_f%06llu_%09lldms.txt", (unsigned long long)first_frame,
                (long long)ms);
  std::FILE* f = std::fopen((dir / name).string().c_str(), "w");
  if (!f) {
    REXLOG_ERROR("PddiTrace: cannot write {}", (dir / name).string());
    return;
  }
  std::fprintf(f,
               "# PDDI/D3D call trace: frame %llu (+%d), t=%lld ms, %zu calls\n"
               "# thread | nesting + function | this<class> | r4..r8 | f1 f2 | -> r3 f1 | "
               "caller\n",
               (unsigned long long)first_frame, REXCVAR_GET(debug_pddi_trace_frames) - 1,
               (long long)ms, records.size());
  std::vector<uint32_t> counts(kFnCount, 0);
  for (const Record& r : records) {
    ++counts[r.fn];
    char thread[8];
    if (r.thread == 0) {
      std::snprintf(thread, sizeof(thread), "M");
    } else {
      std::snprintf(thread, sizeof(thread), "T%u", r.thread);
    }
    const char* cls = ClassOfVtable(r.vtable);
    char self[48];
    if (cls) {
      std::snprintf(self, sizeof(self), "%08X<%s>", r.self, cls);
    } else if (r.vtable) {
      std::snprintf(self, sizeof(self), "%08X<vt %08X>", r.self, r.vtable);
    } else {
      std::snprintf(self, sizeof(self), "%08X", r.self);
    }
    std::fprintf(f, "%-3s %*s%s  this=%s  %08X %08X %08X %08X %08X  f=%g,%g", thread,
                 2 * r.depth, "", kFns[r.fn].name, self, r.args[0], r.args[1], r.args[2],
                 r.args[3], r.args[4], r.fargs[0], r.fargs[1]);
    if (r.done) {
      std::fprintf(f, "  -> %08X %g", r.ret, r.fret);
    } else {
      std::fprintf(f, "  -> (unfinished)");
    }
    std::fprintf(f, "  from %08X\n", r.caller);
  }
  // Per-function totals, most called first.
  std::vector<uint16_t> order;
  for (uint16_t i = 0; i < kFnCount; ++i) {
    if (counts[i]) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(),
            [&](uint16_t a, uint16_t b) { return counts[a] > counts[b]; });
  std::fprintf(f, "\n# calls per function\n");
  for (uint16_t i : order) {
    std::fprintf(f, "# %7u  %08X  %s\n", counts[i], kFns[i].address, kFns[i].name);
  }
  std::fclose(f);
  REXLOG_INFO("PddiTrace: wrote {} calls to {}", records.size(), (dir / name).string());
}

}  // namespace

Token Enter(FnId fn, const PPCContext& ctx) {
  if (t_thread == 0xFF) {
    // The main thread is tagged 0 at its first frame end (OnFrameEnd);
    // any other thread gets the next number.
    t_thread = g_next_thread.fetch_add(1);
  }
  Record r{};
  r.fn = fn;
  r.depth = t_depth;
  r.thread = t_thread;
  r.self = ctx.r3.u32;
  r.vtable = SafeLoad32(ctx.r3.u32);
  r.args[0] = ctx.r4.u32;
  r.args[1] = ctx.r5.u32;
  r.args[2] = ctx.r6.u32;
  r.args[3] = ctx.r7.u32;
  r.args[4] = ctx.r8.u32;
  r.fargs[0] = ctx.f1.f64;
  r.fargs[1] = ctx.f2.f64;
  r.caller = uint32_t(ctx.lr);
  Token token;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    token.index = g_records.size();
    token.generation = g_generation;
    g_records.push_back(r);
  }
  ++t_depth;
  return token;
}

void Exit(const Token& token, const PPCContext& ctx) {
  --t_depth;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (token.generation == g_generation && token.index < g_records.size()) {
    g_records[token.index].ret = ctx.r3.u32;
    g_records[token.index].fret = ctx.f1.f64;
    g_records[token.index].done = true;
  }
}

void OnFrameEnd() {
  if (t_thread == 0xFF) {
    t_thread = 0;  // the thread that swaps is the main thread
  }
  ++g_frame;
  if (REXCVAR_GET(debug_pddi_trace_dir).empty()) {
    return;
  }
  if (!g_config_parsed) {
    g_config_parsed = true;
    // "16000,40000" -> {16000, 40000}
    std::string_view list = REXCVAR_GET(debug_pddi_trace_ms);
    while (!list.empty()) {
      size_t comma = list.find(',');
      std::string_view item = list.substr(0, comma);
      int64_t v = 0;
      if (std::from_chars(item.data(), item.data() + item.size(), v).ec == std::errc()) {
        g_pending_ms.push_back(v);
      }
      list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    std::sort(g_pending_ms.begin(), g_pending_ms.end());
    REXLOG_INFO("PddiTrace: {} timed trace(s), trigger file '{}', output in {}",
                g_pending_ms.size(), REXCVAR_GET(debug_pddi_trace_trigger),
                REXCVAR_GET(debug_pddi_trace_dir));
  }

  // A traced frame just ended: stop and write it out.
  if (Recording()) {
    if (--g_frames_left > 0) {
      return;
    }
    std::vector<Record> records;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      detail::g_recording.store(false);
      ++g_generation;
      records.swap(g_records);
    }
    WriteTrace(records, g_frame - REXCVAR_GET(debug_pddi_trace_frames), NowMs());
    return;
  }

  // Should the next frame be traced?
  const int64_t now = NowMs();
  bool start = false;
  while (!g_pending_ms.empty() && g_pending_ms.front() <= now) {
    g_pending_ms.erase(g_pending_ms.begin());
    start = true;
  }
  const std::string& trigger = REXCVAR_GET(debug_pddi_trace_trigger);
  if (!trigger.empty()) {
    std::error_code ec;
    if (std::filesystem::remove(trigger, ec)) {  // true = it existed
      start = true;
    }
  }
  if (start) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.clear();
    g_records.reserve(1 << 16);
    ++g_generation;
    g_frames_left = REXCVAR_GET(debug_pddi_trace_frames);
    detail::g_recording.store(true);
    REXLOG_INFO("PddiTrace: tracing from frame {} (t={} ms)", g_frame, now);
  }
}

}  // namespace pddi::trace
