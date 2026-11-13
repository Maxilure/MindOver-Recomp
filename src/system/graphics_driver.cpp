// =============================================================================
// graphics_driver.cpp -- the graphics driver's simple answers, given by the port
// =============================================================================
//
// WHAT THIS IS
//   Direct3D on the Xbox 360 (linked into the game, code 0x82305000-0x82323000)
//   talks to the console's GRAPHICS DRIVER through 20 kernel calls named Vd*
//   ("video driver"). In the port the SDK's fake Xbox answers them and runs an
//   emulated GPU behind them. This file is STEP 1 of making the graphics driver
//   the port's own (README "Going native", plans.md s.9, docs/findings/32 s.9):
//   the 14 calls that are only QUESTIONS or no-ops are answered right at the
//   game's call sites (midasm hooks in the manifest, "GRAPHICS DRIVER"), so the
//   fake Xbox isn't asked any more (Xbox calls meter: 0 calls).
//
//   Left for later steps (they are the emulated GPU itself):
//     VdSetGraphicsInterruptCallback  the vertical-blank tick: DONE in step 2
//                                     (src/system/vblank.cpp)
//     VdSwap, VdGetSystemCommandBuffer, VdPersistDisplay  the swap: DONE in
//                                     step 3 (src/system/display_swap.cpp)
//     VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack  the command
//                                     ring + its progress reports (step 4)
//
// THE CALLS AND THEIR ANSWERS (found 2026-11-13: Xbox calls meter + every
// `bl` to the 20 import thunks 0x824BBD6C..0x824BBEFC in the generated code,
// each site read with xexdis; the SDK's answers in
// thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp)
//
//   VdQueryVideoMode (2 sites: D3D's device set-up sub_82311640, the display
//   reset sub_8231B8D0), r3 = 48-byte X_VIDEO_MODE. D3D keeps width / height
//   / refresh in device +0x5428/+0x542C/+0x5434; the refresh is read by the
//   flip function sub_82310728 (how far into a vertical blank a flip came).
//   Answer: THE SAME TV the game hears from XGetVideoMode (console_settings):
//   1280x720 16:9 60 Hz. The SDK answered its video_mode_* flags, which a
//   saved settings file could set to anything (1920x1080 at 180 Hz seen),
//   while its virtual screen really ticks at 60 Hz (SDK patch 0010): D3D
//   heard a refresh three times faster than the ticks it got.
//
//   VdQueryVideoFlags (sub_8231A2E0, scaler size), answer in r3: bit 0
//   widescreen, bit 1 width >= 1024, bit 2 width >= 1920 -> 3 for our TV.
//
//   VdGetCurrentDisplayInformation (3 sites), r3 = 0x58-byte X_DISPLAY_INFO:
//   the same TV, filled like the SDK (scaler 1:1, filter type 1, overscan a
//   quarter of each side, which is what the SDK puts there).
//
//   VdGetCurrentDisplayGamma (sub_823136B8, D3D's gamma-ramp set-up), r3 =
//   type, r4 = power (float): type 2 = TV (BT.709), power 2.2222223 (only
//   used with type 3). The SDK's values: the ramp the game builds, and that
//   both renderers apply, stays exactly the same.
//
//   VdInitializeScalerCommandBuffer (sub_8231BDF8): the driver writes the
//   display scaler's commands into a buffer D3D then copies into its own
//   (memcpy of result x 4 bytes). Arguments 10 and 11 are on the stack:
//   [r1+0x64] = buffer, [r1+0x6C] = word count (200). Answer = the SDK's: the
//   whole buffer filled with GPU no-op packets (0x80000000), result = count.
//   There is no scaler on a PC: the port's presenter sizes the picture.
//
//   Plain "yes / ok / nothing" answers (all the SDK's own):
//     VdRetrainEDRAM (sub_8231C050, EVERY FRAME, 2 sites) -> 0 (no retrain:
//       the second site only runs after a non-zero first answer)
//     VdRetrainEDRAMWorker (same function, once) -> 0
//     VdSetDisplayMode (sub_82311978) -> 0
//     VdIsHSIOTrainingSucceeded (sub_82311B70, device creation) -> 1
//     VdInitializeEngines (sub_82311A80) -> 1; VdShutdownEngines (2 sites) ->
//       nothing; VdEnableDisableClockGating (sub_82313C88) -> 0
//     VdCallGraphicsNotificationRoutines (sub_8231A480) -> 0
//     VdSetSystemCommandBufferGpuIdentifierAddress (3 sites) -> nothing
//
// SWITCH: --graphics_driver_native=false lets the SDK answer again (A/B).
// =============================================================================

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "console_settings.h"

