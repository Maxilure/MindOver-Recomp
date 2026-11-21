// =============================================================================
// game_memory.cpp -- where the game's memory comes from (the port's memory
// manager, step 1) + every refused allocation named
// =============================================================================
//
// WHY (docs/findings/71-memory.md): the Xbox 360 has ONE 512 MB block of
// physical memory, and the game puts everything in it: the graphics data
// (textures, vertex / index buffers, through D3D's XMemAlloc ->
// XPhysicalAlloc) AND all of Radical's heaps, which live inside one 135 MB
// block the game takes at boot (sub_8235D9D8: XPhysicalAlloc(0x8700000), then
// its root "MainMemory" allocator, MemoryDlAllocator, is built in it; every
// other heap - level, permanent, front end... - is carved from that one).
// When split screen kept all 26 zones of Wumpa Island loaded, the physical
// memory ran out (a 1 MB texture refused at 0.3 MB free) and the texture
// loader crashed on the null it got back.
//
// STEP 1 (this file, setting main_heap_virtual, on by default): that 135 MB
// block comes from the guest's VIRTUAL memory (the 0x40000000 range: ~1 GB,
// nearly unused) instead. Game pointers stay 32-bit guest addresses, the
// heaps work exactly as before; the physical memory is left to the graphics
// (Wumpa Island: 142 -> 277 MB free; the whole level then fits with 130 MB
// to spare).
//
// ONLY WITHOUT THE EMULATED GPU (renderer = "native", the default): the
// emulated GPU reads guest memory by PHYSICAL address, and a virtual block
// has none, so if the game ever hands heap memory to the GPU the emulated
// picture would break. The native renderer reads guest memory directly. With
// --renderer=emulated (a launch option) the block stays physical: the
// original's 512 MB limits apply there.
//
// STEP 2 (setting heap_growth, on by default, same condition): NO HEAP IS
// FULL ANY MORE. Every game heap keeps working exactly as on the console
// (same allocator, same addresses) until it is full; then, instead of
// failing, the request is answered from the port's own pool
// (system/guest_pool.h: guest virtual memory, grown as needed). Hooked:
//   - MemoryDlAllocator (Radical's dlmalloc heap, vtable 0x8200B3F4: the root
//     "MainMemory" heap AND the inside of every CDLMemoryAllocator heap: Temp,
//     Script, Sound, FrontEnd, Level, heap 0): alloc slot +0x0C sub_82360F88,
//     aligned alloc +0x18 sub_82360FF8 -> pool when the original returns 0;
//     free +0x10 sub_82360430 / +0x1C sub_82360378, "is it mine" +0x14/+0x20
//     sub_823600E8 and the destructor +0x08 sub_823601B0 (the heap goes away
//     = its pool blocks go too) know the pool's blocks;
//   - `delete` sub_82357598 and the heap-less free sub_82357540: they find
//     the heap from the address's REGION (table 0x8256D1B0), which a pool
//     block has none of -> freed by the pool first;
//   - the PERMANENT rule: `new` asserted (a DbgBreakPoint = the game ended) on
//     any permanent-heap allocation once the level heap existed (byMemory.cpp
//     "( allocator != GAME_ALLOCATOR_PERMANENT ) || s_LevelHeapAllocated ==
//     false"): midasm CrashMomPermanentAfterLevel 0x8227E92C reads the flag
//     as 0 there. The permanent heap (92 MB, ~1.5 MB used) takes them.
// Only heaps in the game's heap table (0x8256F1D0) grow; a private
// MemoryDlAllocator (one is made for a moment in sub_8227AB58) fails like the
// original. A pool block is logged the first time each heap grows:
// "Memory: heap 7 (FrontEnd) is full: it grows into the port's pool".
// Test switch: --debug_heap_force_pool=7,8 sends EVERY allocation of those
// heaps to the pool (the pool's paths exercised all the time).
//
// NEXT (findings/71 s.4 step 3): graphics memory past the 512 MB.
//
// ALSO HERE: every refused allocation is logged with the calling game
// function (the first 50 of each kind; counts in the console's `memory`):
//   "Memory: heap 8 refused 1048576 bytes for sub_X+0xNN"          (Radical)
//   "Memory: XAPI heap ... refused ..."                              (D3D's small objects)
//   "Memory: physical allocation of N bytes ... refused (...)"       (graphics data)
// plus one line per physical block of 8 MB or more (the boot-time pools).
// =============================================================================
#include <algorithm>
#include <atomic>
#include <vector>
#include <fmt/format.h>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game_memory.h"
#include "../debug/memory_survey.h"
#include "guest_pool.h"
#include "../debug/guest_stack.h"
#include "command_ring.h"

