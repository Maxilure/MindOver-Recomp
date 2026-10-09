// =============================================================================
// debug/write_watchpoints.cpp -- see write_watchpoints.h
// =============================================================================
#include "debug/write_watchpoints.h"

#if defined(__linux__) && defined(__x86_64__)

#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "debug/guest_stack.h"
#include "debug/host_symbols.h"

namespace write_watchpoints {
namespace {

constexpr uint64_t kPageSize = 4096;  // x86-64 Linux host pages
constexpr uint64_t kTrapFlag = 0x100; // EFLAGS.TF: trap after the next instruction
constexpr int kMaxRanges = 8;
constexpr int kMaxPages = 64;
constexpr uint32_t kMaxRangeSize = 0x10000;
constexpr size_t kRing = 4096;
constexpr int kMaxWriters = 128;

// --- State the signal handlers read: fixed arrays of atomics only. ---------

// A watched range; size 0 = free slot.
struct RangeSlot {
  std::atomic<uint32_t> address{0};
  std::atomic<uint32_t> size{0};
};
RangeSlot g_ranges[kMaxRanges];

// A host page we have protected at some point. Once listed, a page stays
// listed (until DisarmAll): a fault on it after a disarm (a thread that was
// already on its way) is then still recognised as ours and just let through.
struct PageSlot {
  std::atomic<uint64_t> host{0};  // page start (host address), 0 = free
  std::atomic<bool> armed{false};
  std::atomic<bool> physical{false};  // in a guest physical heap (see the .h)
  std::atomic<uint32_t> guest{0};     // guest address of the page start
};
PageSlot g_pages[kMaxPages];

// The hit ring. `serial` = 0 while a slot is being filled.
struct HitSlot {
  std::atomic<uint64_t> serial{0};
  Hit hit;
  std::atomic<bool> done{false};
};
HitSlot g_ring[kRing];
std::atomic<uint64_t> g_next{0};  // hits handed out so far

struct WriterSlot {
  std::atomic<uint64_t> pc{0};
  std::atomic<uint64_t> count{0};
};
WriterSlot g_writers[kMaxWriters];

uint8_t* g_membase = nullptr;  // guest memory base (set when the first range is armed)
struct sigaction g_old_trap{};
bool g_installed = false;
std::mutex g_mutex;  // Arm / DisarmAll (console threads; never the handlers)

// The single step in flight on this thread (one at a time per thread).
thread_local uint64_t t_page = 0;     // page to protect again, 0 = none
thread_local int64_t t_slot = -1;     // ring slot to finish, -1 = none
thread_local uint32_t t_word = 0;     // guest address of the word to read back

double NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
double g_start_ms = NowMs();

PageSlot* FindPage(uint64_t host_page) {
  for (PageSlot& p : g_pages) {
    if (p.host.load(std::memory_order_acquire) == host_page) return &p;
  }
  return nullptr;
}

bool InRange(uint32_t guest) {
  for (const RangeSlot& r : g_ranges) {
    const uint32_t size = r.size.load(std::memory_order_acquire);
    if (size && guest - r.address.load(std::memory_order_relaxed) < size) return true;
  }
  return false;
}

uint32_t ReadWord(uint32_t guest_address) {
  uint32_t v = 0;
  guest_stack::SafeRead32(guest_address, &v);
  return v;
}

void CountWriter(uint64_t pc) {
  for (WriterSlot& w : g_writers) {
    uint64_t seen = w.pc.load(std::memory_order_acquire);
    if (seen == 0) {
      uint64_t expected = 0;
      if (w.pc.compare_exchange_strong(expected, pc)) seen = pc;
      else seen = expected;
    }
    if (seen == pc) {
      w.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // Table full: the hit is still in the ring and in TotalHits().
}

struct sigaction g_old_segv{};  // the SDK's SIGSEGV handler: everything not ours goes there

// Is this write fault ours? Then let the write through and arm the single
// step; true = handled. Runs inside the SIGSEGV handler.
bool HandleFault(uint64_t host, uint64_t pc, ucontext_t* uc) {
  // x86 page-fault error code bit 1 = a write (reads never fault on our
  // read-only pages).
  if (!(uint64_t(uc->uc_mcontext.gregs[REG_ERR]) & 2)) return false;
  const uint64_t host_page = host & ~(kPageSize - 1);
  PageSlot* page = FindPage(host_page);
  if (!page) return false;  // not ours: the SDK decides

  const uint32_t guest = page->guest.load() + uint32_t(host - host_page);
  int64_t slot_index = -1;
  if (page->armed.load() && InRange(guest)) {
    const uint64_t serial = g_next.fetch_add(1) + 1;
    slot_index = int64_t((serial - 1) % kRing);
    HitSlot& slot = g_ring[slot_index];
    slot.serial.store(0, std::memory_order_release);  // being filled
    slot.done.store(false, std::memory_order_relaxed);
    Hit& h = slot.hit;
    h = Hit{};
    h.serial = serial;
    h.ms = NowMs() - g_start_ms;
    h.address = guest;
    h.old_value = ReadWord(guest & ~3u);
    h.host_pc = pc;
    h.thread = uint32_t(syscall(SYS_gettid));
    if (auto* ts = rex::runtime::ThreadState::Get()) {
      const PPCContext* ctx = ts->context();
      h.guest_lr = uint32_t(ctx->lr);
      h.caller_count = guest_stack::Callers(ctx->r1.u32, h.callers, 8);
    }
    slot.serial.store(serial, std::memory_order_release);
    CountWriter(h.host_pc);
  }

  // Let this one write through. Graphics memory: run the SDK's own watchers
  // first (the emulated GPU / texture cache must hear about the write).
  if (page->physical.load()) {
    if (auto* memory = rex::system::kernel_memory()) {
      memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             page->guest.load(), uint32_t(kPageSize), true, false);
    }
  }
  mprotect(reinterpret_cast<void*>(host_page), kPageSize, PROT_READ | PROT_WRITE);
  if (page->armed.load()) {
    // Protect it again right after this ONE instruction (OnTrap).
    t_page = host_page;
    t_slot = slot_index;
    t_word = guest & ~3u;
    uc->uc_mcontext.gregs[REG_EFL] |= greg_t(kTrapFlag);
  }
  return true;
}

// SIGSEGV: ours first, then the SDK's handler (see Install for why we sit in
// front of it at the signal level instead of joining its handler list).
void OnSegv(int signal, siginfo_t* info, void* context) {
  if (HandleFault(uint64_t(info->si_addr),
                  uint64_t(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]),
                  static_cast<ucontext_t*>(context))) {
    return;
  }
  if ((g_old_segv.sa_flags & SA_SIGINFO) && g_old_segv.sa_sigaction) {
    g_old_segv.sa_sigaction(signal, info, context);
  } else if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN &&
             g_old_segv.sa_handler) {
    g_old_segv.sa_handler(signal);
  } else {
    // Nobody else: back to the default action; the fault repeats and ends the game.
    sigaction(signal, &g_old_segv, nullptr);
  }
}

// SIGTRAP: the single step after a watched write.
void OnTrap(int signal, siginfo_t* info, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
  if (t_page) {
    if (PageSlot* page = FindPage(t_page); page && page->armed.load()) {
      mprotect(reinterpret_cast<void*>(t_page), kPageSize, PROT_READ);
    }
    if (t_slot >= 0) {
      HitSlot& slot = g_ring[t_slot];
      slot.hit.new_value = ReadWord(t_word);
      slot.hit.done = true;
      slot.done.store(true, std::memory_order_release);
    }
    t_page = 0;
    t_slot = -1;
    uc->uc_mcontext.gregs[REG_EFL] &= ~greg_t(kTrapFlag);
    return;
  }
  // Not ours: hand it on (a debugger, if any, saw it before us anyway). A
  // default action would dump core (never wanted: gigabytes of mappings), so a stray
  // trap without another handler is ignored.
  if ((g_old_trap.sa_flags & SA_SIGINFO) && g_old_trap.sa_sigaction) {
    g_old_trap.sa_sigaction(signal, info, context);
  } else if (g_old_trap.sa_handler != SIG_DFL && g_old_trap.sa_handler != SIG_IGN &&
             g_old_trap.sa_handler) {
    g_old_trap.sa_handler(signal);
  }
}

}  // namespace

