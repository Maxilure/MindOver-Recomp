// =============================================================================
// command_ring.cpp -- the GPU command ring, read by the port
// =============================================================================
//
// WHAT THIS IS
//   GRAPHICS DRIVER STEP 4 (README "Going native", plans.md s.9, docs/findings/
//   32 s.12-13). Direct3D (inside the game) talks to the GPU through a
//   COMMAND RING: a circle of memory it fills with GPU commands ("packets"),
//   then it writes the new end position to a GPU register (CP_RB_WPTR) and the
//   GPU reads up to there, reporting back how far it got. Until now the SDK's
//   emulated GPU read the ring (and drew from it). The port draws with its own
//   renderer, from the game's renderer calls one level up (PDDI), so with
//   Native Only nothing the ring DRAWS is used any more. But D3D also WAITS on
//   the ring: fences ("tell me when you got here"), GPU callbacks (interrupts),
//   its own space in the ring. Those still have to happen, in the same order.
//
//   With --command_ring_native=true (launch setting, OFF by default) the port
//   takes the ring: the game's two ring set-up calls are answered at their
//   sites, its write-pointer register write is caught, and a "Command Ring"
//   thread of ours reads the packets, does what D3D waits for, and skips the
//   drawing. The emulated GPU never sees a command: no emulated picture that
//   session (Native Only forced, the renderer switch and the dual window
//   refuse with a banner). That trade is why it's a launch switch.
//
// WHAT THE RING HOLDS (packet census, findings/32 s.12) AND WHAT WE DO
//   Copied from the SDK's command processor (thirdparty/rexglue-sdk/src/
//   graphics/command_processor.cpp), packet by packet, minus the drawing:
//   type 0 / 1   register writes: stored in our register file. Side effects:
//                SCRATCH_REG0-7 written to memory (SCRATCH_ADDR + 4n) when
//                their SCRATCH_UMSK bit is set = D3D's fences; COHER_STATUS_HOST
//                gets its "dirty" bit 31 (resolve copies to make visible).
//   type 2       no-op.
//   INDIRECT_BUFFER(_PFD)   run that command buffer (address & 0x1FFFFFFF).
//   WAIT_REG_MEM            wait until memory / a register meets the test;
//                           on COHER_STATUS_HOST the status is cleared first
//                           (the SDK's MakeCoherent: nothing draws, so
//                           nothing is ever out of date).
//   REG_RMW, REG_TO_MEM, MEM_WRITE, COND_WRITE   as the SDK.
//   EVENT_WRITE             the event number into VGT_EVENT_INITIATOR.
//   EVENT_WRITE_SHD         a value (or the frame counter) into memory = fence.
//   EVENT_WRITE_EXT / _ZPD  the SDK's stand-in answers (full-screen extents;
//                           occlusion queries "passed" with
//                           query_occlusion_fake_sample_count, read by name).
//   INTERRUPT               the game's interrupt callback with SOURCE 1, once
//                           per CPU bit (the callback of step 2, vblank.cpp).
//   XE_SWAP                 the frame counter + 1 (the port presents itself).
//   SET_BIN_* / predicates  kept: a packet with the predicate bit runs only if
//                           (bin_select & bin_mask) != 0, swaps never.
//   everything else         drawing (draws, shader code, constants, state):
//                           skipped.
//   Memory values go through the packet's endian code, then are stored in the
//   host's order, exactly as the SDK does (GpuSwap + memory::store).
//   After each run the read position (in words) is written back to the
//   address D3D gave (VdEnableRingBufferRPtrWriteBack).
//
// THE SITES (xexdis, 2026-11-13)
//   0x82309E20  VdInitializeRingBuffer in D3D's device set-up sub_82309C20:
//               r3 = ring (physical, from MmGetPhysicalAddress), r4 = log2 of
//               its size in 8-byte units (the SDK: 1 << (r4 + 3) bytes)
//   0x82309E58  VdEnableRingBufferRPtrWriteBack: r3 = where to report the read
//               position (physical), r4 = log2 block size
//   0x82308DD8  `stw r29,1812(r11)`, r11 = 0x7FC80000: the CP_RB_WPTR write
//               (r29 = new end, in words) in D3D's kick sub_82308C48. Its only
//               write of that register.
//   The GPU's other registers D3D touches directly (the flip address, the
//   interrupt status the handler reads) stay with the SDK: plain register
//   reads / writes, no commands.
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "command_ring.h"
#include "vblank.h"

