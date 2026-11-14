// =============================================================================
// guest_memory.cpp -- "may our C++ read this guest range?" (see guest_memory.h)
// =============================================================================
//
// guest_memory::Readable() is the ONE check our own code uses before following
// a pointer it read out of a game object (cheats, spawn, the debug console,
// the controllers' guest buffers). A stray read of a page the host has no
// memory behind kills the game: the SDK's fault handler can't recover it.
//
// WHY A PAGE MUST BE COMMITTED, NOT JUST "READABLE" (GitHub issue #25,
// docs/findings/27-cheat-menu.md s.12): the SDK's BaseHeap::QueryRangeAccess
// looks only at each page's PROTECTION flags. A range the game only RESERVED
// (VirtualAlloc-style MEM_RESERVE, no commit) still carries the protection it
// was reserved with (xmemory.cpp AllocFixed: current_protect = protect for
// reserve and commit alike), so QueryRangeAccess answers "read/write" for it.
//   - On Linux the whole 4 GB guest space is one shared-memory mapping, so a
//     read of a reserved-only page just returns old bytes: harmless.
//   - On Windows the host page behind a reserved-only guest page is reserved
//     too (MEM_RESERVE, no backing): reading it = access violation = crash.
// The report: a Windows player in the ice cave switched to the electric
// mutant; the cheat menu's per-frame player snapshot read that titan's
// actor +0x180 (for this titan type NOT an upgrade object pointer: 0x41D26B0D,
// junk), Readable() said yes, Read32 faulted inside ObjectWithVtable.
// So: every page of the range must be COMMITTED and have read access.
// =============================================================================
#include "guest_memory.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace guest_memory {

bool Readable(uint32_t address, uint32_t size) {
  // Low page = null-ish pointers; empty or wrapping ranges are never fine.
  if (address < 0x1000 || size == 0 || address + (size - 1) < address) return false;
  auto* memory = rex::system::kernel_memory();
  if (!memory) return false;
  auto* heap = memory->LookupHeap(address);
  const uint32_t last = address + (size - 1);
  // One heap must hold the whole range (heaps don't overlap; a range across
  // two heaps is a junk pointer anyway).
  if (!heap || memory->LookupHeap(last) != heap) return false;

  // Protection first (the old check: also catches no-access pages).
  if (heap->QueryRangeAccess(address, last) == rex::memory::PageAccess::kNoAccess) return false;

  // Then every page committed. Our ranges are a few hundred bytes = one or
  // two pages, so asking page by page is cheap. QueryRegionInfo reports the
  // state of the page holding `base_address` (reserve / commit bits).
  const uint32_t page_size = heap->page_size();
  for (uint32_t page = address & ~(page_size - 1);; page += page_size) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(page, &info)) return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit)) return false;
    if (last - page < page_size) break;  // this page holds the range's last byte
  }
  return true;
}

}  // namespace guest_memory
