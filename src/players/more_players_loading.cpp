// =============================================================================
// players/more_players_loading.cpp -- the loading screen's paw prints for
// players 3 and 4 (findings/26 s.28)
// =============================================================================
//
// WHAT THE GAME DOES (found 2026-10-06, extends findings/26 s.14):
//   The loading screen (CLoadingScreen, vtable 0x8203B0CC, the front end's
//   element at +304; page GameLoad_Loading) draws trails of paw prints that
//   walk around the screen while a level loads, ONE TRAIL PER PLAYER WHO HAS
//   A CONTROLLER (the front end's "controller of player p", sub_82266130,
//   >= 0): player 2's trail appears once player 2 has joined. Each player
//   steers their own trail with the left stick. Both trails live INSIDE the
//   object, 548 bytes each:
//
//     +4      byte, bit 0x80 = active (set by slot 1, cleared by slot 2)
//     +8/+12/+16  the page, its root group, the paw picture (FE_Footprint)
//     +20 + 548 * t   trail t (t = 0, 1):
//       +0 .. +519      10 prints x 52 bytes (print +48 = its alpha, 0 = gone)
//       +520            index of the newest print (0-9, a ring)
//       +524 .. +544    position / heading / step data
//     +1116 / +1120   step timer (wraps every step) / time shown (page fade)
//
//   Update = slot 3 sub_8226B2C8 (f1 = frame time): asks players 0-1 for a
//   controller, steers each present player's trail, and on a step (the
//   +1116 timer wrapped) puts down that trail's next print (and plays a
//   footstep). With NO player present trail 0 wanders by itself, steered
//   by any pad. Draw = slot 4 sub_8226BDA0: the page, then every print of
//   every present player's trail through sub_8225F108 (this, print, the
//   left / right foot's 4 UV corners, ARGB colour, picture). Colours:
//   alone = white (0x825078E8); two players = player 1 0x82507920 (orange
//   0xC8501E), player 2 0x82507924 (blue 0x3CAFE1), alpha from the print.
//   Reset = sub_8226BF90 (called as a level starts loading).
//
// WHAT THIS MODULE DOES (only when player 3 or 4 has a controller, so with
// two players nothing changes):
//   * A SHADOW loading screen of our own (guest heap): its two trails are
//     players 3 and 4. After the real update runs, the game's own update
//     runs again on the shadow with the same step timing and the front
//     end's controller lookup shifted by 2 (more_players::
//     ShiftControllerLookup): players 3 and 4 steer their own paws.
//   * After the real draw, the shadow's prints are drawn with the same
//     picture and foot UVs, in the players' MARKER colours (the colour of
//     the banner over their heads: --marker_colour_player3 green,
//     --marker_colour_player4 purple, more_players_frontend.cpp).
//   * Player 1's trail takes its two-player colour (orange) as soon as
//     anyone else plays, not only player 2: the real draw counts players
//     1-2 only, and alone it's white.
//   * The shadow is reset with the real one.
// =============================================================================
#include "more_players.h"

#include <atomic>
#include <chrono>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(uint32_t, marker_colour_player3);
REXCVAR_DECLARE(uint32_t, marker_colour_player4);
REXCVAR_DECLARE(bool, debug_coop_trace);

extern "C" REX_FUNC(__imp__sub_8226B2C8);  // CLoadingScreen update (r3 this, f1 frame time)
extern "C" REX_FUNC(__imp__sub_8226BDA0);  // CLoadingScreen draw (r3 this)
extern "C" REX_FUNC(__imp__sub_8226BF90);  // CLoadingScreen reset of the trails (r3 this)
extern "C" REX_FUNC(__imp__sub_8225F108);  // front-end element: draw a sprite quad
extern "C" REX_FUNC(sub_82266130);         // front end: controller of player r4 (ours: four)

