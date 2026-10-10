// =============================================================================
// players/who_plays.cpp -- see who_plays.h
// =============================================================================
// Each override says what the 360 version did and what the PC version does.
// Offsets are from the game's central object, *(0x8259B190) (who_plays.h).
#include "who_plays.h"

#include <atomic>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../input/players.h"

// The game's own functions the PC versions still call.
extern "C" REX_FUNC(sub_82258E20);  // save handler: a new main player (r3 handler, r4 0)
extern "C" REX_FUNC(sub_82266398);  // front end: the player defaults (ours, below)
extern "C" REX_FUNC(sub_82270608);  // the game object (difficulty at +240)
extern "C" REX_FUNC(sub_8235ADD0);  // wait for an event object (r3 -> handle)
extern "C" REX_FUNC(sub_823002E0);  // XShowDirtyDiscErrorUI wrapper (r3 user)

namespace who_plays {
namespace {

constexpr uint32_t kUber = 0x8259B190;  // -> the game's central object

// Its fields (who_plays.h).
constexpr uint32_t kMainPlayer = 352;     // the main player's controller, -1 = none
constexpr uint32_t kMainPlayerWas = 356;  // its previous value
constexpr uint32_t kPlayerCount = 360;    // players with a controller
constexpr uint32_t kPlayerMask = 364;     // bit N = player N has one
constexpr uint32_t kMainController = 372; // the input manager's controller of the main player

// The front end's player defaults (front end = *(uber + 52)).
constexpr uint32_t kInvertAxis = 8588;   // 4 bytes: each player's Invert Axis (1 = on)
constexpr uint32_t kFrontEndFlags = 8593; // bit 7 = vibration on
constexpr uint32_t kDifficulty = 240;    // game object: the default difficulty (1 = Normal)

// "The players changed": starts raised, so the fields are filled once at boot
// (the 360's system announced its signed-in profile then).
std::atomic<bool> g_changed{true};
// The front end whose Invert Axis bytes were set up (once per front end).
uint32_t g_invert_set_up_for = 0;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// Bit N = player N (0-3) has a controller. Player 1 always counts: someone
// is at the PC (keyboard or pad; the Players tab can't leave player 1 empty
// for the game's purposes).
uint32_t PlayerMask() {
  uint32_t mask = 1;
  if (const kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get()) {
    for (int p = 1; p < 4; ++p) {
      if (players->HasDeviceFor(p)) mask |= 1u << p;
    }
  }
  return mask;
}

// Fills +360/+364 and returns the mask.
uint32_t StorePlayers(uint8_t* base, uint32_t game) {
  const uint32_t mask = PlayerMask();
  Store32(base, game + kPlayerCount, uint32_t(__builtin_popcount(mask)));
  Store32(base, game + kPlayerMask, mask);
  return mask;
}

// The player number of the input manager's controller `controller`: its
// device's port (virtual slot 7, the same question the 360 version asked).
// -1 if there's no device or it isn't playing. Guest call: `ctx` restored.
uint32_t PlayerOfController(PPCContext& ctx, uint8_t* base, uint32_t controller, uint32_t mask) {
  const uint32_t uber = Load32(base, kUber);
  const uint32_t controllers = Load32(base, Load32(base, uber + 56) + 8);
  const uint32_t device = Load32(base, Load32(base, controllers + 4 * controller) + 20);
  if (!device) return 0xFFFFFFFFu;
  PPCFunc* port_of = rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, device) + 28));
  if (!port_of) return 0xFFFFFFFFu;
  const PPCContext saved = ctx;
  ctx.r3.u64 = device;
  port_of(ctx, base);
  const uint32_t player = ctx.r3.u32;
  ctx = saved;
  return player < 4 && (mask & (1u << player)) ? player : 0xFFFFFFFFu;
}

void LogPlayers(uint32_t mask) {
  REXLOG_INFO("Players: playing now: {}{}{}{}", mask & 1 ? "1 " : "", mask & 2 ? "2 " : "",
              mask & 4 ? "3 " : "", mask & 8 ? "4" : "");
}

}  // namespace

void PlayersChanged() {
  g_changed.store(true);
}

}  // namespace who_plays

using namespace who_plays;

// -----------------------------------------------------------------------------
// A controller starts playing (r3 = the central object, r4 = the input
// manager's controller): Press START at the title, joining a game, the quick
// load (saves/quick_load.cpp). 360 (0x8227CEA8): the signed-in users, the
// controller's user (-1 = not signed in), the profile name's hash; then the
// save handler and the front end hear about the new main player (the latter
// applies the profile). PC: the players with a controller, the controller's
// player number; the same two calls (the front end's = PC defaults, below).
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_8227CEA8) {
  const uint32_t game = ctx.r3.u32, controller = ctx.r4.u32;
  const uint32_t uber = Load32(base, kUber);
  const uint32_t controllers = Load32(base, Load32(base, uber + 56) + 8);
  if (!Load32(base, Load32(base, controllers + 4 * controller) + 20)) {
    return;  // no device behind it (the 360 version stopped here too)
  }
  const uint32_t mask = StorePlayers(base, game);
  const uint32_t player = PlayerOfController(ctx, base, controller, mask);
  Store32(base, game + kMainPlayer, player);
  Store32(base, game + kMainPlayerWas, player);
  Store32(base, game + kMainController, controller);
  REXLOG_INFO("Players: controller {} plays as player {}", controller, int32_t(player) + 1);
  LogPlayers(mask);

  const PPCContext saved = ctx;
  ctx.r3.u64 = Load32(base, Load32(base, uber + 80) + 124);  // the save handler
  ctx.r4.u64 = 0;
  sub_82258E20(ctx, base);
  ctx.r3.u64 = Load32(base, uber + 52);  // the front end
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 1;
  sub_82266398(ctx, base);
  ctx = saved;
}

