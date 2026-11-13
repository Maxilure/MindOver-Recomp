// =============================================================================
// display_swap.cpp -- the graphics driver's swap, and D3D's gamma ramp
// =============================================================================
//
// WHAT THIS IS
//   GRAPHICS DRIVER STEP 3 (README "Going native", plans.md s.9, docs/findings/
//   32 s.11). When a frame is finished, D3D's swap sub_82310DA8 asks the
//   console's graphics driver three things:
//     VdGetSystemCommandBuffer  "give me the system command buffer" (0x82310F90)
//     VdSwap                    "put this frontbuffer on the TV": the driver
//                               writes 64 words into the GPU command ring that
//                               D3D reserved for it (0x82310FF4)
//     VdPersistDisplay          "keep the picture across a title switch"
//                               (0x823112B0, once)
//   The port answers all three at their call sites (manifest "GRAPHICS DRIVER
//   STEP 3"). And the native renderer now takes the DISPLAY GAMMA RAMP straight
//   from D3D (below) instead of from the SDK's emulated GPU.
//
// THE ANSWERS (found 2026-11-13: Xbox calls meter, xexdis of each site, the
// SDK's answers in thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp)
//
//   VdGetSystemCommandBuffer(r3 = 0x94-byte block, r4 = word): block zeroed,
//   0xBEEF0000 at its start, 0xBEEF0001 in the word = the SDK's tokens. D3D
//   passes both back to VdSwap (r6 / r7) and never looks inside.
//
//   VdSwap(r3 = the 64 reserved ring words, r4 = the frontbuffer texture's D3D
//   fetch constant (6 words), r8 = &frontbuffer address, width / height
//   pointers = stack arguments 9-10 at [r1+0x54] / [r1+0x5C]; r5-r7, r9, r10
//   = system words, format, colour space: unused). The 64 words are what the
//   GPU will run, so they keep the SDK's exact content while the emulated GPU
//   exists (it shows the emulated picture from them):
//     type-0 packet: the 6-word fetch constant into SHADER_CONSTANT_FETCH_00,
//                    its base address turned from the CPU's view (virtual)
//                    into the GPU's (physical)
//     type-3 XE_SWAP: 'SWAP', physical frontbuffer address, width, height
//     type-2 no-ops for the rest.
//   Without the emulated GPU (later steps) the same words are simply never run.
//
//   VdPersistDisplay(r3, r4 = &out): the SDK allocated 64 bytes and returned 1;
//   D3D's only use of a non-zero answer is to free that block right away
//   (MmFreePhysicalMemory(1, [r1+160]) at 0x823112C4). A PC keeps no picture
//   across title switches: answer 0, nothing to allocate or free.
//
//   THE GAMMA RAMP. D3D builds the TV's ramp itself: the game's gamma ramp
//   corrected for the TV's gamma (VdGetCurrentDisplayGamma, step 1) =
//   3 x 256 u16, values in bits 6-15 (red at +0, green +512, blue +1024).
//   sub_82313960(r3 = device, r4 = that table) writes it into the command ring
//   as display-register writes (DC_LUT_30_COLOR, red << 20 | green << 10 |
//   blue), which the SDK's emulated GPU collected and the native renderer read
//   from there (native_renderer.cpp GammaRampReader). The writer is wrapped:
//   the original still runs (the emulated GPU needs its packets), and the port
//   keeps its own copy of the 256 packed entries, in the SAME layout, for the
//   native renderer. (D3D's other writer, the piecewise-linear one
//   sub_82313A58, is only reached through sub_823105B8 and was never used by
//   the native renderer.)
//
// SWITCH: --display_swap_native=false lets the SDK answer again and the native
// renderer read the emulated GPU's ramp (A/B). --debug_gamma_check logs whether
// the two ramps agree, each time D3D sends a new one.
// =============================================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "display_swap.h"

// The original ramp writer (generated code): still run after we copy.
extern "C" REX_FUNC(__imp__sub_82313960);

REXCVAR_DEFINE_BOOL(display_swap_native, true, "CrashMoM",
                    "The port answers the graphics driver's swap calls and reads the display "
                    "gamma ramp from D3D itself; false = the SDK's emulated Xbox does (for "
                    "comparisons)");

namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kSwapRingWords = 64;        // what D3D reserves for VdSwap
constexpr uint32_t kSystemBlockBytes = 0x94;   // VdGetSystemCommandBuffer's block
constexpr uint32_t kSystemToken0 = 0xBEEF0000;
constexpr uint32_t kSystemToken1 = 0xBEEF0001;
constexpr uint16_t kFetchConstant0 = 0x4800;   // SHADER_CONSTANT_FETCH_00_0

