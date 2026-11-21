// =============================================================================
// memory_survey.h -- how full is the game's memory? (the `memory` command)
// =============================================================================
//
// WHY: the port keeps running into the Xbox 360's fixed memory pools (the
// front end's ~100 KB heap, the zone lists, the D3D ring buffer, the whole
// level not fitting in split screen...). Before replacing the memory manager
// (notes/plans.md, "MEMORY: our own memory manager"), this tool says WHICH
// pool is full, and by how much. Findings: docs/findings/71-memory.md.
//
// TWO KINDS OF MEMORY, both reported:
//
//  1. RADICAL'S HEAPS (the game's own `new` / `delete`): up to 16 heap
//     objects in the table 0x8256F1D0 (16 bytes each, the object first;
//     sub_823575F0(i) reads it). The game's own report sub_8227F130(i, &used,
//     &total, &third) asks the heap object (virtual slots +0x30 / +0x34).
//     Which heap an address belongs to: the region table 0x8256D1B0 (8 bytes
//     each: start address, heap id; count 0x825711E4; sub_82357128 searches
//     it). `new` (sub_8227E9C8) takes from the CURRENT heap 0x825711EC
//     (sub_823574E8 swaps it) through sub_82357758(heap, bytes).
//
//  2. THE SDK'S GUEST MEMORY (what the "Xbox" gives the game): the virtual
//     ranges 0x00000000 (4 KB pages), 0x40000000 (64 KB pages; Radical's
//     heaps are carved from here by NtAllocateVirtualMemory), the 0x80000000 /
//     0x90000000 XEX ranges, and the 512 MB of PHYSICAL memory that the three
//     windows 0xA0000000 (64 KB pages) / 0xC0000000 (16 MB) / 0xE0000000
//     (4 KB) all share. Textures, vertex buffers and other GPU data come from
//     the physical memory: D3D asks the game's XMemAlloc (sub_8227A500 =
//     sub_823005A8), which for "physical" requests calls XPhysicalAlloc
//     (sub_82300E98 -> MmAllocatePhysicalMemoryEx). The per-window numbers
//     do NOT show what the other windows took: only the shared physical heap
//     does (Memory::GetPhysicalHeap()).
//
// WHAT YOU GET:
//   - debug console `memory`: the latest report (refreshed every second on the
//     game thread), with each heap's PEAK since the start;
//   - a log line every --debug_memory_log_s seconds (default 60, 0 = off):
//     "Memory: h0 used/total KB ... | physical free N MB (largest M MB) ...";
//   - EVERY refused allocation is logged (system/game_memory.cpp) (heap id, bytes, the calling game
//     function), the first 50 of each kind: "Memory: heap 8 refused 1048576
//     bytes for sub_X+0xNN" / "Memory: physical allocation of N bytes refused
//     (physical free F MB, largest L MB) for sub_X+0xNN".
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, debug_memory_log_s);

namespace memory_survey {

// Registers the per-frame refresh (call once, after the runtime exists).
void Install();

// The latest report (empty before the first game frame). `fresh` asks the
// game thread for a new one first (waits up to ~0.5 s).
std::string Report(bool fresh = true);

// For the allocation wrappers (system/game_memory.cpp): counts a refused
// allocation (returns how many came before), names a guest return address
// ("sub_X+0xNN"), and says how much physical memory is left.
int NoteRefused(bool physical);
std::string CallerName(uint32_t lr);
std::string PhysicalFreeText();

// DEBUG: zone `zone`'s neighbour list = every zone (the whole-level test).
std::string NeighboursAll(uint32_t zone);

}  // namespace memory_survey
