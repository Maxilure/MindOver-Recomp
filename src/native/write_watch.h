// =============================================================================
// native/write_watch.h -- "has the game written to this memory since I looked?"
// =============================================================================
//
// WHY
//   Our texture cache must notice when the game changes a texture's pixels
//   (palettes, movie frames, textures reloaded into reused memory). Until
//   2026-09-29 it re-hashed EVERY texture a frame used, every frame: ~3 ms of
//   the game's main thread per frame in busy scenes (a playtest:
//   "textures 2.83 ms" of a 3.7 ms renderer cost), enough to push frames
//   past 16.7 ms and drop a big fight to ~50 fps (docs/findings/20).
//   Almost all of those textures never change after loading.
//
// HOW (the trick the emulated GPU already uses)
//   The SDK can write-protect pages of guest PHYSICAL memory (where all
//   graphics data lives). When anything then writes to such a page (the
//   game's own code, a file read, a free + re-allocation), the write faults,
//   the SDK's fault handler calls every registered "invalidation callback"
//   with the page range, lifts the protection and lets the write go on.
//   (xmemory.h: RegisterPhysicalMemoryInvalidationCallback,
//   EnablePhysicalMemoryAccessCallbacks; the emulated GPU's SharedMemory is
//   the other listener.) One fault per page per watch: cheap for memory that
//   rarely changes.
//
//   We keep a WRITE EPOCH per 4 KB page: a counter value, bumped by our
//   callback when the page is written. Arm() protects a range and returns a
//   ticket (the counter's current value); Written(range, ticket) is true if
//   any page's epoch is newer than the ticket. Epochs rather than "dirty
//   bits" because several cache entries can share a page (one entry's check
//   must not clear another's news).
//
//   Ordering that makes it safe: take the ticket, THEN protect, THEN read the
//   data (hash / upload). A write before the protection lands is in the data
//   we read; a write after it faults and bumps the epoch past the ticket.
//
// WHAT IT DOESN'T SEE
//   Writes that don't go through the protected guest mappings: the emulated
//   GPU's own writes (resolves read back into memory, memexport) go through
//   the SDK's raw physical view. Resolve destinations are drawn from our own
//   images (render_targets), so textures never depend on those; but the debug
//   mode --debug_native_emulated_resolves does, and must not rely on watches.
//   Pages the game keeps read-only can't be armed (a write there must stay a
//   real fault); Watchable() says so, and such memory is hashed as before.
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace native {

class WriteWatch {
 public:
  WriteWatch();   // registers our invalidation callback with guest memory
  ~WriteWatch();  // unregisters it (armed pages just fault once more, harmlessly)

  WriteWatch(const WriteWatch&) = delete;
  WriteWatch& operator=(const WriteWatch&) = delete;

  // True if every page of the guest VIRTUAL range [address, address + size)
  // is in physical memory and writable by the game in its mapping (so a
  // write would fault once armed). False: can't be watched, hash instead.
  bool Watchable(uint32_t address, uint32_t size) const;

  // Starts watching the guest VIRTUAL range (must be Watchable) and returns
  // the ticket to pass to Written(). Take it BEFORE reading the data.
  uint64_t Arm(uint32_t address, uint32_t size);

  // Has any page of the range been written since `ticket` was handed out?
  bool Written(uint32_t address, uint32_t size, uint64_t ticket) const;

 private:
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t physical_start,
                                                uint32_t length, bool exact_range);
  // Marks the pages of a physical range as written now.
  void MarkWritten(uint32_t physical_start, uint32_t length);

  static constexpr uint32_t kPageShift = 12;                     // 4 KB pages
  static constexpr uint32_t kPageCount = 0x20000000u >> kPageShift;  // 512 MB

  std::unique_ptr<std::atomic<uint64_t>[]> page_epoch_;  // kPageCount entries, 0 = never
  std::atomic<uint64_t> epoch_{1};                       // last value handed out
  void* handle_ = nullptr;                               // the callback's registration
};

}  // namespace native