REXCVAR_DEFINE_BOOL(graphics_driver_native, true, "CrashMoM",
                    "The port answers the graphics driver's simple questions (TV mode, "
                    "gamma, scaler, EDRAM retrain, engine start) itself; false = the "
                    "SDK's emulated Xbox answers (for comparisons)");

namespace {

using console_settings::kTvHeight;
using console_settings::kTvRefreshHz;
using console_settings::kTvWidth;

// VdGetCurrentDisplayGamma: 2 = TV (BT.709); the power is only read with type 3.
constexpr uint32_t kDisplayGammaType = 2;
constexpr float kDisplayGammaPower = 2.22222233f;

// VdQueryVideoFlags for the TV above: widescreen (1) + at least 1024 wide (2).
constexpr uint32_t kVideoFlags = 1 | 2;

// The GPU's no-op packet (PM4 type 2), what the scaler buffer is filled with.
constexpr uint32_t kGpuNop = 0x80000000;

uint8_t* Membase() { return REX_KERNEL_STATE()->memory()->virtual_membase(); }

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Store16(uint8_t* base, uint32_t address, uint16_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, address, bits);
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Logged once per call, so a session log shows the port answered it.
void Note(const char* call, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("Graphics driver: {} answered by the port", call);
}

bool On() { return REXCVAR_GET(graphics_driver_native); }

}  // namespace

// -----------------------------------------------------------------------------
// VdQueryVideoMode: r3 = X_VIDEO_MODE to fill (the console's TV).
// -----------------------------------------------------------------------------
bool CrashMomDriverVideoMode(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdQueryVideoMode (1280x720, 16:9, 60 Hz)", said);
  console_settings::WriteTvMode(r3.u32);
  return true;
}

// -----------------------------------------------------------------------------
// VdQueryVideoFlags: answer in r3.
// -----------------------------------------------------------------------------
bool CrashMomDriverVideoFlags(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdQueryVideoFlags", said);
  r3.u64 = kVideoFlags;
  return true;
}

// -----------------------------------------------------------------------------
// VdGetCurrentDisplayInformation: r3 = 0x58-byte X_DISPLAY_INFO (layout in the
// SDK's xboxkrnl_video.cpp), filled for our TV the way the SDK fills it.
// -----------------------------------------------------------------------------
bool CrashMomDriverDisplayInfo(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdGetCurrentDisplayInformation", said);
  uint8_t* base = Membase();
  const uint32_t info = r3.u32;
  std::memset(GuestPtr(base, info), 0, 0x58);
  Store16(base, info + 0x00, uint16_t(kTvWidth));   // front buffer width
  Store16(base, info + 0x02, uint16_t(kTvHeight));  // front buffer height
  // Scaler: source rect (0,0)-(w,h), output w x h, filter type 1 both ways.
  Store32(base, info + 0x08 + 0x08, kTvWidth);
  Store32(base, info + 0x08 + 0x0C, kTvHeight);
  Store32(base, info + 0x08 + 0x10, kTvWidth);
  Store32(base, info + 0x08 + 0x14, kTvHeight);
  Store32(base, info + 0x08 + 0x18, 1);  // vertical filter type
  Store32(base, info + 0x08 + 0x28, 1);  // horizontal filter type
  // Overscan: a quarter of each side (the SDK's numbers; D3D doesn't draw them).
  Store16(base, info + 0x40, uint16_t(kTvWidth / 4));
  Store16(base, info + 0x42, uint16_t(kTvHeight / 4));
  Store16(base, info + 0x44, uint16_t(kTvWidth / 4));
  Store16(base, info + 0x46, uint16_t(kTvHeight / 4));
  Store16(base, info + 0x48, uint16_t(kTvWidth));   // display width
  Store16(base, info + 0x4A, uint16_t(kTvHeight));  // display height
  StoreFloat(base, info + 0x4C, kTvRefreshHz);
  Store32(base, info + 0x50, 0);                    // interlaced: no
  Store16(base, info + 0x56, uint16_t(kTvWidth));   // actual display width
  return true;
}

