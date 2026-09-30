// =============================================================================
// native/write_watch.cpp -- see write_watch.h for the why and how
// =============================================================================

#include "write_watch.h"

#include <algorithm>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace native {

WriteWatch::WriteWatch() : page_epoch_(new std::atomic<uint64_t>[kPageCount]) {
  for (uint32_t i = 0; i < kPageCount; ++i) {
    page_epoch_[i].store(0, std::memory_order_relaxed);
  }
  if (auto* memory = rex::system::kernel_memory()) {
    handle_ = memory->RegisterPhysicalMemoryInvalidationCallback(Callback, this);
  }
}

WriteWatch::~WriteWatch() {
  auto* memory = rex::system::kernel_memory();
  if (memory && handle_) {
    memory->UnregisterPhysicalMemoryInvalidationCallback(handle_);
  }
}

bool WriteWatch::Watchable(uint32_t address, uint32_t size) const {
  auto* memory = rex::system::kernel_memory();
  if (!memory || !handle_ || !size) {
    return false;
  }
  const uint32_t physical = memory->GetPhysicalAddress(address);
  if (physical == UINT32_MAX || uint64_t(physical) + size > 0x20000000u) {
    return false;  // not graphics (physical) memory: the SDK can't watch it
  }
  // Each page: writable in the window it's allocated in, read-only nowhere.
  // A physical page can be allocated through any of the three windows
  // (guest.h, PhysicalAddress), so all three are asked; the 0xE0000000 one
  // is 4 KB off. Read-only pages are never protected by the SDK (a write
  // there must stay a real fault), so they'd be written unseen if the game
  // later made them writable: hash those instead.
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = (physical + size - 1) >> kPageShift;
  for (uint32_t page = first; page <= last; ++page) {
    const uint32_t p = page << kPageShift;
    const uint32_t windows[3] = {0xA0000000u + p, 0xC0000000u + p,
                                 p >= 0x1000u ? 0xE0000000u + p - 0x1000u : 0};
    bool writable = false;
    for (uint32_t window : windows) {
      if (!window) {
        continue;
      }
      auto* heap = memory->LookupHeap(window);
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(window, &protect) || !protect) {
        continue;  // not allocated through this window
      }
      if (!(protect & rex::memory::kMemoryProtectWrite)) {
        return false;  // read-only somewhere
      }
      writable = true;
    }
    if (!writable) {
      return false;
    }
  }
  return true;
}

uint64_t WriteWatch::Arm(uint32_t address, uint32_t size) {
  auto* memory = rex::system::kernel_memory();
  // The ticket first, then the protection (write_watch.h: ordering).
  const uint64_t ticket = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (memory) {
    memory->EnablePhysicalMemoryAccessCallbacks(memory->GetPhysicalAddress(address), size,
                                                true, false);
  }
  return ticket;
}

bool WriteWatch::Written(uint32_t address, uint32_t size, uint64_t ticket) const {
  auto* memory = rex::system::kernel_memory();
  const uint32_t physical = memory ? memory->GetPhysicalAddress(address) : UINT32_MAX;
  if (physical == UINT32_MAX || !size) {
    return true;  // can't tell: say "written", the caller then hashes
  }
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = std::min((physical + size - 1) >> kPageShift, kPageCount - 1);
  for (uint32_t page = first; page <= last; ++page) {
    if (page_epoch_[page].load(std::memory_order_acquire) > ticket) {
      return true;
    }
  }
  return false;
}

void WriteWatch::MarkWritten(uint32_t physical_start, uint32_t length) {
  if (!length || physical_start >= 0x20000000u) {
    return;
  }
  const uint64_t now = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint32_t first = physical_start >> kPageShift;
  const uint32_t last =
      std::min(uint32_t((uint64_t(physical_start) + length - 1) >> kPageShift), kPageCount - 1);
  for (uint32_t page = first; page <= last; ++page) {
    page_epoch_[page].store(now, std::memory_order_release);
  }
}

std::pair<uint32_t, uint32_t> WriteWatch::Callback(void* context, uint32_t physical_start,
                                                   uint32_t length, bool exact_range) {
  // Called by the SDK with its global memory lock held, on whichever thread
  // wrote (or freed / re-protected) the memory.
  auto* self = static_cast<WriteWatch*>(context);
  if (exact_range) {
    // Only exactly this range loses its protection.
    self->MarkWritten(physical_start, length);
    return {0, UINT32_MAX};  // ignored for exact ranges
  }
  // The SDK may lift the protection of MORE pages than were written (fewer
  // faults for a big sequential write), limited to what every listener
  // allows. Any page it unprotects stops reporting to us, so each one we
  // allow must count as written. 64 KB blocks: a texture being rewritten
  // faults once per 64 KB instead of once per page, and a write marks at
  // most 15 innocent neighbour pages (their textures just get re-hashed).
  constexpr uint32_t kBlock = 64 * 1024;
  const uint32_t start = physical_start & ~(kBlock - 1);
  const uint64_t end =
      (uint64_t(physical_start) + std::max(length, 1u) + kBlock - 1) & ~uint64_t(kBlock - 1);
  const uint32_t block_length = uint32_t(std::min<uint64_t>(end, 0x20000000u) - start);
  self->MarkWritten(start, block_length);
  return {start, block_length};
}

}  // namespace native