REXCVAR_DEFINE_BOOL(command_ring_native, false, "CrashMoM",
                    "Launch setting: the port reads the game's GPU command ring itself "
                    "instead of the emulated GPU. Native Only for the whole session (no "
                    "emulated picture, no dual window)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DECLARE(bool, vblank_native);
REXCVAR_DECLARE(bool, emulated_only);

namespace {

// Register numbers (rexglue register_table.inc).
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
constexpr uint32_t kRegisterCount = 0x8000;

// Type-3 opcodes we act on (rexglue xenos.h).
enum : uint32_t {
  kOpIndirectBufferPfd = 0x37,
  kOpWaitRegMem = 0x3C,
  kOpMemWrite = 0x3D,
  kOpRegToMem = 0x3E,
  kOpIndirectBuffer = 0x3F,
  kOpRegRmw = 0x21,
  kOpCondWrite = 0x45,
  kOpEventWrite = 0x46,
  kOpInterrupt = 0x54,
  kOpEventWriteShd = 0x58,
  kOpEventWriteExt = 0x5A,
  kOpEventWriteZpd = 0x5B,
  kOpSetBinMaskLo = 0x60,
  kOpSetBinMaskHi = 0x61,
  kOpSetBinSelectLo = 0x62,
  kOpSetBinSelectHi = 0x63,
  kOpXeSwap = 0x64,
  kOpSetBinMask = 0x50,
  kOpSetBinSelect = 0x51,
};

// The interrupt callback's first argument for a command-stream interrupt.
constexpr uint64_t kSourceCommandStream = 1;

struct Ring {
  std::atomic<bool> active{false};
  uint32_t ring_physical = 0;
  uint32_t ring_words = 0;
  uint32_t writeback_physical = 0;  // 0 = none

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<uint32_t> write_index{0};  // from the kick (words)
  uint32_t read_index = 0;                // the thread's own

  std::vector<uint32_t> regs = std::vector<uint32_t>(kRegisterCount, 0);
  uint64_t bin_select = 0xFFFFFFFFull;  // the SDK's starting values
  uint64_t bin_mask = 0xFFFFFFFFull;
  std::atomic<uint32_t> counter{0};     // vblanks + swaps (EVENT_WRITE_SHD bit 31)

  rex::system::object_ref<rex::system::XHostThread> thread;
  rex::system::XThread* self = nullptr;  // our guest thread, for interrupts

  // Statistics for the once-a-minute line.
  uint64_t packets = 0, interrupts = 0, waits = 0, fences = 0, swaps = 0;
};
Ring g;

auto* Memory() { return REX_KERNEL_STATE()->memory(); }

uint8_t* Physical(uint32_t address) { return Memory()->TranslatePhysical(address); }

uint32_t ReadBe(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// xenos::GpuSwap: the packet's endian code (address bits 0-1).
uint32_t GpuSwap(uint32_t value, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);  // 8in16
    case 2: return __builtin_bswap32(value);                                      // 8in32
    case 3: return (value >> 16) | (value << 16);                                 // 16in32
    default: return value;
  }
}

// memory::store / load: host order (the SDK's convention after GpuSwap).
void StoreHost(uint32_t physical, uint32_t value) { std::memcpy(Physical(physical), &value, 4); }
uint32_t LoadHost(uint32_t physical) {
  uint32_t v;
  std::memcpy(&v, Physical(physical), 4);
  return v;
}
void WriteMemory(uint32_t address_and_endian, uint32_t value) {
  StoreHost(address_and_endian & ~3u, GpuSwap(value, address_and_endian & 3));
}
uint32_t ReadMemory(uint32_t address_and_endian) {
  return GpuSwap(LoadHost(address_and_endian & ~3u), address_and_endian & 3);
}

// A reader over guest words: the primary ring (wraps around) or a command
// buffer (doesn't).
struct Reader {
  uint32_t base;      // physical address
  uint32_t capacity;  // in words
  uint32_t offset;    // read position, words
  uint32_t end;       // write position, words
  uint32_t Left() const { return end >= offset ? end - offset : capacity - offset + end; }
  uint32_t Read() {
    const uint32_t value = ReadBe(Physical(base + 4 * offset));
    offset = (offset + 1) % capacity;
    return value;
  }
  void Skip(uint32_t words) { offset = (offset + words) % capacity; }
};

uint32_t ReadRegister(uint32_t index) { return index < kRegisterCount ? g.regs[index] : 0; }

void WriteRegister(uint32_t index, uint32_t value) {
  if (index >= kRegisterCount) return;
  g.regs[index] = value;
  if (index >= kRegScratch0 && index <= kRegScratch7) {
    const uint32_t n = index - kRegScratch0;
    if ((1u << n) & g.regs[kRegScratchUmsk]) {
      // A fence: the value goes to memory, read by D3D (big-endian there).
      const uint32_t at = g.regs[kRegScratchAddr] + 4 * n;
      const uint32_t be = __builtin_bswap32(value);
      StoreHost(at, be);
      ++g.fences;
    }
  } else if (index == kRegCoherStatusHost) {
    g.regs[index] = value | 0x80000000u;  // "dirty" until a wait makes it coherent
  }
}

bool Test(uint32_t function, uint32_t value, uint32_t mask, uint32_t ref) {
  value &= mask;
  switch (function & 7) {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    case 7: return true;
    default: return false;  // 0: never
  }
}

void Interrupt(uint32_t cpu_mask) {
  uint32_t callback = 0, argument = 0;
  if (!vblank::GameCallback(callback, argument) || callback == 0) return;
  auto* dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  for (uint8_t cpu = 0; cpu < 6; ++cpu) {
    if (!(cpu_mask & (1u << cpu))) continue;
    g.self->SetActiveCpu(cpu);
    uint64_t args[] = {kSourceCommandStream, argument};
    dispatcher->ExecuteInterrupt(g.self->thread_state(), callback, args, 2);
    ++g.interrupts;
  }
}

void RunBuffer(Reader& r, int depth);

// One type-3 packet; the reader stands on its first data word.
void Type3(Reader& r, uint32_t header, int depth) {
  const uint32_t opcode = (header >> 8) & 0x7F;
  const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
  const uint32_t data_end = (r.offset + count) % r.capacity;
  // Predicated packets (tiling): only when a selected bin is in the mask.
  if ((header & 1) && ((g.bin_select & g.bin_mask) == 0 || opcode == kOpXeSwap)) {
    r.offset = data_end;
    return;
  }
  switch (opcode) {
    case kOpIndirectBuffer:
    case kOpIndirectBufferPfd: {
      const uint32_t address = r.Read() & 0x1FFFFFFF;
      const uint32_t words = r.Read() & 0xFFFFF;
      if (depth < 4 && words) {
        // A whole command buffer: read 0..words, never wrapping (capacity
        // one more than the length, so the end isn't taken for the start).
        Reader sub{address, words + 1, 0, words};
        RunBuffer(sub, depth + 1);
      }
    } break;
    case kOpWaitRegMem: {
      const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(),
                     wait = r.Read();
      ++g.waits;
      for (;;) {
        uint32_t value;
        if (info & 0x10) {
          value = ReadMemory(poll);
        } else {
          if (poll == kRegCoherStatusHost) g.regs[poll] = 0;  // MakeCoherent: nothing drew
          value = ReadRegister(poll);
        }
        if (Test(info, value, mask, ref)) break;
        if (!g.active.load(std::memory_order_relaxed)) break;
        if (wait >= 0x100) {
          std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
        } else {
          std::this_thread::yield();
        }
      }
    } break;
    case kOpRegRmw: {
      const uint32_t info = r.Read(), and_mask = r.Read(), or_mask = r.Read();
      uint32_t value = ReadRegister(info & 0x1FFF);
      value &= (info >> 31) ? ReadRegister(and_mask & 0x1FFF) : and_mask;
      value |= ((info >> 30) & 1) ? ReadRegister(or_mask & 0x1FFF) : or_mask;
      WriteRegister(info & 0x1FFF, value);
    } break;
    case kOpRegToMem: {
      const uint32_t reg = r.Read(), address = r.Read();
      WriteMemory(address, ReadRegister(reg));
    } break;
    case kOpMemWrite: {
      uint32_t address = r.Read();
      for (uint32_t i = 0; i + 1 < count; ++i, address += 4) WriteMemory(address, r.Read());
    } break;
    case kOpCondWrite: {
      const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(),
                     target = r.Read(), data = r.Read();
      const uint32_t value = (info & 0x10) ? ReadMemory(poll) : ReadRegister(poll);
      if (Test(info, value, mask, ref)) {
        if (info & 0x100) {
          WriteMemory(target, data);
        } else {
          WriteRegister(target, data);
        }
      }
    } break;
    case kOpEventWrite:
      WriteRegister(kRegVgtEventInitiator, r.Read() & 0x3F);
      break;
    case kOpEventWriteShd: {
      const uint32_t initiator = r.Read(), address = r.Read(), value = r.Read();
      WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
      WriteMemory(address, (initiator >> 31) ? g.counter.load() : value);
      ++g.fences;
    } break;
    case kOpEventWriteExt: {
      const uint32_t initiator = r.Read(), address = r.Read();
      WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
      // The SDK's stand-in: "the whole screen", as 16-bit big-endian values.
      const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      uint8_t* p = Physical(address & ~3u);
      for (int i = 0; i < 6; ++i) {
        p[2 * i] = uint8_t(extents[i] >> 8);
        p[2 * i + 1] = uint8_t(extents[i]);
      }
    } break;
    case kOpEventWriteZpd: {
      WriteRegister(kRegVgtEventInitiator, r.Read() & 0x3F);
      // Occlusion queries: the SDK's answer (a fixed "passed" count when the
      // query was ended: D3D wrote 0xFFFFFEED into ZPass/ZFail A or B).
      static const int32_t fake = [] {
        const std::string v = rex::cvar::GetFlagByName("query_occlusion_fake_sample_count");
        return v.empty() ? 1000 : int32_t(std::strtol(v.c_str(), nullptr, 10));
      }();
      const uint32_t at = ReadRegister(kRegRbSampleCountAddr);
      if (fake >= 0 && at) {
        uint32_t* counts = reinterpret_cast<uint32_t*>(Physical(at));
        const uint32_t finished = __builtin_bswap32(0xFFFFFEED);
        // xe_gpu_depth_sample_counts (8 words, little-endian: D3D swaps them):
        // Total_A/B, ZFail_A/B, ZPass_A/B, StencilFail_A/B.
        const bool ended = counts[4] == finished || counts[5] == finished ||
                           counts[2] == finished || counts[3] == finished;
        std::memset(counts, 0, 8 * 4);
        if (ended) {
          counts[4] = uint32_t(fake);  // ZPass_A (host order, as the SDK)
          counts[0] = uint32_t(fake);  // Total_A
        }
      }
    } break;
    case kOpInterrupt:
      Interrupt(r.Read());
      break;
    case kOpXeSwap:
      g.counter.fetch_add(1, std::memory_order_relaxed);
      ++g.swaps;
      break;
    case kOpSetBinMaskLo: g.bin_mask = (g.bin_mask & ~0xFFFFFFFFull) | r.Read(); break;
    case kOpSetBinMaskHi: g.bin_mask = (g.bin_mask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
    case kOpSetBinSelectLo: g.bin_select = (g.bin_select & ~0xFFFFFFFFull) | r.Read(); break;
    case kOpSetBinSelectHi: g.bin_select = (g.bin_select & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
    case kOpSetBinMask: {
      const uint64_t hi = r.Read(), lo = r.Read();
      g.bin_mask = (hi << 32) | lo;
    } break;
    case kOpSetBinSelect: {
      const uint64_t hi = r.Read(), lo = r.Read();
      g.bin_select = (hi << 32) | lo;
    } break;
    default:
      break;  // drawing: nothing to do without an emulated GPU
  }
  r.offset = data_end;  // every packet ends where its count says
}

void RunBuffer(Reader& r, int depth) {
  while (r.Left() > 0) {
    const uint32_t header = r.Read();
    ++g.packets;
    if (header == 0) continue;
    switch (header >> 30) {
      case 0: {
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t base = header & 0x7FFF;
        const bool one = (header >> 15) & 1;
        for (uint32_t m = 0; m < count; ++m) WriteRegister(one ? base : base + m, r.Read());
      } break;
      case 1: {
        const uint32_t a = r.Read(), b = r.Read();
        WriteRegister(header & 0x7FF, a);
        WriteRegister((header >> 11) & 0x7FF, b);
      } break;
      case 2:
        break;
      default:
        Type3(r, header, depth);
        break;
    }
  }
}

int ThreadMain() {
  g.self = rex::system::XThread::GetCurrentThread();
  auto last_report = std::chrono::steady_clock::now();
  uint64_t reported_packets = 0, reported_interrupts = 0, reported_waits = 0, reported_fences = 0,
           reported_swaps = 0;
  for (;;) {
    uint32_t write_index;
    {
      std::unique_lock<std::mutex> lock(g.mutex);
      g.wake.wait_for(lock, std::chrono::milliseconds(100), [] {
        return g.write_index.load(std::memory_order_acquire) != g.read_index;
      });
      write_index = g.write_index.load(std::memory_order_acquire);
    }
    if (write_index != g.read_index) {
      Reader r{g.ring_physical, g.ring_words, g.read_index, write_index % g.ring_words};
      RunBuffer(r, 0);
      g.read_index = write_index % g.ring_words;
      if (g.writeback_physical) {
        // The read position, big-endian (the SDK's store_and_swap).
        StoreHost(g.writeback_physical, __builtin_bswap32(g.read_index));
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report > std::chrono::seconds(60)) {
      REXLOG_INFO("Command ring: last minute {} packets, {} swaps, {} fences, {} waits, {} interrupts",
                  g.packets - reported_packets, g.swaps - reported_swaps,
                  g.fences - reported_fences, g.waits - reported_waits,
                  g.interrupts - reported_interrupts);
      reported_packets = g.packets;
      reported_swaps = g.swaps;
      reported_fences = g.fences;
      reported_waits = g.waits;
      reported_interrupts = g.interrupts;
      last_report = now;
    }
  }
  return 0;
}

}  // namespace

namespace command_ring {

bool Wanted() {
  return REXCVAR_GET(command_ring_native) && REXCVAR_GET(vblank_native) &&
         !REXCVAR_GET(emulated_only);
}

bool Active() { return g.active.load(std::memory_order_acquire); }

void OnVblank() { g.counter.fetch_add(1, std::memory_order_relaxed); }

}  // namespace command_ring

// -----------------------------------------------------------------------------
// VdInitializeRingBuffer (0x82309E20): r3 = ring (physical), r4 = log2 size.
// -----------------------------------------------------------------------------
bool CrashMomDriverRingBuffer(PPCRegister& r3, PPCRegister& r4) {
  if (!command_ring::Wanted()) return false;
  g.ring_physical = r3.u32;
  g.ring_words = (uint32_t(1) << (r4.u32 + 3)) / 4;
  g.read_index = 0;
  g.write_index.store(0);
  if (!g.thread) {
    g.thread = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(REX_KERNEL_STATE(), 128 * 1024, 0, [] { return ThreadMain(); }));
    g.thread->set_name("Command Ring");
    g.thread->Create();
  }
  g.active.store(true, std::memory_order_release);
  REXLOG_INFO("Command ring: the port reads the game's GPU commands ({} KB ring at {:08X}); the "
              "emulated GPU gets none this session",
              g.ring_words * 4 / 1024, g.ring_physical);
  return true;
}

// -----------------------------------------------------------------------------
// VdEnableRingBufferRPtrWriteBack (0x82309E58): r3 = where to report (physical).
// -----------------------------------------------------------------------------
bool CrashMomDriverRPtrWriteBack(PPCRegister& r3, PPCRegister& /*r4: block size*/) {
  if (!command_ring::Active()) return false;
  g.writeback_physical = r3.u32;
  return true;
}

// -----------------------------------------------------------------------------
// D3D's kick (0x82308DD8, `stw r29,1812(r11)` into CP_RB_WPTR): r29 = new end.
// -----------------------------------------------------------------------------
bool CrashMomRingKick(PPCRegister& r29) {
  if (!command_ring::Active()) return false;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.write_index.store(r29.u32, std::memory_order_release);
  }
  g.wake.notify_one();
  return true;  // jump over the register write: the SDK must not wake on it
}
