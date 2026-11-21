// memory_survey.cpp -- see memory_survey.h for what is measured and why.
#include "memory_survey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "../pddi/intercept.h"
#include "guest_stack.h"
#include "../system/guest_pool.h"

REXCVAR_DEFINE_INT32(debug_memory_log_s, 60, "CrashMoM",
                     "Seconds between 'Memory:' log lines (the game's heaps + guest physical memory; "
                     "0 = never; src/debug/memory_survey.h)");

extern "C" REX_FUNC(__imp__sub_8227F130);  // heap report: r3 heap id, r4 &used, r5 &total, r6 &third

namespace memory_survey {
namespace {

constexpr uint32_t kHeapTable = 0x8256F1D0;    // 16 heaps x 16 bytes, +0 the heap object
constexpr uint32_t kRegionTable = 0x8256D1B0;  // regions: +0 start, +4 heap id (8 bytes each)
constexpr uint32_t kRegionCount = 0x825711E4;  // how many regions
constexpr uint32_t kCurrentHeap = 0x825711EC;  // the heap `new` takes from
constexpr int kHeaps = 16;

rex::memory::Memory* Mem() { return rex::system::kernel_memory(); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Mem()->TranslateVirtual<const uint8_t*>(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// "sub_X+0xNN" for a guest return address (the caller of what we wrapped).
std::string Caller(uint32_t lr) {
  const uint32_t f = guest_stack::FunctionContaining(lr);
  return f ? fmt::format("sub_{:08X}+0x{:X}", f, lr - f) : fmt::format("{:08X}", lr);
}

// An SDK heap's totals: reserved bytes, free bytes, the largest free run
// (what a big contiguous request can still get).
struct Span {
  uint64_t size = 0, free = 0, largest = 0;
};
Span Walk(rex::memory::BaseHeap* heap) {
  Span s;
  if (!heap) return s;
  s.size = heap->heap_size();
  const uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
  uint64_t a = heap->heap_base();
  while (a < end) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(a), &info)) break;
    uint64_t next;
    if (info.state == 0) {
      s.free += info.region_size;
      s.largest = std::max<uint64_t>(s.largest, info.region_size);
      next = a + std::max<uint32_t>(info.region_size, heap->page_size());
    } else {
      next = uint64_t(info.allocation_base) + std::max<uint32_t>(info.allocation_size, heap->page_size());
    }
    if (next <= a) next = a + heap->page_size();
    a = next;
  }
  return s;
}

std::string MB(uint64_t bytes) { return fmt::format("{:.1f}", double(bytes) / (1 << 20)); }

struct HeapNumbers {
  uint32_t object = 0, vtable = 0, used = 0, total = 0, third = 0, peak = 0;
};

struct State {
  std::mutex lock;
  std::condition_variable fresh_cv;
  std::string report;  // the latest, for the console
  uint64_t report_number = 0;
  std::atomic<bool> want_fresh{false};
  std::array<HeapNumbers, kHeaps> heaps{};
  uint64_t physical_low_free = ~0ull;  // lowest physical free seen
  uint32_t scratch = 0;                // 16 guest bytes for the game's heap report
  std::chrono::steady_clock::time_point last_refresh{}, last_log{};
  std::atomic<int> refused_heap{0}, refused_physical{0};
};
State g;

// On the game thread: ask every heap (the game's own report function),
// read the SDK's heaps, build the report text.
void Refresh(bool log) {
  PPCContext* live = rex::runtime::current_ppc_context();
  if (!live) return;
  if (!g.scratch) g.scratch = Mem()->SystemHeapAlloc(16);
  if (!g.scratch) return;
  uint8_t* base = Mem()->virtual_membase();
  std::string heaps_line, detail;
  for (int i = 0; i < kHeaps; ++i) {
    HeapNumbers& h = g.heaps[i];
    const uint32_t object = Read32(kHeapTable + 16 * i);
    if (object != h.object) h.peak = 0;  // a new heap in this slot: its own peak
    h.object = object;
    if (h.object < 0x40000000) continue;
    h.vtable = Read32(h.object);
    PPCContext ctx = *live;
    ctx.r3.u64 = uint64_t(i);
    ctx.r4.u64 = g.scratch;
    ctx.r5.u64 = g.scratch + 4;
    ctx.r6.u64 = g.scratch + 8;
    __imp__sub_8227F130(ctx, base);
    h.used = Read32(g.scratch);
    h.total = Read32(g.scratch + 4);
    h.third = Read32(g.scratch + 8);
    h.peak = std::max(h.peak, h.used);
    heaps_line += fmt::format(" h{} {}/{}", i, h.used >> 10, h.total >> 10);
    const double pct = h.total ? 100.0 * h.used / h.total : 0.0;
    detail += fmt::format("  heap {:2} object {:08X} vtable {:08X}: used {:>9} of {:>9} KB ({:5.1f}%), peak {:>9} KB, "
                          "third {:08X}\n",
                          i, h.object, h.vtable, h.used >> 10, h.total >> 10, pct, h.peak >> 10, h.third);
  }
  // Which address ranges belong to which heap (the game's region table).
  std::string regions;
  const uint32_t count = std::min<uint32_t>(Read32(kRegionCount), 64);
  for (uint32_t r = 0; r < count; ++r) {
    regions += fmt::format(" {:08X}:h{}", Read32(kRegionTable + 8 * r), Read32(kRegionTable + 8 * r + 4));
  }
  // The SDK's guest memory.
  auto* m = Mem();
  const Span phys = Walk(m->GetPhysicalHeap());
  g.physical_low_free = std::min(g.physical_low_free, phys.free);
  const Span v0 = Walk(m->LookupHeap(0x10000000));
  const Span v4 = Walk(m->LookupHeap(0x40000000));
  const Span a0 = Walk(m->LookupHeap(0xA0000000));
  const Span c0 = Walk(m->LookupHeap(0xC0000000));
  const Span e0 = Walk(m->LookupHeap(0xE0000000));
  const std::string sdk = fmt::format(
      "physical free {} MB (largest {}, lowest seen {}) of {} | windows used A0 {} C0 {} E0 {} MB | "
      "virtual 40000000 free {} MB (largest {}) of {} | virtual 00000000 free {} MB (largest {})",
      MB(phys.free), MB(phys.largest), MB(g.physical_low_free), MB(phys.size), MB(a0.size - a0.free),
      MB(c0.size - c0.free), MB(e0.size - e0.free), MB(v4.free), MB(v4.largest), MB(v4.size), MB(v0.free),
      MB(v0.largest));
  std::string text = fmt::format("current heap {}\n{}regions:{}\n{}\n{}refused so far: {} heap, {} physical\n",
                                 Read32(kCurrentHeap), detail, regions, sdk, guest_pool::Summary(),
                                 g.refused_heap.load(), g.refused_physical.load());
  if (log) REXLOG_INFO("Memory:{} | {} | pool {} MB used", heaps_line, sdk, guest_pool::UsedBy() >> 20);
  {
    std::lock_guard<std::mutex> l(g.lock);
    g.report = std::move(text);
    ++g.report_number;
  }
  g.fresh_cv.notify_all();
}

void OnFrameEnd(void*) {
  const auto now = std::chrono::steady_clock::now();
  const int32_t log_s = REXCVAR_GET(debug_memory_log_s);
  const bool log = log_s > 0 && now - g.last_log >= std::chrono::seconds(log_s);
  const bool fresh = g.want_fresh.exchange(false);
  if (!log && !fresh && now - g.last_refresh < std::chrono::seconds(1)) return;
  g.last_refresh = now;
  if (log) g.last_log = now;
  Refresh(log);
}

}  // namespace

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

