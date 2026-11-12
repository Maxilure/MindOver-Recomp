// =============================================================================
// system_messages.cpp -- the game's "system messages" (XMsg*), answered by the port
// =============================================================================
//
// WHAT THIS IS
//   On the Xbox 360 a game talks to some system services by sending numbered
//   MESSAGES to a system "app" (XMsgInProcessCall / XMsgStartIORequest(Ex)).
//   This game sends them from two small Microsoft helpers only (found
//   2026-11-12 with the Xbox calls meter; docs/findings/32 s.7):
//
//   1. THE DASHBOARD MUSIC PLAYER (app 0xFA "XMP", "custom soundtracks").
//      A 360 player could run their own music over any game; the game then
//      mutes its own. Radical's music code asks every frame:
//        sub_822DE3C8 -> sub_82300130 = XMPGetStatus (message 0x00070009)
//      and movies / cutscene tracks take the music and give it back:
//        sub_82300140 = XMPOverrideBackgroundMusic (asks 0x0007001B
//                       GetPlaybackController, sets 0x0007001A)
//        sub_82300218 = XMPRestoreBackgroundMusic (sets 0x0007001A)
//      ~200 messages a second, the meter's biggest XMsg number.
//      A PC has no dashboard music. Answers: status 0 = "idle" (no player
//      music; the SDK's answer every session so far, so the game keeps its
//      own music exactly as before), take / give back = success, no message.
//
//   2. RICH PRESENCE (app 0xFB "XGI", message 0x000B0006 XUserSetContext).
//      sub_8227B3B8 (main-loop subsystems) sets two contexts per player with
//      a controller (context 1 = what they're doing, 0x8001 = presence) via
//      sub_82300770 -> sub_823006E8: the friends list's "what is Crash doing"
//      line. The SDK only logged it. Answer: success, nothing sent. (The
//      values could feed a PC rich presence one day: findings/32 s.7.2.)
//
//   With these four overrides none of the three XMsg imports is called.
//   The game's own logic around them (music counting, muting, the presence
//   loop) is untouched: it hears what the emulated Xbox told it before.
//
//   Note: the SDK's XMPGetStatus SLEEPS 1 ms when asked from any thread but
//   the main one ("some games hammer this"); ours answers at once.
//
// SWITCH: --system_messages_native=false = the SDK's emulated Xbox answers
// again (A/B; the meter then shows the old callers).
// =============================================================================

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"

// The originals (generated code), for the A/B switch.
extern "C" REX_FUNC(__imp__sub_82300130);
extern "C" REX_FUNC(__imp__sub_82300140);
extern "C" REX_FUNC(__imp__sub_82300218);
extern "C" REX_FUNC(__imp__sub_823006E8);

REXCVAR_DEFINE_BOOL(system_messages_native, true, "CrashMoM",
                    "The port answers the game's system messages itself (dashboard music "
                    "player, rich presence); false = the SDK's emulated Xbox answers "
                    "(for comparisons)");

namespace {

constexpr uint32_t kSuccess = 0;   // ERROR_SUCCESS, what every wrapper returns on success
constexpr uint32_t kXmpIdle = 0;   // XMP_STATE_IDLE: no player music

// Logged once per kind, so a session log shows the port answered it.
void Note(const char* what, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("System messages: {} answered by the port", what);
}

}  // namespace

// -----------------------------------------------------------------------------
// sub_82300130(r3 = u32* state) = XMPGetStatus(2, state). Asked every frame
// by the music player's update (through sub_822DE3C8, which turns "idle" into
// "the game may play its own music").
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82300130) {
  if (!REXCVAR_GET(system_messages_native)) {
    __imp__sub_82300130(ctx, base);
    return;
  }
  static bool said = false;
  Note("music player status (idle)", said);
  const uint32_t state_ptr = ctx.r3.u32;
  if (state_ptr) {
    uint8_t* p = GuestPtr(base, state_ptr);
    p[0] = p[1] = p[2] = 0;
    p[3] = uint8_t(kXmpIdle);
  }
  ctx.r3.u64 = kSuccess;
}

// -----------------------------------------------------------------------------
// sub_82300140() = XMPOverrideBackgroundMusic: the game takes the music for a
// movie / cutscene track (counted by sub_822DE2D8: only the first take gets
// here). Nothing to take on a PC.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82300140) {
  if (!REXCVAR_GET(system_messages_native)) {
    __imp__sub_82300140(ctx, base);
    return;
  }
  static bool said = false;
  Note("take the background music", said);
  ctx.r3.u64 = kSuccess;
}

// -----------------------------------------------------------------------------
// sub_82300218() = XMPRestoreBackgroundMusic: gives it back (last give-back,
// counted by sub_822DE320).
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82300218) {
  if (!REXCVAR_GET(system_messages_native)) {
    __imp__sub_82300218(ctx, base);
    return;
  }
  static bool said = false;
  Note("give back the background music", said);
  ctx.r3.u64 = kSuccess;
}

// -----------------------------------------------------------------------------
// sub_823006E8(r3 user, r4 context id, r5 value, r6 overlapped) =
// XUserSetContextEx. Its only caller is sub_82300770 (r6 = 0 = wait for the
// result), from the presence loop sub_8227B3B8.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_823006E8) {
  if (!REXCVAR_GET(system_messages_native)) {
    __imp__sub_823006E8(ctx, base);
    return;
  }
  static bool said = false;
  Note("rich presence", said);
  ctx.r3.u64 = kSuccess;
}
