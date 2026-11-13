// =============================================================================
// world/zone_lists.cpp -- every character in a zone gets drawn, however many
// =============================================================================
// THE BUG (2026-11-12; first seen as "enemies with only their coconut showing"
// after spawning many with the cheat menu, in a player's report): with more
// than a handful of characters in one area, the extra ones were invisible.
// Their props (a Chiratta's coconut is its own object) still showed. The same
// in both pictures and with the original skinning, so it's the game itself.
//
// HOW THE GAME DRAWS AN AREA. The level is cut into ZONES (the zone manager at
// [[0x8259B190]+100], its zones at +20..+24). Each zone keeps lists of the
// render objects inside it (sub_822F1CF8 = "add render object r4 to zone r3",
// called when a render behaviour sets up, e.g. sub_82185E60):
//     +0x4C  list 0     objects of kind (+0x10 >> 16) != 0
//     +0x5C  list 1     objects of kind 0
//     +0x6C  list 2     objects with flag bit 2 (+0x10): characters
//     +0x7C  list 3     objects with flag 0x1000, in addition
//     (+0x8C: another list the add's other branch handles, not touched here)
// each list = begin, end, capacity end: arrays the zone allocates with the
// game's `new` (sub_8227E9C8; list 2 gets 128 bytes = 32 entries at
// 0x822EE5xx, list 3 1024 bytes) and frees with the zone. Every frame the
// render manager walks the zones (sub_822B4B08 -> sub_822F4CE0 ->
// sub_822F4760 per list) and fades each object in or out by its distance from
// the camera versus its own limit (+0x16C); the fade goes to render object
// +0x170, and the object draw (sub_822B6FB8) skips anything whose fade is 0.
//
// THE LIMIT: the add only pushes while end < capacity (0x822F1DD8 / 0x822F1E10:
// cmplw r11,r9; bge = NOT added, without a word). A character that didn't fit
// is in no list: its fade never rises from 0, it's never drawn. List 2 has
// room for 32 objects, so about 16 enemies fit in a zone next to Crash, the
// partners and the props. Measured on Wumpa Island: 50 Chirattas spawned one
// by one, the zone's list 2 full at 32, the 16 listed ones drawn (fade 1), the
// 5 newest checked ones in no list (fade 0). Found with the console: a
// temporary skin-draw counter (flat at ~35 per frame past 30 enemies), `who`
// on a drawn body's +0x170 (writer sub_822F4760), then every zone's lists.
// (players/outfit_previews.cpp met the same limit on the chest's zone in
// 2026-10 and swapped in its own storage while previews are up; with growth on
// it now leaves that to this file.)
//
// THE FIX: sub_822F1CF8 wrapped. Before the original runs, any of the zone's
// lists 0-3 that is FULL gets a bigger array from the game's own `new`
// (double, at least 64 entries; entries copied), so the push that follows
// lands. The zone frees it with the game's `delete` (sub_8227E9D0 finds the
// heap from the address) like its own. A FIRST VERSION used our own memory
// (SystemHeapAlloc, in two mid-function hooks): the zone's teardown freed it
// with the game's `delete` = heap damage, and loading a save after "Quit Game"
// crashed in sub_822E6AA0 (a zone's neighbour list read as 0). The old array
// is not freed here: the add can run while a walk over the same list is still
// using it, so it's left (a few hundred bytes per growth, until the level ends).
//
// THE SECOND LIMIT: D3D's RING BUFFER. Every skinned body (and other inline
// geometry) is written into D3D's secondary ring buffer (BeginIndexedVertices
// sub_82477248 -> allocator sub_82308B60 -> sub_82308960), and a frame drawn
// in tiles must fit whole, without wrapping. The game asks for 6 MB
// (0x82433BE8). With the lists grown, 64 Chirattas dying at once overflowed
// it: the allocator returned 0, BeginIndexedVertices gave back a NULL index
// pointer (E_OUTOFMEMORY) and the game copied indices to it: crash in
// sub_823F2DC0 (memcpy to 0xFFFFFFF8), twice in tests. The original never got
// there because the 32-slot lists capped what was drawn. With growth on the
// ring is kRingSize (24 MB = 4x; the spawn cheat's handle-table check stops
// near 200 enemies). Cost: 18 MB more of the guest's physical memory.
//
// THE RELOAD CRASH (found while testing the above, 2026-11-12; it happens
// with --zone_list_growth=false too): after hundreds of enemies were killed
// (their mojo pickups everywhere) and "Quit Game", loading a save could crash
// while the new level was built (front end 486 -> 488). CCollisionManager
// (slot 10) still updated a Collectable (slot 5 sub_821984A0); its position
// update sub_8217E850 asks "which zone is this point in" =
// sub_822E6AA0(zone manager, point, the actor's zone = actor +40), and that
// zone was the OLD level's, already freed: its neighbour list (+0x198/+0x19C)
// and volume lists read as garbage (reads of 0, 0x10, 0x17C, 0xBF). 6 of 6
// heavy runs with growth on, 1 of 3 with it off. Null checks only moved the
// crash (stale, not null). FIX: sub_822E6AA0 rewritten (it's 48 instructions,
// Ghidra + xexdis 0x822E6AA0-0x822E6B60) with one rule added: a zone is used
// only if it's in the manager's CURRENT list (+0x14..+0x18); the actor's own
// zone and its neighbours are skipped otherwise, and the search goes on over
// the current zones like the original's fallback.
//
// --zone_list_growth=false = the original limits (A/B tests; read at start
// for the ring).
// =============================================================================

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(zone_list_growth, true, "CrashMoM",
                    "Every character in an area is drawn, however many: the game's zones hold 32 "
                    "characters each and left the rest invisible (src/world/zone_lists.cpp)");

