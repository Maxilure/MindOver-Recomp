// =============================================================================
// console_settings.cpp -- the console's settings, answered by the port
// =============================================================================
//
// WHAT THIS IS
//   On a real Xbox 360 the game (and Microsoft's libraries linked into it)
//   asks the console a handful of questions about itself: "what TV mode is
//   it in?", "which video cable is plugged in?", "what are the user's video
//   flags?", "which dashboard version is this?". Without this file the SDK's
//   fake Xbox answers them, partly from its own leftover flags (the TV size
//   came from video_mode_width/height/refresh_rate, which a saved settings
//   file could set to any size and refresh rate). There is no console on a PC:
//   the port gives fixed, documented answers itself, at the call sites, so
//   the fake Xbox isn't asked at all (Xbox calls meter: 0 calls).
//   "Console settings" in README's "Going native" table, plans.md s.9,
//   docs/findings/32 s.6.
//
// THE QUESTIONS AND WHO ASKED (found 2026-11-12 with the Xbox calls meter +
// a scan of every `bl` to the import thunks + Ghidra on each caller)
//
//   XGetVideoMode (thunk 0x824BB94C), 4 call sites, r3 = a 48-byte
//   X_VIDEO_MODE (big-endian: width, height, interlaced, widescreen, hi-def,
//   refresh float, video standard, 2 unknown words, 3 reserved):
//     0x82433B20  PDDI's display set-up sub_82433B00: widescreen -> xnDisplay
//                 +0x678 bit 0x40; hi-def -> back buffers 1280x720 (else 640x480)
//     0x8227A698  the game's own start-up sub_8227A620: video standard 3 (PAL)
//                 -> byte 13 of the movie object (sub_822C8C18) = 1
//     0x82316D10  D3D (sub_82316C48: the display's scaler, re-set when the mode
//     0x82318B44  differs) + sub_82318A50 (scaler filter ratios, 4:3 vs 16:9)
//   Answer: 1280 x 720, progressive, widescreen, hi-def, 60 Hz, NTSC: the
//   picture the game was made for. The port draws that picture at any size
//   and shape itself (render_resolution, widescreen.*, the presenter), so the
//   game must always hear the same TV, whatever the player's monitor is.
//
//   XamGetSystemVersion (0x824BB99C), 3 call sites: Microsoft library code
//   (0x82300140 / 0x82300218 / 0x82322A90) that picks old or new dashboard
//   features by version. Answer 0, the SDK's answer since the first boot:
//   "an old dashboard", the paths every session has used so far.
//
//   ExGetXConfigSetting (0x824BBADC) at 0x82311BE8: D3D's device creation
//   sub_82311B70 reads the user VIDEO FLAGS (category 3, setting 10) into
//   device +0x414C. Answer 0x00040000, the SDK's value (all sessions so far).
//   (The other ExGetXConfigSetting sites: 2 in the start-up check below; the
//   country in sub_82300398 and the region in sub_82474258 are only called
//   from functions language.cpp replaced; the 8 in sub_824769B0 belong to a
//   Microsoft helper never reached in play: 0 calls in the meter.)
//
//   THE START-UP CHECK sub_823009B8, called from the program's ENTRY POINT
//   0x82300BA0 (bl at 0x82300BE0): a Microsoft library check for a PAL console
//   on a standard-definition cable set to 60 Hz: XexCheckExecutablePrivilege(10),
//   XGetAVPack (cable kind), ExGetXConfigSetting(2,2) + (3,10), then
//   XGetLanguage picks the text of a system message box; it returns 1 when it
//   showed one and the entry point then ends the program (0x824BBA5C). The
//   SDK said cable 6 (VGA) = it always returned 0. A PC has no cable: the
//   override returns 0 straight away (same result, 5 questions fewer).
//
//   XGetLanguage / XGetGameRegion / XamGetExecutionId: their only callers
//   were replaced before (language.cpp sub_82262980, who_plays.cpp
//   sub_82322960) or are in the start-up check above.
//
// SWITCH: --console_settings_native=false lets the SDK answer again (A/B).
// =============================================================================

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "console_settings.h"

// The original start-up check (the generated code), for the A/B switch.
extern "C" REX_FUNC(__imp__sub_823009B8);

