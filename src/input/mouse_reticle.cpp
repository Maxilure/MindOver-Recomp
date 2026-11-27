// =============================================================================
// input/mouse_reticle.cpp -- see mouse_reticle.h (findings/47)
// =============================================================================
#include <algorithm>
#include "mouse_reticle.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <chrono>
#include <cstring>

#include "../guest_memory.h"
#include "keyboard_mouse.h"
#include "players.h"
#include "prompt_icons.h"
#include "../players/more_players.h"
#include "../players/reticle_idle.h"
#include "../net/state_sync.h"
#include "../net/aim_sync.h"
#include "../ui/wii_cursor.h"

// DEFAULT 2 = Crosshair (2026-11-06, the port's out-of-the-box setting).
REXCVAR_DEFINE_INT32(mouse_reticle, 2, "CrashMoM",
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
float ReadFloat(const uint8_t* base, uint32_t a) {
  const uint32_t v = Read32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
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

// WHO AIMS: the mouse or the stick, whichever was used LAST. Player 1 is
// always on the keyboard AND may hold a controller too; without this the
// pointer's spot overwrote the stick's movement every frame, so a player 1
// who picked up a pad couldn't aim with its stick any more. The mouse's turn
// starts when the pointer moves (its counter goes up, checked every frame:
// player 1's reticle updates always, aiming or not, so a move made just
// before aiming counts at its time); the stick's when a controller is used on
// the keyboard's socket (prompt_icons PadActivity: buttons, triggers, sticks
// past the dead zone; the same signal hides the Wii cursor). steady_clock ticks.
int64_t g_mouse_used = 0;
uint32_t g_seen_moves = 0;
void NoteMouseMoves() {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float px = 0, py = 0;
  uint32_t moves = 0;
  if (!driver || !driver->Pointer(&px, &py, &moves) || moves == g_seen_moves) return;
  g_seen_moves = moves;
  g_mouse_used = std::chrono::steady_clock::now().time_since_epoch().count();
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
  // Online state-sync co-op: mouse aiming ONLY drives this PC's own local slot
  // (never Player 1 on a client's screen!). Remote players' reticles are driven
  // by their owner's networked aim coordinates below so their mask rotates and
  // shoots where they aim without showing their crosshair here.
  if (net::Active()) {
    const int local_slot = net::LocalSlot();
    if (local_slot < 0) return;
    if (int(player) != local_slot) {
      float rx = 0.0f, ry = 0.0f;
      if (net::GetRemoteReticle(int(player), rx, ry)) {
        static uint32_t net_pair = 0;
        if (!net_pair) net_pair = rex::system::kernel_memory()->SystemHeapAlloc(8);
        if (net_pair) {
          WriteFloat(base, net_pair, rx);
          WriteFloat(base, net_pair + 4, ry);
          CallGame(__imp__sub_82255360, ctx, base, reticle, net_pair);
          CallGame(__imp__sub_822EAA18, ctx, base, reticle + kSmoother, net_pair);
        }
      }
      return;
    }
  } else {
    const kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get();
    if (!players || !players->KeyboardAnswersFor(player)) return;
  }
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float px = 0, py = 0;
  uint32_t moves = 0;
  if (!driver || !driver->Pointer(&px, &py, &moves)) return;
  // WHO AIMS: the stick when a controller was used more recently than the
  // mouse; a mouse that never moved never aims. Online, the local player plays
  // on this PC's FIRST controller (state_sync copies it into their slot), so
  // that one's activity counts too (2026-11-27 playtest: the guest's player 2
  // could not aim with a controller: socket 2 never showed activity, and the
  // unmoved pointer, canvas (0, 0), took the crosshair every frame).
  if (g_mouse_used == 0) return;
  int64_t pad = prompt_icons::PadActivity(int(player));
  if (net::Active()) pad = std::max(pad, prompt_icons::PadActivity(0));
  if (pad > g_mouse_used) return;
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
  if (mouse_reticle::Read32(base, reticle + 44) == 0) {
    wii_cursor::SwitchGameCursors(ctx, base);
    mouse_reticle::NoteMouseMoves();
  }
  // Its spot before and after (stick + mouse): moved = its player aims
  // (players/reticle_idle.h hides a plain mask's reticle while they don't).
  const float x0 = mouse_reticle::ReadFloat(base, reticle + 0x14);
  const float y0 = mouse_reticle::ReadFloat(base, reticle + 0x28);
  __imp__sub_82255160(ctx, base);
  const float xg = mouse_reticle::ReadFloat(base, reticle + 0x14);
  const float yg = mouse_reticle::ReadFloat(base, reticle + 0x28);
  mouse_reticle::FollowPointer(ctx, base, reticle);
  const float x1 = mouse_reticle::ReadFloat(base, reticle + 0x14);
  const float y1 = mouse_reticle::ReadFloat(base, reticle + 0x28);
  const int32_t reticle_player = int32_t(mouse_reticle::Read32(base, reticle + 44));
  if (net::Active() && net::DebugTraceOn()) {  // online aim test trace (2026-11-27)
    static uint32_t n[8] = {};
    const bool on = (*GuestPtr(base, reticle + 48) & 0x80) != 0;
    if (on && (n[reticle_player & 7]++ % 30) == 0) {
      REXLOG_INFO("OnlineTrace: reticle {:08X} P{} on: game {:.1f},{:.1f} -> after ours {:.1f},{:.1f} (local slot P{})",
                  reticle, reticle_player + 1, xg, yg, x1, y1, net::LocalSlot() + 1);
    }
  }
  if (net::Active() && reticle_player == net::LocalSlot()) {
    net::SetLocalReticle(reticle_player, x1, y1);
  }
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const bool on = (*GuestPtr(base, reticle + 48) & 0x80) != 0;
  bool moved = dx * dx + dy * dy > 0.25f * 0.25f;
  // Online: another PC's player "aims" while their own PC runs their aim (it
  // sends its aim line then, net/aim_sync.h): their crosshair here showed only
  // while it MOVED and hid 2 s after they held still on a target, and a hidden
  // crosshair runs no aim = the copy stopped turning and shooting there
  // (2026-11-27 playtest: the guest's mask aim didn't show on the host).
  if (net::Active() && reticle_player != net::LocalSlot() && reticle_player >= 0) {
    moved = net::aim_sync::RemoteAimFresh(reticle_player);
  }
  reticle_idle::NoteReticleUpdate(ctx, base, reticle_player, on, moved);
}
