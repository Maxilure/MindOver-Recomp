// =============================================================================
// debug/sound_stop_trace.cpp -- who starts and who stops a sound (--debug_sound_stop_trace)
// =============================================================================
//
// WHY: a voice line is sometimes cut off by the next one (two Aku Aku lines
// in a row: the second stops the first). Reading the code did not show where:
// the game's dialogue queue (CSoundDialogueBehaviour / CSpeechManager) waits
// for a playing line, so the stop comes from elsewhere. This logs, per sound
// INSTANCE (the 0x58-byte object Radical's sound player makes for each play,
// built by sub_822DD390), its start and its stop, each with the game's call
// stack, so a cut shows its maker.
//
// WHAT IS WRAPPED (strong definitions of the generated weak sub_X; the
// originals stay callable as __imp__sub_X):
//   sub_822DDA10  an instance gets its clip (r3 instance, r4 clip): the START
//                 of a play (called right after the instance is made).
//   sub_822DDAB8  an instance's state = r4 (top 3 bits of +0x48): 1 play,
//                 2 STOP (with the fade length at +0x4C, ms x 16).
//   sub_822DD8F0  an instance's playing voice faded out at once (200 ms):
//                 the speech bus's "stop everything" (sub_822D71A0) uses
//                 it, findings/61 s.2.
// THE INSTANCE: +0x30 the sound resource (CSoundResource), +0x38 the clip,
// +0x3C the list it plays on (an actor's / a dialogue behaviour's), +0x48
// state, +0x4C fade. THE CLIP: +0x10 -> its sound, whose +0x34 is the name's
// hash object (compared by sub_820FB8D0 when the game asks "same track?").
//
// LOG LINES:
//   Sound stop trace: start <inst> clip <clip> list <list> id <hash> | <stack>
//   Sound stop trace: stop  <inst> clip <clip> list <list> id <hash> | <stack>
//   Sound stop trace: fade  <inst> ... (sub_822DD8F0; only when it has a voice)
// <stack> = the game's callers, innermost first, as sub_X+offset.
// =============================================================================

#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "debug/guest_stack.h"
#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(debug_sound_stop_trace, false, "CrashMoM",
                    "Developer: log every sound the game starts and stops, with the game's call "
                    "stack (for finding who cuts a voice line off)");

extern "C" REX_FUNC(__imp__sub_822DDA10);
extern "C" REX_FUNC(__imp__sub_822DDAB8);
extern "C" REX_FUNC(__imp__sub_822DD8F0);

namespace {

uint32_t Word(uint32_t address) {
  uint32_t v = 0;
  return guest_stack::SafeRead32(address, &v) ? v : 0;
}

// The game's callers from r1 (+ the link register: these two are leaves
// without a frame of their own), as "sub_X+off sub_Y+off ...".
std::string Stack(PPCContext& ctx) {
  uint32_t callers[8] = {};
  callers[0] = uint32_t(ctx.lr);
  const int n = 1 + guest_stack::Callers(ctx.r1.u32, callers + 1, 7);
  std::string out;
  for (int i = 0; i < n; ++i) {
    const uint32_t fn = guest_stack::FunctionContaining(callers[i]);
    out += fmt::format(" sub_{:08X}+{:X}", fn, callers[i] - fn);
  }
  return out;
}

// The name hash the game compares (clip +0x10 -> +0x34), 0 if unreadable.
uint32_t ClipId(uint32_t clip) {
  if (!clip) return 0;
  const uint32_t sound = Word(clip + 0x10);
  return sound ? Word(sound + 0x34) : 0;
}

void Log(const char* what, PPCContext& ctx, uint32_t inst, uint32_t clip, const std::string& extra) {
  REXLOG_INFO("Sound stop trace: {} {:08X} clip {:08X} list {:08X} id {:08X}{} |{}", what, inst, clip,
              Word(inst + 0x3C), ClipId(clip), extra, Stack(ctx));
}

}  // namespace

extern "C" REX_FUNC(sub_822DDA10) {
  if (REXCVAR_GET(debug_sound_stop_trace)) Log("start", ctx, ctx.r3.u32, ctx.r4.u32, "");
  __imp__sub_822DDA10(ctx, base);
}

extern "C" REX_FUNC(sub_822DDAB8) {
  if (REXCVAR_GET(debug_sound_stop_trace) && ctx.r4.u32 == 2) {
    Log("stop ", ctx, ctx.r3.u32, Word(ctx.r3.u32 + 0x38), "");
  }
  __imp__sub_822DDAB8(ctx, base);
}

extern "C" REX_FUNC(sub_822DD8F0) {
  if (REXCVAR_GET(debug_sound_stop_trace) && Word(ctx.r3.u32 + 0x2C) != 0) {
    Log("fade ", ctx, ctx.r3.u32, Word(ctx.r3.u32 + 0x38), "");
  }
  __imp__sub_822DD8F0(ctx, base);
}
