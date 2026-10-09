// =============================================================================
// debug/write_watchpoints.h -- "who wrote this value?" (debug console `who`)
// =============================================================================
//
// WHY
//   The most common question while hunting a bug: SOME code changes a value
//   (a player's state, a camera distance, a flag) and we don't know which.
//   Before this, the answer meant a gdb session: slow boot under the debugger,
//   hardware watchpoints set by hand, the game paused at every hit. Now the
//   debug console asks the running game:
//       who [p1+320] 4        start watching 4 bytes
//       wait who 20000        until something writes there
//       who                   every writer so far: count, old -> new value,
//                             the PowerPC instruction, the game's call stack
//       who stop              stop watching
//
// HOW (a page trap + one single step)
//   1. Arm: the 4 KB host page(s) holding the range are made READ-ONLY
//      (mprotect). Reads go on as normal.
//   2. A write to such a page faults (SIGSEGV). Our signal handler sits IN
//      FRONT of the SDK's (it takes over SIGSEGV and passes every fault that
//      isn't on one of our pages to the SDK's handler) and checks the address: inside a watched range -> record a hit
//      (old value, host instruction, the guest's LR and call stack from the
//      thread's PPCContext). Either way: make the page writable again and set
//      the CPU's TRAP FLAG (x86 EFLAGS.TF) in the faulting thread's context.
//   3. The write runs again, succeeds, and the trap flag stops the thread
//      right after that ONE instruction (SIGTRAP). Our SIGTRAP handler
//      re-protects the page, records the new value and clears the flag.
//   So every write is seen, not just the first, and the game never pauses
//   longer than two signals (~ microseconds). A page that also holds other
//   busy data costs a fault + trap per write to it: watch with care.
//
//   Hits go into a fixed ring (4,096, newest kept) plus a table of distinct
//   writers (host instruction address -> count); both are plain arrays with
//   atomics, because signal handlers can't allocate or lock. Naming the
//   writer (symbols, the exact PowerPC instruction) happens later, in the
//   console's own thread (host_symbols.h).
//
// GRAPHICS / PHYSICAL MEMORY: pages in a physical heap (0xA0000000+) may also
//   be watched by the emulated GPU and our texture cache. There our handler
//   first lets the SDK run those watchers (Memory::TriggerPhysicalMemory-
//   Callbacks, exactly what the SDK's own handler does) before lifting the
//   protection, so their bookkeeping stays right. Writes through another
//   address window of the same physical page aren't seen.
//
// LIMITS
//   - Linux x86-64 only (signals + EFLAGS.TF). Not under gdb: the debugger
//     takes the SIGTRAPs for itself.
//   - The KERNEL writing into a watched page (a file read straight into the
//     buffer) doesn't fault: the read fails with EFAULT. Don't watch a buffer
//     the game is loading into.
//   - Another thread writing to the page during the single step isn't seen
//     (the page is writable for that one instruction).
//   - At most 8 ranges, 64 pages in all, 64 KB per range.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace write_watchpoints {

// Installs our SIGSEGV handler in front of the SDK's, and the SIGTRAP one.
// Call AFTER the runtime is set up (OnPostSetup). Why not the SDK's handler
// list (rex::arch::ExceptionHandler)? Ours would have to come first in it,
// i.e. be added before guest memory exists, and the first Install() is what
// takes over SIGSEGV. Runtime::Setup installs its SEH emulation's SIGSEGV
// handler (rex::initialize_seh: "not in a __try -> SIG_DFL + raise") right
// before creating memory, so with an early Install that one stayed on top
// and the first normal GPU write-watch fault killed the game (2026-10-09,
// the first test of this file). At the signal level the order is ours.
void Install();

// Starts watching [address, address + size). "" = armed, else the reason it
// can't be (not writable guest memory, too many ranges...).
std::string Arm(uint32_t address, uint32_t size);
// Stops every watch and forgets the hits.
void DisarmAll();

struct Range {
  uint32_t address, size;
};
std::vector<Range> Ranges();

// One recorded write.
struct Hit {
  uint64_t serial = 0;      // 1, 2, 3... in order of the writes
  double ms = 0;            // since the game started (steady clock)
  uint32_t address = 0;    // the guest address written
  uint32_t old_value = 0;   // the aligned 4-byte word there, before (big-endian value)
  uint32_t new_value = 0;   // ... and after (valid when `done`)
  bool done = false;        // the single step finished (new_value valid)
  uint64_t host_pc = 0;     // the host instruction that wrote
  uint32_t thread = 0;      // host thread id (gettid)
  uint32_t guest_lr = 0;    // the guest's LR (0 = not a game thread)
  uint32_t callers[8] = {}; // the guest's call stack (guest_stack.h), innermost first
  int caller_count = 0;
};
// The ring's hits, oldest first.
std::vector<Hit> Hits();

// Every distinct writer since the last DisarmAll: host instruction + count.
struct Writer {
  uint64_t host_pc;
  uint64_t count;
};
std::vector<Writer> Writers();

// All hits recorded so far (the ring keeps only the newest 4,096).
uint64_t TotalHits();

}  // namespace write_watchpoints
