// =============================================================================
// input/mouse_reticle.cpp -- see mouse_reticle.h (findings/47)
// =============================================================================
#include "mouse_reticle.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstring>

#include "../guest_memory.h"
#include "keyboard_mouse.h"
#include "players.h"
#include "../players/more_players.h"
#include "../ui/wii_cursor.h"

REXCVAR_DEFINE_INT32(mouse_reticle, 1, "CrashMoM",
                     "Aiming reticle on keyboard and mouse (a masked player aiming): 0 = moves with "
                     "WASD (original), 1 = follows the mouse, 2 = follows the mouse + the gameplay "
                     "cursor (the Wii version's pointer: a bubble that follows the mouse in play, ui/wii_cursor.h)")
    .range(0, 2);

extern "C" REX_FUNC(__imp__sub_82255160);  // CReticleController: Update (r3 it, f1 dt)
extern "C" REX_FUNC(__imp__sub_82255360);  // CReticleController: clamp a target (r3 it, r4 &x,y)
extern "C" REX_FUNC(__imp__sub_822EAA18);  // 2-axis smoother: set (r3 it, r4 &x,y), no easing

namespace mouse_reticle {
namespace {

constexpr int kMaxPlayers = 4;
constexpr uint32_t kGameGlobal = 0x8259B190;   // the game object
constexpr uint32_t kPausedByte = 168;          // GAME +168 bit 0x20: paused (the Update's own test, r31 = the game)
constexpr uint8_t kPaused = 0x20;
// CReticleController: +4 the shown position (2-axis smoother), +44 its
// player (0-3), +48 bit 0x80 = on (a masked player aims; the scripts set it
// through sub_8213FF18 / sub_8213FFA8).
constexpr uint32_t kSmoother = 4, kPlayer = 44, kFlags = 48;
constexpr uint8_t kOn = 0x80;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void WriteFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

using GuestFunction = void (*)(PPCContext&, uint8_t*);
void CallGame(GuestFunction f, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4;
  f(ctx, base);
  ctx = saved;
}

// After the game's Update of `reticle`: on the pointer, if its player is on
// the keyboard and the reticle is on.
void FollowPointer(PPCContext& ctx, uint8_t* base, uint32_t reticle) {
  if (Mode() == 0) return;
  const uint32_t player = Read32(base, reticle + kPlayer);
  if (player >= uint32_t(kMaxPlayers)) return;
  if (!(*GuestPtr(base, reticle + kFlags) & kOn)) return;
  // Paused: the game's own test (game +168; a first version read the FRONT
  // END's +168 and kept the pointer hidden in the pause menu), and any of the
  // game's menus on screen (they need the pointer).
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game || (*GuestPtr(base, game + kPausedByte) & kPaused)) return;
  if (menu_input::MenuOnScreen()) return;
  const kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get();
  if (!players || !players->KeyboardAnswersFor(player)) return;
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float px = 0, py = 0;
  uint32_t moves = 0;
  if (!driver || !driver->Pointer(&px, &py, &moves)) return;
  // The pointer (0..1 over the 16:9 picture) on the 640 x 480 canvas drawn
  // 4:3 in its middle (x = 160 + 1.5 cx of 1280, y = 720 - 1.5 cy of 720;
  // the same mapping as the menu mouse, mouse_menus.h).
  const float cx = (px * 1280.0f - 160.0f) / 1.5f;
  const float cy = (1.0f - py) * 480.0f;
  // Guest room for the x, y pair (8 bytes, made once).
  static uint32_t pair = 0;
  if (!pair) pair = rex::system::kernel_memory()->SystemHeapAlloc(8);
  if (!pair) return;
  WriteFloat(base, pair, cx);
  WriteFloat(base, pair + 4, cy);
  CallGame(__imp__sub_82255360, ctx, base, reticle, pair);              // the game's clamp
  CallGame(__imp__sub_822EAA18, ctx, base, reticle + kSmoother, pair);  // target = shown = there
  // The reticle IS the pointer now: the system one hides (back ~0.2 s after
  // aiming ends, so menus have it).
  driver->HidePointerWhileAiming();
}

}  // namespace

int Mode() { return REXCVAR_GET(mouse_reticle); }

}  // namespace mouse_reticle

// CReticleController's Update (sub_82255160): the game's stick movement,
// then the mouse's spot for a keyboard player (mouse_reticle.h).
extern "C" REX_FUNC(sub_82255160) {
  const uint32_t reticle = ctx.r3.u32;
  // Once per frame (player 1's reticle always exists and updates): the
  // game's own Wii cursor on / off for crosshair mode (ui/wii_cursor.h).
  if (mouse_reticle::Read32(base, reticle + 44) == 0) wii_cursor::SwitchGameCursors(ctx, base);
  __imp__sub_82255160(ctx, base);
  mouse_reticle::FollowPointer(ctx, base, reticle);
}