// -----------------------------------------------------------------------------
// Every frame (r3 = the central object). 360 (0x8227B1D8): the next system
// notification: "sign-in changed" re-read the users and the main player's
// profile (a different name = "the profile changed": back to the title);
// "guide open/closed" paused the game. PC: when a controller was added,
// removed or moved in the Players tab, the players and the main player's
// number are filled in again. Nothing else can change.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_8227B1D8) {
  if (!g_changed.exchange(false)) {
    return;
  }
  const uint32_t game = ctx.r3.u32;
  const uint32_t mask = StorePlayers(base, game);
  LogPlayers(mask);
  const uint32_t controller = Load32(base, game + kMainController);
  if (controller == 0xFFFFFFFFu) {
    return;  // nobody has pressed START yet
  }
  const uint32_t player = PlayerOfController(ctx, base, controller, mask);
  Store32(base, game + kMainPlayer, player);
  Store32(base, game + kMainPlayerWas, player);
}

// The 360 version made the system notification listener (+340). PC: none.
extern "C" REX_FUNC(sub_8227CE70) {
  Store32(base, ctx.r3.u32 + 340, 0);
}

// -----------------------------------------------------------------------------
// The player defaults (r3 = the front end, r4 = set Invert Axis (and, with
// r5, the default difficulty)). 360 (0x82266398): read the main player's
// profile settings and set the default difficulty (game object +240),
// each player's Invert Axis (+8588..8591) and vibration (+8593 bit 7); with
// no profile: Normal, off, on. PC: the same defaults (what the 360 gave a
// fresh profile too), except Invert Axis: set off once for this front end,
// then it stays the player's choice (Options -> Controls) instead of being
// reset at every Press START, quit to menu and join.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82266398) {
  const uint32_t fe = ctx.r3.u32;
  const bool players = (ctx.r4.u32 & 0xFF) != 0;
  const bool difficulty = (ctx.r5.u32 & 0xFF) != 0;
  // Like the original: with no main player yet (the front end's set-up at
  // boot) the default is set whatever r4/r5 say; later only when asked.
  // (Found the hard way: leaving +240 at 0 until the first Press START made
  // the quick load's level set-up ask the loader for "levels/L0/(null)" and
  // crash.)
  const uint32_t uber = Load32(base, kUber);
  const bool no_main_player = Load32(base, uber + kMainPlayer) == 0xFFFFFFFFu;
  if (no_main_player || (players && difficulty)) {
    const PPCContext saved = ctx;
    sub_82270608(ctx, base);
    Store32(base, ctx.r3.u32 + kDifficulty, 1);  // Normal
    ctx = saved;
  }
  if ((no_main_player || players) && g_invert_set_up_for != fe) {
    g_invert_set_up_for = fe;
    std::memset(GuestPtr(base, fe + kInvertAxis), 0, 4);
  }
  *GuestPtr(base, fe + kFrontEndFlags) |= 0x80;  // vibration on
}

// -----------------------------------------------------------------------------
// The front end's set-up (sub_8225F8A0) asks how big the profile settings
// are (r7 -> size, r8 no buffer yet), allocates that and keeps it at +8744.
// 360 (XUserReadProfileSettings, 0x82322960): 8 + 40 per setting. PC: no
// settings to read: 8 bytes (an empty result), never filled or read.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82322960) {
  const uint32_t size_ptr = ctx.r7.u32;
  if (size_ptr && Load32(base, size_ptr) == 0) Store32(base, size_ptr, 8);
  ctx.r3.u64 = 0x7A;  // ERROR_INSUFFICIENT_BUFFER: "here's the size"
}

// -----------------------------------------------------------------------------
// Radical's error thread (r3 = it; 0x8235B400): waits for an error (the
// event at +104), then, unless +100 says not to, shows the system's disc
// error box. 360: first loops on the sign-in pop-up until somebody is signed
// in (the box needs a user). PC: no sign-in; the box for player 1.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_8235B400) {
  const uint32_t thread = ctx.r3.u32;
  const PPCContext saved = ctx;
  ctx.r3.u64 = thread + 104;
  sub_8235ADD0(ctx, base);
  if (*GuestPtr(base, thread + 100) == 0) {
    REXLOG_ERROR("Players: the game reported a disc read error");
    ctx.r3.u64 = 0;
    sub_823002E0(ctx, base);
  }
  ctx = saved;
  ctx.r3.u64 = 0;
}