REXCVAR_DEFINE_BOOL(console_settings_native, true, "CrashMoM",
                    "The port answers the game's questions about the console (TV mode, "
                    "video flags, dashboard version, start-up cable check) itself; "
                    "false = the SDK's emulated Xbox answers (for comparisons)");

namespace {

// The TV the game hears (see the header).
using console_settings::kTvWidth;
using console_settings::kTvHeight;
using console_settings::kTvRefreshHz;
constexpr uint32_t kVideoStandardNtsc = 1;  // 1 NTSC-M, 2 NTSC-J, 3 PAL

// XCONFIG user video flags (category 3, setting 10): the value the SDK gave.
constexpr uint32_t kUserVideoFlags = 0x00040000;

// The dashboard version: 0 = "old", what the SDK answered.
constexpr uint32_t kSystemVersion = 0;

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

// Logged once per question, so a session log shows the port answered it.
void Note(const char* question, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("Console settings: {} answered by the port", question);
}

}  // namespace

// -----------------------------------------------------------------------------
// XGetVideoMode call sites (manifest "CONSOLE SETTINGS"): r3 = X_VIDEO_MODE.
// -----------------------------------------------------------------------------
bool CrashMomConsoleVideoMode(PPCRegister& r3) {
  if (!REXCVAR_GET(console_settings_native)) return false;
  static bool said = false;
  Note("TV mode (1280x720, 16:9, 60 Hz, NTSC)", said);
  console_settings::WriteTvMode(r3.u32);
  return true;  // jump over the call (it returns nothing)
}

// The TV answer itself, shared with graphics_driver.cpp (D3D asks the same
// question through the graphics driver's VdQueryVideoMode).
void console_settings::WriteTvMode(uint32_t mode) {
  uint8_t* base = Membase();
  std::memset(GuestPtr(base, mode), 0, 48);
  Store32(base, mode + 0, kTvWidth);
  Store32(base, mode + 4, kTvHeight);
  Store32(base, mode + 8, 0);   // interlaced: no
  Store32(base, mode + 12, 1);  // widescreen
  Store32(base, mode + 16, 1);  // hi-def
  uint32_t refresh_bits;
  std::memcpy(&refresh_bits, &kTvRefreshHz, 4);
  Store32(base, mode + 20, refresh_bits);
  Store32(base, mode + 24, kVideoStandardNtsc);
  // Two words nobody here reads; the values the SDK (Xenia) puts there.
  Store32(base, mode + 28, 0x4A);
  Store32(base, mode + 32, 0x01);
}

// -----------------------------------------------------------------------------
// XamGetSystemVersion call sites: the answer goes in r3.
// -----------------------------------------------------------------------------
bool CrashMomConsoleSystemVersion(PPCRegister& r3) {
  if (!REXCVAR_GET(console_settings_native)) return false;
  static bool said = false;
  Note("dashboard version", said);
  r3.u64 = kSystemVersion;
  return true;
}

// -----------------------------------------------------------------------------
// ExGetXConfigSetting(3, 10, buffer r5, size r6, required size r7) in D3D's
// device creation (0x82311BE8). Result in r3 (0 = success; checked with
// `cmpwi r3,0 / blt` right after).
// -----------------------------------------------------------------------------
bool CrashMomConsoleVideoFlags(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                               PPCRegister& r6, PPCRegister& r7) {
  if (!REXCVAR_GET(console_settings_native)) return false;
  // Only the question this site asks; anything else goes to the SDK.
  if ((r3.u32 & 0xFFFF) != 3 || (r4.u32 & 0xFFFF) != 10 || (r6.u32 & 0xFFFF) < 4) {
    return false;
  }
  static bool said = false;
  Note("user video flags", said);
  uint8_t* base = Membase();
  if (r5.u32) Store32(base, r5.u32, kUserVideoFlags);
  if (r7.u32) Store16(base, r7.u32, 4);
  r3.u64 = 0;
  return true;
}

// -----------------------------------------------------------------------------
// The start-up cable check sub_823009B8 (see the header): 0 = "nothing to
// tell the player, carry on".
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_823009B8) {
  if (!REXCVAR_GET(console_settings_native)) {
    __imp__sub_823009B8(ctx, base);
    return;
  }
  static bool said = false;
  Note("start-up video cable check", said);
  ctx.r3.u64 = 0;
}
