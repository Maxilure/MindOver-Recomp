// =============================================================================
// players/reticle_idle.cpp -- see reticle_idle.h (findings/26 s.57)
// =============================================================================
#include "reticle_idle.h"

#include <chrono>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "more_players.h"
#include "../input_record.h"

REXCVAR_DEFINE_INT32(reticle_hide_idle_ms, 2000, "CrashMoM",
                     "Co-op: a mask's aiming reticle shows only while its player aims (moves it or "
                     "shoots) and hides after this many ms without aiming; hidden at first. Only for "
                     "masks on a Crash on foot (a titan's keeps the original). 0 = always shown (original)")
    .range(0, 60000);

extern "C" REX_FUNC(__imp__sub_822AD0A0);  // the aim ray's 2D spot (r3 out x,y; r4 the aiming object)
extern "C" REX_FUNC(__imp__sub_8221D478);  // controller object -> its controller number
extern "C" REX_FUNC(sub_822661B8);
extern "C" REX_FUNC(__imp__sub_822AE1F8);  // animation player: play animation r4
extern "C" REX_FUNC(__imp__sub_822AE910);  // its restart         // player of controller r4 (r3 front end; 4 players: ours)

namespace reticle_idle {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPlayers = 4;
constexpr uint32_t kCoopState = 0x8259B11C;  // u32 per player: 1 = a mask (findings/26 s.8)
constexpr uint32_t kGameGlobal = 0x8259B190; // the game object (+52 the front end)

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Behaviour {
  uint32_t owner = 0;        // the host actor it was last updated for
  int player = -1;           // whose reticle spot its ray read (-1 = not known yet)
  bool hiding = false;       // switched off by us (lock out played / gone at once)
  uint8_t wanted_saved = 0;  // +292 before the Update (put back after it)
};
// CReticleAimingBehaviour: +264 shown flags (0x80 on, 0x40 closing = lock out
// playing), +292 wanted (bit 0x80, the scripts' sub_8213FF18 / sub_8213FFA8).
constexpr uint32_t kFlags = 264, kWanted = 292;
constexpr uint8_t kOn = 0x80, kClosing = 0x40;
std::unordered_map<uint32_t, Behaviour> g_behaviours;  // CReticleAimingBehaviour -> facts
uint32_t g_updating = 0;                               // behaviour inside its Update (0 = none)

// Per player: when they last aimed, and whether their reticle was on last frame.
Clock::time_point g_last_aimed[kPlayers] = {};
bool g_aimed_ever[kPlayers] = {};  // false = not since the reticle came on
bool g_was_on[kPlayers] = {};

// Shooting counts as aiming: the shot flies to the reticle, so it shows where
// it went. X, Y, the shoulders, a trigger past a quarter (XINPUT_GAMEPAD bits).
bool Shooting(int player) {
  uint8_t pad[12];
  if (!input_record::LastRead(uint32_t(player), pad)) return false;
  const uint16_t buttons = uint16_t(pad[0] << 8 | pad[1]);
  return (buttons & (0x4000 | 0x8000 | 0x0100 | 0x0200)) || pad[2] > 64 || pad[3] > 64;
}

}  // namespace

// The reticle's own switch (sub_822ACD78, r3 = behaviour + 28, r4 on, r5 at
// once) does, on the behaviour: on = flags bit 0x80 + animation 0 (LOCK IN);
// off = bit 0x40 "closing" + animation 2 (LOCK OUT: drawn until the game's
// aim update sub_822AC668 sees the animation done (state 3) and clears 0x40);
// at once = both bits off + animation 4. The animation player is behaviour
// +60 (sub_822AE1F8(it, animation)), restarted by sub_822AE910(behaviour +92).
// It also writes the player's 2D reticle's "on" bit: the port's switch below
// copies only the behaviour part, so a hidden reticle's spot keeps moving and
// moving it brings the reticle back.
void PlayAnimation(PPCContext& ctx, uint8_t* base, uint32_t behaviour, uint32_t animation,
                   bool restart) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = behaviour + 60;
  ctx.r4.u64 = animation;
  __imp__sub_822AE1F8(ctx, base);
  if (restart) {
    ctx.r3.u64 = behaviour + 92;
    __imp__sub_822AE910(ctx, base);
  }
  ctx = saved;
}