REXCVAR_DEFINE_BOOL(main_heap_virtual, true, "CrashMoM",
                    "Launch setting: the game's main heap (135 MB, every one of its heaps) in guest virtual "
                    "memory, leaving the 512 MB of physical memory to the graphics (native renderer only; "
                    "src/system/game_memory.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(heap_growth, true, "CrashMoM",
                    "Launch setting: a full game heap grows into the port's own memory pool instead of failing "
                    "(native renderer only; src/system/game_memory.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(debug_heap_force_pool, "", "CrashMoM",
                      "Test: heap ids (e.g. 7,8) whose every allocation comes from the port's pool "
                      "(src/system/game_memory.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(debug_physical_callers, false, "CrashMoM",
                    "Research: count every XPhysicalAlloc by calling game function (bytes, live blocks); "
                    "console `memory physical` (src/system/game_memory.cpp)");

extern "C" REX_FUNC(__imp__sub_82357758);  // Radical alloc: r3 heap id, r4 bytes -> r3 address or 0
extern "C" REX_FUNC(__imp__sub_82300E98);  // XPhysicalAlloc: r3 bytes, r4 address (-1 any), r5 align, r6 protect
extern "C" REX_FUNC(__imp__sub_82303410);  // XAPI heap alloc (RtlAllocateHeap): r3 heap, r4 flags, r5 bytes

// --debug_physical_callers: who holds the physical memory (step 3 research).
// Callers are named by the game function two levels up when the direct one
// is the XMemAlloc helper (sub_823005A8: everything D3D allocates).
#include <map>
#include <mutex>
#include <unordered_map>
namespace game_memory {
namespace {
struct CallerStats {
  uint64_t bytes_live = 0, bytes_total = 0;
  uint32_t live = 0, total = 0;
  std::map<uint32_t, uint32_t> protects;
};
std::mutex g_phys_lock;
std::map<std::string, CallerStats> g_phys_by_caller;           // callers -> stats
std::unordered_map<uint32_t, std::pair<uint32_t, std::string>> g_phys_live;  // address -> (bytes, callers)
}  // namespace
void NotePhysical(uint32_t address, uint32_t bytes, uint32_t lr, uint32_t protect, uint32_t r1) {
  // Named by the direct caller + two more up the guest stack (D3D's
  // XMemAlloc helper sub_823005A8 is the direct caller of nearly all).
  uint32_t up[2] = {0, 0};
  const int n = guest_stack::Callers(r1, up, 2);
  std::string key = memory_survey::CallerName(lr);
  for (int i = 0; i < n; ++i) key += " < " + memory_survey::CallerName(up[i]);
  std::lock_guard<std::mutex> l(g_phys_lock);
  auto& c = g_phys_by_caller[key];
  c.bytes_live += bytes; c.bytes_total += bytes; ++c.live; ++c.total; ++c.protects[protect];
  g_phys_live[address] = {bytes, key};
}
void NotePhysicalFree(uint32_t address) {
  std::lock_guard<std::mutex> l(g_phys_lock);
  const auto it = g_phys_live.find(address);
  if (it == g_phys_live.end()) return;
  auto& c = g_phys_by_caller[it->second.second];
  c.bytes_live -= it->second.first; --c.live;
  g_phys_live.erase(it);
}
std::string PhysicalReport() {
  std::lock_guard<std::mutex> l(g_phys_lock);
  std::vector<std::pair<uint64_t, std::string>> order;
  for (const auto& [key, c] : g_phys_by_caller) order.push_back({c.bytes_live, key});
  std::sort(order.rbegin(), order.rend());
  std::string out = "live MB | blocks live/total | total MB | caller | protect flags\n";
  for (const auto& [bytes, key] : order) {
    const auto& c = g_phys_by_caller[key];
    std::string prot;
    for (const auto& [p, n] : c.protects) prot += fmt::format(" {:08X}x{}", p, n);
    out += fmt::format("{:8.1f} | {}/{} | {:.1f} | {} |{}\n", bytes / 1048576.0, c.live, c.total,
                       c.bytes_total / 1048576.0, key, prot);
  }
  return out;
}
}  // namespace game_memory

