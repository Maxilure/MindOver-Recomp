// =============================================================================
// debug/guest_stack.cpp -- see guest_stack.h
// =============================================================================
#include "debug/guest_stack.h"

#include <algorithm>
#include <atomic>

#include <rex/ppc/func.h>

#include "guest_memory.h"

#if defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace guest_stack {
namespace {
std::atomic<uint8_t*> g_membase{nullptr};

// The game's code (0x820B0000-0x824D0000, docs/findings/01): return addresses outside it are
// garbage in a frame, not a caller.
constexpr uint32_t kCodeStart = 0x820B0000;
constexpr uint32_t kCodeEnd = 0x824D0000;
}  // namespace

void SetMembase(uint8_t* base) { g_membase = base; }

bool SafeRead(uint32_t guest_address, void* out, size_t n) {
#if defined(__linux__)
  uint8_t* base = g_membase.load();
  if (!base || guest_address < 0x1000 || guest_address + n < guest_address) return false;
  iovec local{out, n};
  iovec remote{GuestPtr(base, guest_address), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
#else
  (void)guest_address, (void)out, (void)n;
  return false;
#endif
}

bool SafeRead32(uint32_t guest_address, uint32_t* value) {
  uint8_t b[4];
  if (!SafeRead(guest_address, b, 4)) return false;
  *value = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
  return true;
}

int Callers(uint32_t r1, uint32_t* out, int max) {
  int n = 0;
  uint32_t sp = r1;
  // 64 frames at most, and a chain must go UP the stack (frames below are
  // newer): anything else is a broken chain, stop there.
  for (int frame = 0; frame < 64 && n < max; ++frame) {
    uint32_t chain = 0, lr = 0;
    if (!SafeRead32(sp, &chain) || chain <= sp || chain - sp > 0x100000) break;
    if (SafeRead32(chain - 8, &lr) && lr >= kCodeStart && lr < kCodeEnd) out[n++] = lr;
    sp = chain;
  }
  return n;
}

uint32_t FunctionContaining(uint32_t guest_address) {
  // The generated table (crash_mom_init.cpp) is sorted by guest address and
  // ends with { 0, nullptr }.
  static const size_t count = [] {
    size_t n = 0;
    while (PPCFuncMappings[n].host) ++n;
    return n;
  }();
  const PPCFuncMapping* begin = PPCFuncMappings;
  const PPCFuncMapping* it = std::upper_bound(
      begin, begin + count, guest_address,
      [](uint32_t a, const PPCFuncMapping& m) { return a < m.guest; });
  if (it == begin || guest_address < kCodeStart || guest_address >= kCodeEnd) return 0;
  return uint32_t((it - 1)->guest);
}

}  // namespace guest_stack