extern "C" REX_FUNC(__imp__sub_822F1CF8);  // zone r3: add render object r4 to its lists
extern "C" REX_FUNC(__imp__sub_8227E9C8);  // the game's `new` (r3 = bytes; current heap)
extern "C" REX_FUNC(__imp__sub_82357128);  // the heap REGION an address lies in (index, -1 = none)

namespace {

constexpr uint32_t kLists[] = {0x4C, 0x5C, 0x6C, 0x7C};  // zone: list headers grown when full
constexpr uint32_t kMinEntries = 64;
constexpr uint32_t kRingSize = 24u << 20;  // D3D secondary ring (the game asks for 6 MB)
// The game's heaps (sub_82357598 = `delete`, sub_823574E8 = "use this heap"):
constexpr uint32_t kRegionHeaps = 0x8256D1B4;  // region i's heap id at +8*i
constexpr uint32_t kCurrentHeap = 0x825711EC;  // the heap `new` takes from

uint8_t* Guest(uint32_t address) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(address); }
uint32_t Read32(uint32_t address) {
  const uint8_t* p = Guest(address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t address, uint32_t value) {
  uint8_t* p = Guest(address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}
// A guest address that is the game's memory (not null, not a stray number).
bool Plausible(uint32_t address) { return address >= 0x40000000; }

// The list at `list` (begin, end, capacity end) is full: a bigger array from
// the game's `new`, entries copied, the list's three pointers moved to it.
void Grow(PPCContext& ctx, uint8_t* base, uint32_t zone, uint32_t offset) {
  const uint32_t list = zone + offset;
  const uint32_t begin = Read32(list), end = Read32(list + 4), cap = Read32(list + 8);
  if (!Plausible(begin) || end < begin || cap < end) return;  // empty or not a list we understand
  const uint32_t used = (end - begin) / 4;
  const uint32_t room = std::max(kMinEntries, 2 * ((cap - begin) / 4));
  // In the SAME heap as the zone's own array: the zone was made under a
  // level heap, while an add can run with another one current (a first try
  // with the current heap broke the next level load after "Quit Game" the
  // same way our own memory did; see the header).
  const PPCContext saved = ctx;
  ctx.r3.u64 = begin;
  __imp__sub_82357128(ctx, base);
  const int32_t region = int32_t(ctx.r3.u32);
  const uint32_t current = Read32(kCurrentHeap);
  const uint32_t heap = region >= 0 ? Read32(kRegionHeaps + 8 * uint32_t(region)) : current;
  Write32(kCurrentHeap, heap);
  ctx = saved;
  ctx.r3.u64 = room * 4;
  __imp__sub_8227E9C8(ctx, base);
  const uint32_t array = ctx.r3.u32;
  ctx = saved;
  Write32(kCurrentHeap, current);
  if (!Plausible(array)) return;  // no memory: the game's own "dropped" behaviour stays
  std::copy(Guest(begin), Guest(begin) + used * 4, Guest(array));
  Write32(list, array);
  Write32(list + 4, array + used * 4);
  Write32(list + 8, array + room * 4);
  REXLOG_INFO("Zone lists: zone {:08X} list +{:02X} was full ({} objects), now has room for {} (heap {}, current {})",
              zone, offset, used, room, heap, current);
}

}  // namespace

extern "C" REX_FUNC(sub_822F1CF8) {
  const uint32_t zone = ctx.r3.u32;
  if (REXCVAR_GET(zone_list_growth) && Plausible(zone)) {
    for (const uint32_t offset : kLists) {
      const uint32_t list = zone + offset;
      if (Plausible(Read32(list)) && Read32(list + 4) >= Read32(list + 8)) Grow(ctx, base, zone, offset);
    }
  }
  __imp__sub_822F1CF8(ctx, base);
}

// "Is point r4 in zone r3?" (volumes +0x178..+0x17C) and the manager's zones.
extern "C" REX_FUNC(__imp__sub_822EF2C0);
namespace {
bool ZoneHas(PPCContext& ctx, uint8_t* base, uint32_t zone, uint32_t point) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = zone;
  ctx.r4.u64 = point;
  __imp__sub_822EF2C0(ctx, base);
  const bool inside = (ctx.r3.u32 & 0xFF) != 0;
  ctx = saved;
  return inside;
}
}  // namespace