namespace more_players_loading {
namespace {

constexpr uint32_t kLoadingScreenVtable = 0x8203B0CC;
constexpr uint32_t kObjectSize = 1124;      // the reset writes up to +1120
constexpr uint32_t kActive = 4;             // byte, bit 0x80
constexpr uint32_t kPicture = 16;           // the paw picture (draw's r7)
constexpr uint32_t kTrails = 20;            // first trail
constexpr uint32_t kTrailSize = 548;
constexpr uint32_t kPrintSize = 52;
constexpr int kPrints = 10;
constexpr uint32_t kPrintAlpha = 48;
constexpr uint32_t kStepTimer = 1116, kShownTime = 1120;
constexpr uint32_t kTwoPlayerColours = 0x82507920;  // ARGB: player 1, player 2

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
float ReadFloat(uint32_t a) {
  const uint32_t v = Read32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteFloat(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Write32(a, v);
}
uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0,
                  uint32_t r7 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6; ctx.r7.u64 = r7;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

uint32_t g_shadow = 0;  // our loading screen: trail 0 = player 3, trail 1 = player 4
uint32_t g_uvs = 0;     // the draw's two foot UV sets (2 x 4 corners x (u, v) floats)
bool g_logged[2] = {};  // --debug_coop_trace: "paws shown" once per player per load

uint32_t FrontEnd() { return Read32(Read32(0x8259B190) + 52); }

// Does player p (0-3) have a controller, i.e. a trail? (The game's own test.)
bool HasController(PPCContext& ctx, uint8_t* base, int p) {
  return int32_t(CallGame(sub_82266130, ctx, base, FrontEnd(), uint32_t(p))) >= 0;
}
bool ExtraPlayersPresent(PPCContext& ctx, uint8_t* base) {
  if (more_players::LocalPlayerCount() <= 2) return false;
  return HasController(ctx, base, 2) || HasController(ctx, base, 3);
}

// The marker colour (0xAARRGGBB) of player 3 / 4 (p = 2, 3).
uint32_t MarkerColour(int p) {
  return p == 2 ? REXCVAR_GET(marker_colour_player3) : REXCVAR_GET(marker_colour_player4);
}

// Our object, made once (lives as long as the game: the front end's loading
// screen does too). Only the fields the update / reset / our draw use matter;
// the page pointers (+8/+12/+16) stay 0: the update never reads them and the
// draw of the shadow is ours (the picture is the real screen's).
uint32_t Shadow(PPCContext& ctx, uint8_t* base) {
  if (g_shadow) return g_shadow;
  auto* memory = rex::system::kernel_memory();
  g_shadow = memory->SystemHeapAlloc(kObjectSize);
  std::memset(Guest(g_shadow), 0, kObjectSize);
  Write32(g_shadow, kLoadingScreenVtable);
  CallGame(__imp__sub_8226BF90, ctx, base, g_shadow);
  // The draw builds these on its stack (0x8226BE0C..): the corners' (u, v)
  // from 1 (constant 0x8201EE28) and 0 (0x82046720); set 1 is set 0
  // mirrored (left foot / right foot).
  g_uvs = memory->SystemHeapAlloc(64);
  const float one = ReadFloat(0x8201EE28), zero = ReadFloat(0x82046720);
  const float uv[16] = {one, zero, one, one, zero, zero, zero, one,   // set 0 (r1+112)
                        zero, zero, zero, one, one, zero, one, one};  // set 1 (r1+144)
  for (int i = 0; i < 16; ++i) WriteFloat(g_uvs + 4 * i, uv[i]);
  return g_shadow;
}

}  // namespace
}  // namespace more_players_loading

using namespace more_players_loading;

// Update: the real one, then players 3-4's trails on the shadow with the
// same step timing (the timer is copied from before the real update ran, so
// both take their steps on the same frame).
extern "C" REX_FUNC(sub_8226B2C8) {
  const uint32_t screen = ctx.r3.u32;
  const double frame_time = ctx.f1.f64;
  const float step_timer = ReadFloat(screen + kStepTimer);
  const float shown_time = ReadFloat(screen + kShownTime);
  __imp__sub_8226B2C8(ctx, base);
  if (!(*Guest(screen + kActive) & 0x80) || !ExtraPlayersPresent(ctx, base)) return;
  const uint32_t shadow = Shadow(ctx, base);
  *Guest(shadow + kActive) |= 0x80;
  WriteFloat(shadow + kStepTimer, step_timer);
  WriteFloat(shadow + kShownTime, shown_time);
  const PPCContext saved = ctx;
  ctx.r3.u64 = shadow;
  ctx.f1.f64 = frame_time;
  more_players::ShiftControllerLookup(2);  // "player 0 / 1" = player 3 / 4
  __imp__sub_8226B2C8(ctx, base);
  more_players::ShiftControllerLookup(0);
  ctx = saved;
}

// When the loading screen was last drawn (steady clock, ms): Loading().
static std::atomic<int64_t> g_loading_drawn_ms{-1000000};
static int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool more_players::Loading() { return SteadyMs() - g_loading_drawn_ms.load() < 250; }

// Draw: the real one (page + players 1-2's paws), then players 3-4's.
extern "C" REX_FUNC(sub_8226BDA0) {
  const uint32_t screen = ctx.r3.u32;
  g_loading_drawn_ms = SteadyMs();
  __imp__sub_8226BDA0(ctx, base);
  if (!g_shadow || !ExtraPlayersPresent(ctx, base)) return;
  const float alpha_scale = ReadFloat(0x8201F94C);  // 255 (the draw's f30)
  for (int t = 0; t < 2; ++t) {
    const int p = 2 + t;
    if (!HasController(ctx, base, p)) continue;
    if (REXCVAR_GET(debug_coop_trace) && !g_logged[t]) {
      g_logged[t] = true;
      REXLOG_INFO("Co-op: loading screen: player {}'s paw prints shown", p + 1);
    }
    for (int j = 0; j < kPrints; ++j) {
      const uint32_t print = g_shadow + kTrails + kTrailSize * t + kPrintSize * j;
      const float alpha = ReadFloat(print + kPrintAlpha);
      if (!(alpha > 0.0f)) continue;
      // As the original: alpha byte = (int)(alpha x 255), feet alternate UV sets.
      const uint32_t a = uint32_t(int32_t(alpha * alpha_scale)) & 0xFF;
      const uint32_t colour = a << 24 | (MarkerColour(p) & 0x00FFFFFF);
      CallGame(__imp__sub_8225F108, ctx, base, screen, print, g_uvs + 32 * ((j + 1) % 2),
               colour, Read32(screen + kPicture));
    }
  }
}

// Reset (a new load starts): the shadow too.
extern "C" REX_FUNC(sub_8226BF90) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_8226BF90(ctx, base);
  if (g_shadow && screen != g_shadow) {
    CallGame(__imp__sub_8226BF90, ctx, base, g_shadow);
    g_logged[0] = g_logged[1] = false;
  }
}

// A paw drawn by the REAL loading screen: alone, the original draws player
// 1's trail white, and it counts players 1-2 only. With player 3 or 4 in,
// player 1 isn't alone: take the two-player colour of the trail's player.
extern "C" REX_FUNC(sub_8225F108) {
  const uint32_t element = ctx.r3.u32, print = ctx.r4.u32;
  if (Read32(element) == kLoadingScreenVtable && (ctx.r6.u32 & 0x00FFFFFF) == 0x00FFFFFF &&
      print >= element + kTrails && print < element + kTrails + 2 * kTrailSize &&
      ExtraPlayersPresent(ctx, base)) {
    const uint32_t t = (print - element - kTrails) / kTrailSize;
    ctx.r6.u64 = (ctx.r6.u32 & 0xFF000000) | (Read32(kTwoPlayerColours + 4 * t) & 0x00FFFFFF);
  }
  __imp__sub_8225F108(ctx, base);
}