int NoteRefused(bool physical) { return (physical ? g.refused_physical : g.refused_heap).fetch_add(1); }

std::string CallerName(uint32_t lr) { return Caller(lr); }

std::string PhysicalFreeText() {
  const Span s = Walk(Mem()->GetPhysicalHeap());
  return fmt::format("physical free {} MB, largest {} MB", MB(s.free), MB(s.largest));
}

// DEBUG REPRO (`memory neighbours-all <zone>`): the zone's neighbour list
// (zone +0x198, sorted {begin, end}) is pointed at a sorted copy of EVERY
// zone of the level. With split screen on and a player standing in that
// zone (not the camera's focus player: the original's own lists hold 10),
// split_screen.cpp's "every player's zone + neighbours" then asks for the
// whole level = the Wumpa Island out-of-memory test (findings/71).
std::string NeighboursAll(uint32_t zone) {
  constexpr uint32_t kGameGlobal = 0x8259B190, kManager = 100, kZonesBegin = 0x14, kZonesEnd = 0x18;
  constexpr uint32_t kNeighbours = 0x198;
  const uint32_t manager = Read32(Read32(kGameGlobal) + kManager);
  const uint32_t z0 = Read32(manager + kZonesBegin), z1 = Read32(manager + kZonesEnd);
  if (z0 < 0x40000000 || z1 <= z0 || (z1 - z0) / 4 > 256) return "no zone list\n";
  std::vector<uint32_t> zones;
  for (uint32_t a = z0; a < z1; a += 4) zones.push_back(Read32(a));
  std::sort(zones.begin(), zones.end());
  static uint32_t copy = 0;
  if (!copy) copy = Mem()->SystemHeapAlloc(4 * 256);
  if (!copy) return "no memory\n";
  auto w32 = [](uint32_t a, uint32_t v) {
    uint8_t* p = Mem()->TranslateVirtual<uint8_t*>(a);
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
  };
  for (size_t i = 0; i < zones.size(); ++i) w32(copy + 4 * uint32_t(i), zones[i]);
  w32(zone + kNeighbours, copy);
  w32(zone + kNeighbours + 4, copy + 4 * uint32_t(zones.size()));
  REXLOG_WARN("Memory: DEBUG zone {:08X} now lists all {} zones of the level as its neighbours", zone, zones.size());
  return fmt::format("zone {:08X}: {} neighbours\n", zone, zones.size());
}

std::string Report(bool fresh) {
  std::unique_lock<std::mutex> l(g.lock);
  if (fresh) {
    const uint64_t before = g.report_number;
    g.want_fresh = true;
    g.fresh_cv.wait_for(l, std::chrono::milliseconds(500), [&] { return g.report_number != before; });
  }
  return g.report.empty() ? std::string("no game frame yet\n") : g.report;
}

}  // namespace memory_survey