namespace {

// The return address of sub_8235D9D8's one XPhysicalAlloc call (the main
// heap's block; `bl` at 0x8235D9F8).
constexpr uint32_t kMainHeapCall = 0x8235D9FC;

// The main heap's block from the 0x40000000 range (64 KB pages, committed =
// zero-filled like the physical block). 0 = none (then physical as before).
uint32_t VirtualBlock(uint32_t bytes) {
  auto* heap = rex::system::kernel_memory()->LookupHeap(0x40000000);
  uint32_t address = 0;
  if (!heap || !heap->Alloc(bytes, 64 * 1024,
                            rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  return address;
}

}  // namespace

// Radical's heap alloc (every `new`): r3 heap id, r4 bytes.
extern "C" REX_FUNC(sub_82357758) {
  const uint32_t heap = ctx.r3.u32, bytes = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82357758(ctx, base);
  if (ctx.r3.u32 == 0 && bytes != 0 && memory_survey::NoteRefused(false) < 50) {
    REXLOG_WARN("Memory: heap {} refused {} bytes for {}", heap, bytes, memory_survey::CallerName(lr));
  }
}

// XPhysicalAlloc (textures, buffers, anything GPU-visible; and the main
// heap's block at boot): r3 bytes.
extern "C" REX_FUNC(sub_82300E98) {
  const uint32_t bytes = ctx.r3.u32, align = ctx.r5.u32, protect = ctx.r6.u32, lr = uint32_t(ctx.lr);
  const uint32_t r1 = ctx.r1.u32;
  if (lr == kMainHeapCall) {
    if (REXCVAR_GET(main_heap_virtual) && command_ring::Wanted()) {
      if (const uint32_t address = VirtualBlock(bytes)) {
        REXLOG_INFO("Memory: the game's main heap ({} MB) in guest virtual memory at {:08X}", bytes >> 20, address);
        ctx.r3.u64 = address;
        return;
      }
      REXLOG_WARN("Memory: no virtual block of {} MB for the game's main heap: physical memory as on the console",
                  bytes >> 20);
    } else {
      REXLOG_INFO("Memory: the game's main heap ({} MB) in physical memory ({})", bytes >> 20,
                  REXCVAR_GET(main_heap_virtual) ? "the emulated GPU is on" : "main_heap_virtual off");
    }
  }
  __imp__sub_82300E98(ctx, base);
  if (ctx.r3.u32 != 0 && REXCVAR_GET(debug_physical_callers)) game_memory::NotePhysical(ctx.r3.u32, bytes, lr, protect, r1);
  if (ctx.r3.u32 != 0 && bytes >= (8u << 20)) {  // the few big blocks (heaps, rings): where they went
    REXLOG_INFO("Memory: physical block of {} MB at {:08X} for {}", bytes >> 20, ctx.r3.u32,
                memory_survey::CallerName(lr));
  }
  if (ctx.r3.u32 == 0 && memory_survey::NoteRefused(true) < 50) {
    REXLOG_WARN("Memory: physical allocation of {} bytes (align {:X}, protect {:08X}) refused ({}) for {}", bytes,
                align, protect, memory_survey::PhysicalFreeText(), memory_survey::CallerName(lr));
  }
}

// XAPI's own heap (D3D's small objects: texture headers etc., via XMemAlloc
// without the physical bit): r3 heap, r4 flags, r5 bytes.
extern "C" REX_FUNC(sub_82303410) {
  const uint32_t heap = ctx.r3.u32, bytes = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82303410(ctx, base);
  if (ctx.r3.u32 == 0 && bytes != 0 && memory_survey::NoteRefused(false) < 50) {
    REXLOG_WARN("Memory: XAPI heap {:08X} refused {} bytes ({}) for {}", heap, bytes,
                memory_survey::PhysicalFreeText(), memory_survey::CallerName(lr));
  }
}

// =============================================================================
// STEP 2: full heaps grow into the port's pool (see the top of this file).
// =============================================================================
extern "C" REX_FUNC(__imp__sub_82360F88);  // MemoryDlAllocator::Alloc(this, bytes)
extern "C" REX_FUNC(__imp__sub_82360FF8);  // MemoryDlAllocator::AllocAligned(this, bytes, align)
extern "C" REX_FUNC(__imp__sub_82360430);  // MemoryDlAllocator::Free(this, p)
extern "C" REX_FUNC(__imp__sub_82360378);  // MemoryDlAllocator::Free (slot +0x1C)(this, p)
extern "C" REX_FUNC(__imp__sub_823600E8);  // MemoryDlAllocator::Contains(this, p)
extern "C" REX_FUNC(__imp__sub_823601B0);  // MemoryDlAllocator deleting destructor(this, flags)
extern "C" REX_FUNC(__imp__sub_82357598);  // delete(p): heap from the address's region
extern "C" REX_FUNC(__imp__sub_82357540);  // free(p) through slot +0x1C, heap from the region

namespace {

constexpr uint32_t kHeapTable = 0x8256F1D0;            // 16 bytes per heap id, +0 the allocator object
constexpr uint32_t kMemoryDlAllocator = 0x8200B3F4;    // vtable: Radical's dlmalloc heap
constexpr uint32_t kCDLMemoryAllocator = 0x8203C4B4;   // vtable: a locked wrapper, the dlmalloc heap at +4
constexpr int kHeapIds = 32;

uint32_t Read32(uint32_t a) {
  const uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Step 2 on: the setting, and no emulated GPU (pool memory has no physical
// address). Decided once (both are launch settings).
bool Growing() {
  static const bool on = REXCVAR_GET(heap_growth) && command_ring::Wanted();
  return on;
}

// The game heap id whose allocator is (or wraps) the dlmalloc heap `dl`;
// -1 = not a game heap. The lowest id wins (the root heap fills ids 2, 5,
// 10-15).
int HeapIdOf(uint32_t dl) {
  for (int i = 0; i < kHeapIds; ++i) {
    const uint32_t object = Read32(kHeapTable + 16 * uint32_t(i));
    if (object < 0x40000000) continue;
    const uint32_t vtable = Read32(object);
    if ((vtable == kMemoryDlAllocator && object == dl) ||
        (vtable == kCDLMemoryAllocator && Read32(object + 4) == dl)) {
      return i;
    }
  }
  return -1;
}

const char* HeapName(int id) {
  switch (id) {  // the game's heap descriptor table 0x82506D10 (+ the two it doesn't list)
    case 0: return "main";
    case 1: return "Temp";
    case 2: return "MainMemory root";
    case 3: return "Permanent";
    case 4: return "Script";
    case 6: return "Sound";
    case 7: return "FrontEnd";
    case 8: return "Level";
    default: return "?";
  }
}

// --debug_heap_force_pool: heap ids whose every allocation goes to the pool.
bool Forced(int id) {
  static const std::set<int> ids = [] {
    std::set<int> out;
    const std::string list = REXCVAR_GET(debug_heap_force_pool);
    for (size_t i = 0; i < list.size();) {
      char* end = nullptr;
      const long v = std::strtol(list.c_str() + i, &end, 10);
      if (end == list.c_str() + i) { ++i; continue; }
      out.insert(int(v));
      i = size_t(end - list.c_str());
    }
    return out;
  }();
  return !ids.empty() && id >= 0 && ids.count(id);
}
bool AnyForced() { return !REXCVAR_GET(debug_heap_force_pool).empty(); }

// The request in the pool, for game heap `dl` (logged the first time each
// heap id grows). 0 = not a game heap (the original's 0 stands).
uint32_t FromPool(uint32_t dl, int id, uint32_t bytes, uint32_t align, uint32_t lr) {
  if (id < 0) return 0;
  const uint32_t block = guest_pool::Alloc(bytes, align, dl);
  static std::atomic<uint32_t> told{0};
  const uint32_t bit = 1u << (id & 31);
  if (!(told.fetch_or(bit) & bit)) {
    REXLOG_INFO("Memory: heap {} ({}) is {}: it grows into the port's pool (first block {} bytes at {:08X}, for {})",
                id, HeapName(id), Forced(id) ? "sent to the pool (--debug_heap_force_pool)" : "full", bytes, block,
                memory_survey::CallerName(lr));
  }
  return block;
}

}  // namespace

extern "C" REX_FUNC(sub_82360F88) {
  const uint32_t dl = ctx.r3.u32, bytes = ctx.r4.u32, lr = uint32_t(ctx.lr);
  if (Growing() && AnyForced()) {
    const int id = HeapIdOf(dl);
    if (Forced(id)) {
      ctx.r3.u64 = FromPool(dl, id, bytes, 16, lr);
      return;
    }
  }
  __imp__sub_82360F88(ctx, base);
  if (ctx.r3.u32 == 0 && bytes && Growing()) {
    if (const uint32_t block = FromPool(dl, HeapIdOf(dl), bytes, 16, lr)) ctx.r3.u64 = block;
  }
}

extern "C" REX_FUNC(sub_82360FF8) {
  const uint32_t dl = ctx.r3.u32, bytes = ctx.r4.u32, align = ctx.r5.u32, lr = uint32_t(ctx.lr);
  if (Growing() && AnyForced()) {
    const int id = HeapIdOf(dl);
    if (Forced(id)) {
      ctx.r3.u64 = FromPool(dl, id, bytes, align, lr);
      return;
    }
  }
  __imp__sub_82360FF8(ctx, base);
  if (ctx.r3.u32 == 0 && bytes && Growing()) {
    if (const uint32_t block = FromPool(dl, HeapIdOf(dl), bytes, align, lr)) ctx.r3.u64 = block;
  }
}

extern "C" REX_FUNC(sub_82360430) {
  if (Growing() && guest_pool::Free(ctx.r4.u32)) return;
  __imp__sub_82360430(ctx, base);
}

extern "C" REX_FUNC(sub_82360378) {
  if (Growing() && guest_pool::Free(ctx.r4.u32)) return;
  __imp__sub_82360378(ctx, base);
}

extern "C" REX_FUNC(sub_823600E8) {
  if (Growing() && ctx.r4.u32 && guest_pool::OwnerOf(ctx.r4.u32) == ctx.r3.u32) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_823600E8(ctx, base);
}

extern "C" REX_FUNC(sub_823601B0) {
  if (Growing()) {
    const uint32_t dl = ctx.r3.u32;
    if (const uint32_t n = guest_pool::FreeAllOf(dl)) {
      REXLOG_INFO("Memory: heap object {:08X} destroyed: its {} blocks in the port's pool freed too", dl, n);
    }
  }
  __imp__sub_823601B0(ctx, base);
}

extern "C" REX_FUNC(sub_82357598) {
  if (Growing() && guest_pool::Free(ctx.r3.u32)) return;
  __imp__sub_82357598(ctx, base);
}

extern "C" REX_FUNC(sub_82357540) {
  if (Growing() && guest_pool::Free(ctx.r3.u32)) return;
  __imp__sub_82357540(ctx, base);
}

// Mid-function hook at 0x8227E92C (`new`, heap 3 branch: `cmplwi cr6,r10,0`
// on s_LevelHeapAllocated just loaded): 0 = no assert, the permanent heap
// takes the allocation (see the top of this file).
void CrashMomPermanentAfterLevel(PPCRegister& r10) {
  if (!Growing() || r10.u32 == 0) return;
  static std::atomic<int> told{0};
  if (told.fetch_add(1) < 5) {
    REXLOG_INFO("Memory: a permanent-heap allocation after the level heap: allowed (the original asserted)");
  }
  r10.u64 = 0;
}

// XPhysicalFree (r3 = address): only for --debug_physical_callers' books.
extern "C" REX_FUNC(__imp__sub_82300F20);
extern "C" REX_FUNC(sub_82300F20) {
  if (REXCVAR_GET(debug_physical_callers)) game_memory::NotePhysicalFree(ctx.r3.u32);
  __imp__sub_82300F20(ctx, base);
}