// -----------------------------------------------------------------------------
// VdGetCurrentDisplayGamma: r3 = &type, r4 = &power.
// -----------------------------------------------------------------------------
bool CrashMomDriverDisplayGamma(PPCRegister& r3, PPCRegister& r4) {
  if (!On()) return false;
  static bool said = false;
  Note("VdGetCurrentDisplayGamma", said);
  uint8_t* base = Membase();
  if (r3.u32) Store32(base, r3.u32, kDisplayGammaType);
  if (r4.u32) StoreFloat(base, r4.u32, kDisplayGammaPower);
  return true;
}

// -----------------------------------------------------------------------------
// VdInitializeScalerCommandBuffer: buffer = [r1+0x64], word count = [r1+0x6C]
// (arguments 10 and 11, stored by the caller sub_8231BDF8 right before the
// call); answer in r3 = words written.
// -----------------------------------------------------------------------------
bool CrashMomDriverScalerCommands(PPCRegister& r1, PPCRegister& r3) {
  if (!On()) return false;
  uint8_t* base = Membase();
  const uint32_t buffer = Load32(base, r1.u32 + 0x64);
  const uint32_t count = Load32(base, r1.u32 + 0x6C);
  // The caller's buffer is 200 words on its stack; anything far off that
  // means the arguments moved: let the SDK answer instead.
  if (buffer == 0 || count > 4096) return false;
  static bool said = false;
  Note("VdInitializeScalerCommandBuffer", said);
  for (uint32_t i = 0; i < count; ++i) Store32(base, buffer + 4 * i, kGpuNop);
  r3.u64 = count;
  return true;
}

// -----------------------------------------------------------------------------
// The plain answers: r3 = the value (calls that return nothing leave r3 as the
// SDK's thunk would: untouched).
// -----------------------------------------------------------------------------
bool CrashMomDriverRetrainEdram(PPCRegister& r3) {  // every frame
  if (!On()) return false;
  static bool said = false;
  Note("VdRetrainEDRAM (every frame)", said);
  r3.u64 = 0;
  return true;
}

bool CrashMomDriverRetrainEdramWorker(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdRetrainEDRAMWorker", said);
  r3.u64 = 0;
  return true;
}

bool CrashMomDriverSetDisplayMode(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdSetDisplayMode", said);
  r3.u64 = 0;
  return true;
}

bool CrashMomDriverHsioTraining(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdIsHSIOTrainingSucceeded", said);
  r3.u64 = 1;
  return true;
}

bool CrashMomDriverInitializeEngines(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdInitializeEngines", said);
  r3.u64 = 1;
  return true;
}

bool CrashMomDriverShutdownEngines(PPCRegister& /*r3: nothing returned*/) {
  if (!On()) return false;
  static bool said = false;
  Note("VdShutdownEngines", said);
  return true;
}

bool CrashMomDriverClockGating(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdEnableDisableClockGating", said);
  r3.u64 = 0;
  return true;
}

bool CrashMomDriverNotificationRoutines(PPCRegister& r3) {
  if (!On()) return false;
  static bool said = false;
  Note("VdCallGraphicsNotificationRoutines", said);
  r3.u64 = 0;
  return true;
}

bool CrashMomDriverGpuIdentifierAddress(PPCRegister& /*r3: nothing returned*/) {
  if (!On()) return false;
  static bool said = false;
  Note("VdSetSystemCommandBufferGpuIdentifierAddress", said);
  return true;
}
