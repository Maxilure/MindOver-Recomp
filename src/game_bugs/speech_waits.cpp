// =============================================================================
// game_bugs/speech_waits.cpp -- a new hint line waits for the one being spoken
// =============================================================================
//
// THE BUG (the Xbox 360 release has it too): Aku Aku's hint lines (the
// "tutorial" voice: "I smell evil...", the moves he explains) are played by
// the level scripts' CActionTutorialMessage event. When one is asked for
// while another is still being spoken (two hint volumes close together, a
// hint right after a story line), the new one does not wait: it STOPS every
// voice still playing on the speech bus (200 ms fade) and starts at once. The
// old line is cut mid-sentence, and its subtitle stays on screen next to the
// new one, as if both were spoken.
//
// HOW IT WAS FOUND (findings/61 s.2): --debug_sound_stop_trace
// (debug/sound_stop_trace.cpp) showed the tutorial voice started by the front
// end's sound player sub_82261698 from CActionTutorialMessage's Update
// sub_8229E838 (vtable 0x82040618 slot 1). Right before, that Update calls
// sub_822D71A0(sound manager *(game+88), &name 0x825A7598): it walks EVERY
// sound instance (global list 0x825A75A4: node at instance +8, next = word at
// the node; the playing voice at instance +0x2C), follows the voice's buses
// (voice +108 -> first bus sub_82333EF0, next sub_82333EF8) and fades out
// (sub_822DD8F0, 200 ms) each one on the bus whose name hash (bus +16) equals
// the word at 0x825A7598. The villagers' dialogue event (CActionPlayVillagerDialog,
// sub_82297260) and a level change (sub_822D6188 state 5) do the same; they
// are left alone (a villager answers the player's own button press; a level
// change ends everything anyway).
//
// THE FIX: CActionTutorialMessage's Update is wrapped. It is called every
// frame until it returns 1 ("done"; 0 = ask again next frame, the original's
// own retry while its sound can't be had). While a voice is still playing on
// the speech bus, the wrapper answers 0 without running the original: the
// event (and the script's events after it) waits, and the line starts when
// the bus is free, in full. Never longer than kMaxWaitSeconds (a looping
// sound on that bus would hold hints back forever): then the original runs as
// before. The check is a dry run of sub_822D71A0's own walk (same list, same
// bus test), on the same thread the original runs it.
//
// Switch: fix_speech_cut_off (default on; off = the original's cut).
// Log: "Speech: a hint line waits for the voice still speaking" once per
// wait, "... waited N.N s" when it plays.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

#include "guest_memory.h"
#include "game_time.h"  // game time (exact runs / online lockstep), game_time.h

REXCVAR_DEFINE_BOOL(fix_speech_cut_off, true, "CrashMoM",
                    "A new Aku Aku hint line waits until the line being spoken has ended (the "
                    "original stops it mid-sentence); off = the original behaviour");

extern "C" REX_FUNC(__imp__sub_8229E838);

namespace {

// Game time (game_time.h): with sound on game frames (sound_out.h), "a voice
// is still playing" is decided on game time too, so the wait comes out the
// same on every run / PC.
using Clock = game_time::Clock;

constexpr uint32_t kInstanceList = 0x825A75A4;  // first instance node (instance +8), 0 = none
constexpr uint32_t kSpeechBusName = 0x825A7598;  // the speech bus's name hash (a word)
constexpr uint32_t kFirstBus = 0x82333EF0;       // (voice +108) -> its first bus, 0 = none
constexpr uint32_t kNextBus = 0x82333EF8;        // (voice +108, bus) -> the next bus, 0 = none
constexpr double kMaxWaitSeconds = 12.0;         // a hint never waits longer than this
constexpr int kMaxSteps = 4096;                  // a damaged list never loops forever

uint32_t R32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// A game function called with r3 / r4, the caller's registers kept.
uint32_t Call(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t r3, uint32_t r4 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  rex::runtime::ResolveIndirectFunction(address)(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Is a voice playing on the speech bus right now? (sub_822D71A0's walk,
// without its stop.)
bool SpeechBusBusy(PPCContext& ctx, uint8_t* base) {
  const uint32_t name = R32(base, kSpeechBusName);
  int steps = 0;
  for (uint32_t node = R32(base, kInstanceList); node && steps < kMaxSteps; node = R32(base, node), ++steps) {
    const uint32_t voice = R32(base, node + 0x24);  // instance +0x2C
    if (!voice) continue;
    for (uint32_t bus = Call(ctx, base, kFirstBus, R32(base, voice + 108)); bus && steps < kMaxSteps;
         bus = Call(ctx, base, kNextBus, R32(base, R32(base, node + 0x24) + 108), bus), ++steps) {
      if (R32(base, bus + 16) == name) return true;
    }
  }
  return false;
}

// Event object -> when it first had to wait (main thread only).
std::unordered_map<uint32_t, Clock::time_point> g_waiting;

}  // namespace

// CActionTutorialMessage::Update (r3 = the event, f1 = the frame's time):
// 1 = done, 0 = again next frame.
extern "C" REX_FUNC(sub_8229E838) {
  const uint32_t event = ctx.r3.u32;
  if (REXCVAR_GET(fix_speech_cut_off)) {
    // Waits left behind by events that were never asked again (a level
    // change destroyed them) are forgotten after a minute, so a new event
    // at the same address starts its own wait.
    std::erase_if(g_waiting, [](const auto& w) { return Clock::now() - w.second > std::chrono::seconds(60); });
    const auto it = g_waiting.find(event);
    const double waited =
        it == g_waiting.end() ? 0.0 : std::chrono::duration<double>(Clock::now() - it->second).count();
    if (waited < kMaxWaitSeconds && SpeechBusBusy(ctx, base)) {
      if (it == g_waiting.end()) {
        g_waiting.emplace(event, Clock::now());
        REXLOG_INFO("Speech: a hint line waits for the voice still speaking");
      }
      ctx.r3.u64 = 0;  // not done: asked again next frame
      return;
    }
    if (it != g_waiting.end()) {
      REXLOG_INFO("Speech: the hint line waited {:.1f} s{}", waited,
                  waited >= kMaxWaitSeconds ? " (the longest wait: the other voice is cut)" : "");
      g_waiting.erase(it);
    }
  }
  __imp__sub_8229E838(ctx, base);
}