void Install() {
  if (g_installed) return;
  g_installed = true;
  struct sigaction sa {};
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = OnSegv;
  sigaction(SIGSEGV, &sa, &g_old_segv);
  sa.sa_sigaction = OnTrap;
  sigaction(SIGTRAP, &sa, &g_old_trap);
}

std::string Arm(uint32_t address, uint32_t size) {
  std::lock_guard lock(g_mutex);
  if (!g_installed) return "the write watch isn't installed";
  auto* memory = rex::system::kernel_memory();
  if (!memory) return "guest memory isn't up yet";
  if (size == 0 || size > kMaxRangeSize) return fmt::format("size 1..{} bytes", kMaxRangeSize);
  if (address + (size - 1) < address) return "the range wraps around";
  host_symbols::Load();  // names later; read the table now, outside any handler
  g_membase = memory->virtual_membase();
  guest_stack::SetMembase(g_membase);

  // Every guest page of the range must be committed and writable for the game.
  auto* heap = memory->LookupHeap(address);
  if (!heap || heap != memory->LookupHeap(address + size - 1)) {
    return fmt::format("0x{:08X} isn't one guest heap's memory", address);
  }
  for (uint64_t a = address & ~uint64_t(kPageSize - 1); a <= uint64_t(address) + size - 1;
       a += kPageSize) {
    uint32_t protect = 0;
    const uint32_t check = std::max<uint32_t>(uint32_t(a), address);
    if (!heap->QueryProtect(check, &protect) || !(protect & rex::memory::kMemoryProtectWrite)) {
      return fmt::format("0x{:08X} isn't writable guest memory", check);
    }
  }
  const bool physical = heap->heap_type() == rex::memory::HeapType::kGuestPhysical;

  // A free range slot, and room for the pages.
  RangeSlot* free_range = nullptr;
  for (RangeSlot& r : g_ranges) {
    if (r.size.load() == 0) {
      free_range = &r;
      break;
    }
  }
  if (!free_range) return fmt::format("{} ranges at most (who stop)", kMaxRanges);

  std::vector<PageSlot*> pages;
  for (uint64_t a = address & ~uint64_t(kPageSize - 1); a <= uint64_t(address) + size - 1;
       a += kPageSize) {
    const uint64_t host_page = uint64_t(memory->TranslateVirtual<uint8_t*>(uint32_t(a)));
    PageSlot* page = FindPage(host_page);
    if (!page) {
      for (PageSlot& p : g_pages) {
        uint64_t expected = 0;
        if (p.host.load() == 0) {
          p.guest = uint32_t(a);
          p.physical = physical;
          if (p.host.compare_exchange_strong(expected, host_page)) {
            page = &p;
            break;
          }
        }
      }
    }
    if (!page) return fmt::format("{} pages at most (who stop)", kMaxPages);
    pages.push_back(page);
  }

  free_range->address = address;
  free_range->size.store(size, std::memory_order_release);
  for (PageSlot* page : pages) {
    page->armed = true;
    mprotect(reinterpret_cast<void*>(page->host.load()), kPageSize, PROT_READ);
  }
  REXLOG_INFO("Write watch: watching 0x{:08X} ({} bytes{})", address, size,
              physical ? ", graphics memory" : "");
  return "";
}