uint8_t* Membase() { return REX_KERNEL_STATE()->memory()->virtual_membase(); }

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Load16(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint16_t((p[0] << 8) | p[1]);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Note(const char* call, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("Graphics driver: {} answered by the port", call);
}

bool On() { return REXCVAR_GET(display_swap_native); }

// D3D's ramp, packed like the GPU's DC_LUT_30_COLOR (the native renderer's
// layout). Written on the thread that runs D3D (the game's main thread),
// read by the renderer: a mutex, the copy is 1 KB a few times a session.
struct Ramp {
  std::mutex mutex;
  std::array<uint32_t, 256> entries{};
  bool seen = false;
  uint32_t version = 0;
};
Ramp g_ramp;

}  // namespace

// -----------------------------------------------------------------------------
// VdGetSystemCommandBuffer (0x82310F90): r3 = block, r4 = word.
// -----------------------------------------------------------------------------
bool CrashMomDriverSystemCommandBuffer(PPCRegister& r3, PPCRegister& r4) {
  if (!On()) return false;
  static bool said = false;
  Note("VdGetSystemCommandBuffer (every frame)", said);
  uint8_t* base = Membase();
  std::memset(GuestPtr(base, r3.u32), 0, kSystemBlockBytes);
  Store32(base, r3.u32, kSystemToken0);
  Store32(base, r4.u32, kSystemToken1);
  return true;
}

// -----------------------------------------------------------------------------
// VdSwap (0x82310FF4): see the header for the registers.
// -----------------------------------------------------------------------------
bool CrashMomDriverSwap(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r8) {
  if (!On()) return false;
  uint8_t* base = Membase();
  auto* memory = REX_KERNEL_STATE()->memory();

  // The frontbuffer texture's fetch constant, as D3D keeps it (big-endian).
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = Load32(base, r4.u32 + 4 * i);
  const uint32_t virtual_address = fetch[1] & 0xFFFFF000;  // base_address << 12
  const uint32_t physical_address = memory->GetPhysicalAddress(virtual_address);
  const uint32_t width = Load32(base, Load32(base, r1.u32 + 0x54));
  const uint32_t height = Load32(base, Load32(base, r1.u32 + 0x5C));
  if (physical_address == UINT32_MAX || Load32(base, r8.u32) != virtual_address) {
    // Not the frontbuffer the SDK expects either: let it decide (it logs).
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("Graphics driver: VdSwap with an unexpected frontbuffer {:08X}: the SDK answers",
                  virtual_address);
    }
    return false;
  }
  static bool said = false;
  Note("VdSwap (every frame)", said);
  fetch[1] = (fetch[1] & 0x00000FFF) | (physical_address & 0xFFFFF000);

  const uint32_t ring = r3.u32;
  uint32_t n = 0;
  auto put = [&](uint32_t word) { Store32(base, ring + 4 * n++, word); };
  put(xenos::MakePacketType0(kFetchConstant0, 6));
  for (uint32_t word : fetch) put(word);
  put(xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4));
  put(xenos::kSwapSignature);
  put(physical_address);
  put(width);
  put(height);
  while (n < kSwapRingWords) put(xenos::MakePacketType2());
  return true;
}

// -----------------------------------------------------------------------------
// VdPersistDisplay (0x823112B0): 0 = nothing kept (D3D then frees nothing).
// -----------------------------------------------------------------------------
bool CrashMomDriverPersistDisplay(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdPersistDisplay", said);
  r3.u64 = 0;
  return true;
}

// -----------------------------------------------------------------------------
// D3D's 256-entry ramp writer sub_82313960(r3 = device, r4 = 3 x 256 u16).
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82313960) {
  if (On()) {
    uint8_t* b = base;
    const uint32_t table = ctx.r4.u32;
    std::array<uint32_t, 256> entries;
    for (uint32_t i = 0; i < 256; ++i) {
      // Values sit in bits 6-15; the register keeps 10 bits per colour.
      const uint32_t red = Load16(b, table + 2 * i) >> 6;
      const uint32_t green = Load16(b, table + 512 + 2 * i) >> 6;
      const uint32_t blue = Load16(b, table + 1024 + 2 * i) >> 6;
      entries[i] = (red << 20) | (green << 10) | blue;
    }
    std::lock_guard<std::mutex> lock(g_ramp.mutex);
    if (!g_ramp.seen) REXLOG_INFO("Gamma: the native renderer reads D3D's display ramp itself");
    g_ramp.entries = entries;
    g_ramp.seen = true;
    ++g_ramp.version;
  }
  __imp__sub_82313960(ctx, base);
}

namespace display_swap {

bool GammaRamp(std::array<uint32_t, 256>& out, uint32_t* version) {
  if (!On()) return false;
  std::lock_guard<std::mutex> lock(g_ramp.mutex);
  if (!g_ramp.seen) return false;
  out = g_ramp.entries;
  if (version) *version = g_ramp.version;
  return true;
}

}  // namespace display_swap