// sub_822E6AA0(manager r3, point r4, hint zone r5) -> the zone holding the
// point, or 0. Original order: the hint zone, its neighbours (+0x198..+0x19C),
// then every zone of the manager (+0x14..+0x18); first hit wins. Added: the
// hint and its neighbours only when they are CURRENT zones (see the header).
extern "C" REX_FUNC(sub_822E6AA0) {
  const uint32_t manager = ctx.r3.u32, point = ctx.r4.u32, hint = ctx.r5.u32;
  const uint32_t first = Read32(manager + 0x14), last = Read32(manager + 0x18);
  const bool list_ok = Plausible(first) && last >= first && (last - first) / 4 < 4096;
  auto current = [&](uint32_t zone) {
    if (!Plausible(zone) || !list_ok) return false;
    for (uint32_t a = first; a < last; a += 4)
      if (Read32(a) == zone) return true;
    return false;
  };
  auto answer = [&](uint32_t zone) { ctx.r3.u64 = zone; };
  if (hint) {
    if (current(hint)) {
      if (ZoneHas(ctx, base, hint, point)) return answer(hint);
      const uint32_t nb = Read32(hint + 0x198), ne = Read32(hint + 0x19C);
      for (uint32_t a = nb; Plausible(nb) && a < ne && a - nb < 4 * 4096; a += 4) {
        const uint32_t zone = Read32(a);
        if (current(zone) && ZoneHas(ctx, base, zone, point)) return answer(zone);
      }
    } else {
      static int logged = 0;
      if (logged < 5 && ++logged)
        REXLOG_WARN("Zone lists: an object still points at zone {:08X}, not one of this level's: searched them all",
                    hint);
    }
  }
  if (list_ok) {
    for (uint32_t a = first; a < last; a += 4) {
      const uint32_t zone = Read32(a);
      if (Plausible(zone) && ZoneHas(ctx, base, zone, point)) return answer(zone);
    }
  }
  answer(0);
}

// Codegen: D3D device set-up, r5 = RingBufferParameters.SecondarySize (bytes)
// ([[entrypoint.midasm_hook]] at 0x82433BEC in the manifest).
void D3DRingSecondarySize(PPCRegister& r5) {
  if (!REXCVAR_GET(zone_list_growth) || r5.u32 >= kRingSize) return;
  REXLOG_INFO("Zone lists: D3D ring buffer {} MB -> {} MB (room for every character drawn)", r5.u32 >> 20,
              kRingSize >> 20);
  r5.u64 = kRingSize;
}