void DisarmAll() {
  std::lock_guard lock(g_mutex);
  for (RangeSlot& r : g_ranges) r.size = 0;
  for (PageSlot& p : g_pages) {
    if (!p.armed.exchange(false)) continue;
    // Virtual memory: writable again (it was before we came). Graphics
    // memory: left read-only; the next write finds the page in our list,
    // runs the SDK's watchers and lifts it (HandleFault), so a GPU watch set on
    // it meanwhile isn't lost by a blind mprotect here.
    if (!p.physical) mprotect(reinterpret_cast<void*>(p.host.load()), kPageSize, PROT_READ | PROT_WRITE);
  }
  // Page slots stay listed (see PageSlot); hits and writers are forgotten.
  g_next = 0;
  for (HitSlot& s : g_ring) s.serial = 0;
  for (WriterSlot& w : g_writers) w.count = 0, w.pc = 0;
  REXLOG_INFO("Write watch: stopped");
}

std::vector<Range> Ranges() {
  std::vector<Range> out;
  for (const RangeSlot& r : g_ranges) {
    if (const uint32_t size = r.size.load()) out.push_back({r.address.load(), size});
  }
  return out;
}

std::vector<Hit> Hits() {
  std::vector<Hit> out;
  const uint64_t last = g_next.load();
  const uint64_t first = last > kRing ? last - kRing + 1 : 1;
  for (uint64_t serial = first; serial <= last; ++serial) {
    const HitSlot& slot = g_ring[(serial - 1) % kRing];
    if (slot.serial.load(std::memory_order_acquire) != serial) continue;  // being (re)written
    Hit h = slot.hit;
    h.done = slot.done.load(std::memory_order_acquire);
    if (slot.serial.load(std::memory_order_acquire) != serial) continue;
    out.push_back(h);
  }
  return out;
}

std::vector<Writer> Writers() {
  std::vector<Writer> out;
  for (const WriterSlot& w : g_writers) {
    if (const uint64_t pc = w.pc.load()) out.push_back({pc, w.count.load()});
  }
  return out;
}

uint64_t TotalHits() { return g_next.load(); }

}  // namespace write_watchpoints

#else  // not Linux x86-64: see write_watchpoints.h

namespace write_watchpoints {
void Install() {}
std::string Arm(uint32_t, uint32_t) { return "the write watch works on Linux x86-64 only"; }
void DisarmAll() {}
std::vector<Range> Ranges() { return {}; }
std::vector<Hit> Hits() { return {}; }
std::vector<Writer> Writers() { return {}; }
uint64_t TotalHits() { return 0; }
}  // namespace write_watchpoints

#endif