void BeginBehaviourUpdate(PPCContext& ctx, uint8_t* base, uint32_t behaviour, uint32_t owner) {
  g_updating = behaviour;
  Behaviour& b = g_behaviours[behaviour];
  b.owner = owner;
  uint8_t* flags = Guest(behaviour + kFlags);
  uint8_t* wanted = Guest(behaviour + kWanted);
  const bool want = (*wanted & kOn) != 0;
  const bool hide = want && Hidden(behaviour);
  if (hide && !b.hiding) {
    b.hiding = true;
    if (*flags & kOn) {
      if (b.player < 0 || !g_aimed_ever[b.player]) {
        // Just became a mask, never aimed: gone at once (no lock in + out).
        *flags &= 0x3F;
        PlayAnimation(ctx, base, behaviour, 4, false);
      } else {
        // Idle: the reticle's own LOCK OUT.
        *flags = uint8_t((*flags & 0x3F) | kClosing);
        PlayAnimation(ctx, base, behaviour, 2, true);
      }
    }
  } else if (b.hiding && !hide) {
    b.hiding = false;
    // Aiming again (even halfway through its lock out): its LOCK IN, as the
    // game's own switch does when the reticle comes on.
    if (want) {
      *flags = uint8_t((*flags & 0x3F) | kOn);
      PlayAnimation(ctx, base, behaviour, 0, true);
    }
  }
  // While hidden, the game's Update compares the WANTED state (+292, set by
  // the scripts) with the shown one (+264) and would switch it back on: it
  // sees "off" wanted for the call (put back right after).
  b.wanted_saved = *wanted;
  if (b.hiding) *wanted &= uint8_t(~kOn);
}
void EndBehaviourUpdate() {
  const uint32_t behaviour = g_updating;
  g_updating = 0;
  const auto it = g_behaviours.find(behaviour);
  if (it == g_behaviours.end() || !it->second.hiding) return;
  uint8_t* wanted = Guest(behaviour + kWanted);
  *wanted = uint8_t((*wanted & ~kOn) | (it->second.wanted_saved & kOn));
}

bool SkipDraw(uint32_t behaviour) {
  const auto it = g_behaviours.find(behaviour);
  // Ours to hide: drawn only during its lock out. Not switched yet (its first
  // frames as a mask, before the Update knows its player): not drawn.
  if (it != g_behaviours.end() && it->second.hiding) return !(*Guest(behaviour + kFlags) & kClosing);
  return Hidden(behaviour);
}
void Forget(uint32_t behaviour) { g_behaviours.erase(behaviour); }

void NoteReticleUpdate(PPCContext&, uint8_t*, int player, bool on, bool moved) {
  if (player < 0 || player >= kPlayers) return;
  if (on && !g_was_on[player]) g_aimed_ever[player] = false;  // just came on: hidden until used
  g_was_on[player] = on;
  if (!on) return;
  if (moved || Shooting(player)) {
    g_last_aimed[player] = Clock::now();
    g_aimed_ever[player] = true;
  }
}

bool Hidden(uint32_t behaviour) {
  const int32_t idle_ms = REXCVAR_GET(reticle_hide_idle_ms);
  if (idle_ms <= 0) return false;
  const auto it = g_behaviours.find(behaviour);
  if (it == g_behaviours.end()) return false;
  const int p = it->second.player;
  if (p < 0 || p >= kPlayers) return false;
  // Only a plain mask: its player is a mask (co-op state 1) and the reticle's
  // host is not a titan (then it aims the mutant's abilities: original).
  if (Read32(kCoopState + 4 * uint32_t(p)) != 1) return false;
  const uint32_t owner = it->second.owner;
  for (int q = 0; q < more_players::LocalPlayerCount() && q < kPlayers; ++q)
    if (owner && more_players::TitanOfPlayer(q) == owner) return false;
  if (!g_aimed_ever[p]) return true;
  return Clock::now() - g_last_aimed[p] > std::chrono::milliseconds(idle_ms);
}

}  // namespace reticle_idle

// The aim ray's 2D spot (r3 = out x,y; r4 = the aiming object, called from the
// behaviour's Update): the original reads the front end's reticle of the
// player behind the object's controller. The same lookup (its first lines:
// *(*(r4+16)) -> +36 controller object (-> number) or +32 a number ->
// sub_822661B8) names the behaviour's player for the draw rule.
extern "C" REX_FUNC(sub_822AD0A0) {
  const uint32_t object = ctx.r4.u32;
  __imp__sub_822AD0A0(ctx, base);
  const uint32_t behaviour = reticle_idle::g_updating;
  if (!behaviour) return;
  const uint32_t list = reticle_idle::Read32(object + 16);
  const uint32_t first = list ? reticle_idle::Read32(list) : 0;
  if (!first) return;
  const PPCContext saved = ctx;
  uint32_t controller;
  if (const uint32_t object36 = reticle_idle::Read32(first + 36)) {
    ctx.r3.u64 = object36;
    __imp__sub_8221D478(ctx, base);
    controller = ctx.r3.u32;
  } else {
    controller = reticle_idle::Read32(first + 32);
  }
  ctx.r3.u64 = reticle_idle::Read32(reticle_idle::Read32(reticle_idle::kGameGlobal) + 52);
  ctx.r4.u64 = controller;
  sub_822661B8(ctx, base);
  const int player = int32_t(ctx.r3.u32);
  ctx = saved;
  reticle_idle::Behaviour& b = reticle_idle::g_behaviours[behaviour];
  if (b.player != player) {
    REXLOG_DEBUG("Reticle: aiming behaviour {:08X} aims for player {}", behaviour, player + 1);
    b.player = player;
  }
}
