// =============================================================================
// players/more_players_frontend.cpp -- the front end's per-player objects for
// players 3 and 4 (findings/26 s.19)
// =============================================================================
//
// WHAT: the front-end manager (game +52) keeps five kinds of per-player HUD
// objects in arrays of TWO, built by its constructor sub_8225F8A0:
//
//   +1720  32 B  CPlayerIdentifierArrow  the "1" / "2" marker over a player
//   +1852  3152  CComboCounter           the combo meter (+3148 its player)
//   +8156  92 B  CPlayerLockOnArrow      the lock-on arrow
//   +8340  40 B  CCounterOpportunityDisplay  "counter now" prompt (+8 player)
//   +8420  52 B  CReticleController      the aiming reticle (+44 its player)
//
// They can't grow in place (the front end's other fields follow). For players
// 3-4 this module makes two more of four of them (not the markers: their group
// update / draw walk the state table and shared animation globals for exactly
// two; they come with the marker art), and:
//   * runs every front-end loop over them too: set-up (sub_82261330), update
//     (sub_82261C40), slot 2 (sub_82261F90), draw (sub_82263328), the
//     reticles' update (sub_82265248), the destructor (sub_8225FBD8);
//   * points every "object of player p" lookup at them for p = 2-3: the combo
//     meter's script helpers and Crash's collect, the counter prompt
//     (sub_822664F8), the lock-on arrow (sub_82262150), the reticle (by
//     controller: sub_822661B8 rewritten for four; by character: three
//     functions that compared against players 1-2 only, sub_8213FF18 /
//     sub_8213FFA8 / sub_82140038).
// Found 2026-10-04 while building players 3-4's HUD: masked players 3-4 could
// shoot but not move the reticle (no reticle controller), and their combos
// went nowhere (the combo meter of "player 3" was other front end fields).
// =============================================================================
#include "more_players.h"
#include "more_players_hud.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, debug_coop_trace);

extern "C" REX_FUNC(__imp__sub_8225CA98);  // CComboCounter constructor
extern "C" REX_FUNC(__imp__sub_8226DC68);  // CPlayerLockOnArrow constructor
extern "C" REX_FUNC(__imp__sub_8225E320);  // CCounterOpportunityDisplay constructor
extern "C" REX_FUNC(__imp__sub_822550D0);  // CReticleController constructor
extern "C" REX_FUNC(__imp__sub_8225CBE8);  // CComboCounter destructor
extern "C" REX_FUNC(__imp__sub_8226DD50);  // CPlayerLockOnArrow destructor
extern "C" REX_FUNC(__imp__sub_8225E3D8);  // CCounterOpportunityDisplay destructor
extern "C" REX_FUNC(__imp__sub_820FB518);  // lock-on arrow: its player (r4)
extern "C" REX_FUNC(__imp__sub_8226CE20);  // CPlayerIdentifierArrow constructor
extern "C" REX_FUNC(__imp__sub_8226CEC0);  // CPlayerIdentifierArrow destructor

namespace more_players_frontend {
namespace {

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
uint32_t CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, int slot, uint32_t r4 = 0,
                     double f1 = 0) {
  PPCFunc* function = rex::runtime::ResolveIndirectFunction(Read32(Read32(object) + 4 * slot));
  if (!function) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = object; ctx.r4.u64 = r4; ctx.f1.f64 = f1;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// The kinds handled here (not the markers, see the header).
enum Kind { kCombo, kLockOn, kCounter, kReticle, kMarker, kKinds };
struct KindInfo {
  uint32_t offset, size;
  void (*constructor)(PPCContext&, uint8_t*);
  void (*destructor)(PPCContext&, uint8_t*);  // null = none (the reticle's is trivial)
};
const KindInfo kInfo[kKinds] = {
    {1852, 3152, __imp__sub_8225CA98, __imp__sub_8225CBE8},
    {8156, 92, __imp__sub_8226DC68, __imp__sub_8226DD50},
    {8340, 40, __imp__sub_8225E320, __imp__sub_8225E3D8},
    {8420, 52, __imp__sub_822550D0, nullptr},
    {1720, 32, __imp__sub_8226CE20, __imp__sub_8226CEC0},
};

uint32_t g_front_end = 0;            // the front end that owns ours (one at a time)
bool g_counters_set_up = false;      // players 3-4's counter prompts set up (SetUpInGame)
uint32_t g_objects[kKinds][2] = {};  // players 3-4's objects (0 = none)

int Extra() { return more_players::LocalPlayerCount() - 2; }  // how many of ours exist (0-2)

}  // namespace

// Player p's object of `kind` (players 1-2: the front end's own), 0 if none.
uint32_t ObjectOf(int kind, uint32_t front_end, int player) {
  if (player < 0) return 0;
  if (player < 2) return front_end + kInfo[kind].offset + kInfo[kind].size * uint32_t(player);
  if (player - 2 >= Extra() || front_end != g_front_end) return 0;
  return g_objects[kind][player - 2];
}

}  // namespace more_players_frontend

using namespace more_players_frontend;

// --- creation / destruction ------------------------------------------------------
extern "C" REX_FUNC(__imp__sub_8225F8A0);
extern "C" REX_FUNC(sub_8225F8A0) {  // front end constructor (r3 = front end)
  const uint32_t fe = ctx.r3.u32;
  __imp__sub_8225F8A0(ctx, base);
  if (Extra() <= 0) return;
  g_front_end = fe;
  for (int k = 0; k < kKinds; ++k) {
    for (int i = 0; i < Extra(); ++i) {
      const uint32_t object = rex::system::kernel_memory()->SystemHeapAlloc(kInfo[k].size);
      std::memset(Guest(object), 0, kInfo[k].size);
      CallGame(kInfo[k].constructor, ctx, base, object);
      g_objects[k][i] = object;
    }
  }
  for (int i = 0; i < Extra(); ++i) Write32(g_objects[kReticle][i] + 44, uint32_t(i + 2));
  REXLOG_INFO("More players: combo meters, lock-on arrows, counter prompts and reticles for {} more players",
              Extra());
  ctx.r3.u64 = fe;
}
extern "C" REX_FUNC(__imp__sub_8225FBD8);
extern "C" REX_FUNC(sub_8225FBD8) {  // front end destructor body
  if (ctx.r3.u32 == g_front_end && g_front_end) {
    for (int k = 0; k < kKinds; ++k) {
      for (int i = 0; i < 2; ++i) {
        if (!g_objects[k][i]) continue;
        if (kInfo[k].destructor) CallGame(kInfo[k].destructor, ctx, base, g_objects[k][i]);
        rex::system::kernel_memory()->SystemHeapFree(g_objects[k][i]);
        g_objects[k][i] = 0;
      }
    }
    g_front_end = 0;
  }
  __imp__sub_8225FBD8(ctx, base);
}

// --- the front end's loops ------------------------------------------------------
// Set-up (sub_82261330): each player's object gets its number, then slot 1.
extern "C" REX_FUNC(__imp__sub_82261330);
extern "C" REX_FUNC(sub_82261330) {
  const uint32_t fe = ctx.r3.u32;
  __imp__sub_82261330(ctx, base);
  if (fe != g_front_end) return;
  for (int i = 0; i < Extra(); ++i) {
    const uint32_t p = uint32_t(i + 2);
    Write32(g_objects[kCombo][i] + 3148, p);
    CallVirtual(ctx, base, g_objects[kCombo][i], 1);
    CallGame(__imp__sub_820FB518, ctx, base, g_objects[kLockOn][i], p);
    CallVirtual(ctx, base, g_objects[kLockOn][i], 1);
    Write32(g_objects[kCounter][i] + 8, p);
    // (the counter prompt's own set-up waits for the in-game HUD: SetUpInGame)
    Write32(g_objects[kMarker][i] + 8, p);
    CallVirtual(ctx, base, g_objects[kMarker][i], 1);
  }
  g_counters_set_up = false;
}
// Update (sub_82261C40, f1 = frame time): slot 3 of each.
extern "C" REX_FUNC(__imp__sub_82261C40);
extern "C" REX_FUNC(sub_82261C40) {
  const uint32_t fe = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_82261C40(ctx, base);
  if (fe != g_front_end) return;
  for (int i = 0; i < Extra(); ++i) {
    for (int k : {kCombo, kLockOn, kCounter}) CallVirtual(ctx, base, g_objects[k][i], 3, 0, dt);
    // (a marker follows its player's character: none yet, no update)
    if (more_players::CharacterOfPlayer(i + 2)) CallVirtual(ctx, base, g_objects[kMarker][i], 3, 0, dt);
  }
}
// Slot 2 of each (sub_82261F90).
extern "C" REX_FUNC(__imp__sub_82261F90);
extern "C" REX_FUNC(sub_82261F90) {
  const uint32_t fe = ctx.r3.u32;
  __imp__sub_82261F90(ctx, base);
  if (fe != g_front_end) return;
  for (int i = 0; i < Extra(); ++i) {
    for (int k : {kCombo, kLockOn, kCounter, kMarker}) CallVirtual(ctx, base, g_objects[k][i], 2);
  }
}
// Draw (sub_82263328): the combo meters and counter prompts that want to (slot
// 6) draw (slot 4), when the front end draws players 1-2's (+8593 bit 0x10).
// INSIDE A 2D BLOCK of their own: the original draws them between its
// "begin 2D" sub_82262FA8(fe, 1, 0, 1) and "end 2D" sub_82263198(fe); drawn
// after the end, players 3-4's meters and prompts never showed (found
// 2026-10-05: their meters counted hits and said "draw", nothing on screen).
// And BEFORE the original: inside its block the meters and prompts come first
// (loop 0x822634E0-0x82263560), then the in-game page (bctrl 0x82263580),
// which holds every player's prompt TEXT (the Y: CounterButtonPlayerN). Drawn
// after the original, players 3-4's red star covered their own Y (found
// 2026-10-06 from a screenshot); drawn first, the page's Y lands on top, as
// for players 1-2.
extern "C" REX_FUNC(__imp__sub_82263328);
extern "C" REX_FUNC(__imp__sub_82262FA8);
extern "C" REX_FUNC(__imp__sub_82263198);
extern "C" REX_FUNC(sub_82263328) {
  const uint32_t fe = ctx.r3.u32;
  if (fe == g_front_end && (*Guest(fe + 8593) & 0x10)) {
    CallGame(__imp__sub_82262FA8, ctx, base, fe, 1, 0, 1);
    for (int i = 0; i < Extra(); ++i) {
      for (int k : {kCombo, kCounter}) {
        if (CallVirtual(ctx, base, g_objects[k][i], 6) & 0xFF) CallVirtual(ctx, base, g_objects[k][i], 4);
      }
    }
    CallGame(__imp__sub_82263198, ctx, base, fe);
  }
  __imp__sub_82263328(ctx, base);
  if (fe != g_front_end) return;
  // The markers over the heads, when the front end draws players 1-2's
  // (+8594 bit 0x08, then sub_8226D958): each one that wants to (slot 6) and
  // whose player has joined. (The original also sorts players 1-2's two by
  // distance; ours are drawn after them.)
  if (*Guest(fe + 8594) & 0x08) {
    for (int i = 0; i < Extra(); ++i) {
      const uint32_t state = Read32(0x8259B11C + 4 * uint32_t(i + 2));
      if (state != 0 && more_players::CharacterOfPlayer(i + 2) &&
          (CallVirtual(ctx, base, g_objects[kMarker][i], 6) & 0xFF)) {
        CallVirtual(ctx, base, g_objects[kMarker][i], 4);
      }
    }
  }
}
// The reticles' update (sub_82265248, f1): slot 0 of each.
extern "C" REX_FUNC(__imp__sub_82265248);
extern "C" REX_FUNC(sub_82265248) {
  const uint32_t fe = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_82265248(ctx, base);
  if (fe != g_front_end) return;
  for (int i = 0; i < Extra(); ++i) CallVirtual(ctx, base, g_objects[kReticle][i], 0, 0, dt);
}
// Game start (sub_82260048) numbers the reticles 0 and 1: ours 2 and 3.
extern "C" REX_FUNC(__imp__sub_82260048);
extern "C" REX_FUNC(sub_82260048) {
  const uint32_t fe = ctx.r3.u32;
  __imp__sub_82260048(ctx, base);
  if (fe != g_front_end) return;
  for (int i = 0; i < Extra(); ++i) Write32(g_objects[kReticle][i] + 44, uint32_t(i + 2));
}

// --- lookups by player ------------------------------------------------------------
extern "C" REX_FUNC(__imp__sub_8226FCD8);  // player number of an actor (r3 = game object)
static int PlayerOfActor(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  return int32_t(CallGame(__imp__sub_8226FCD8, ctx, base, 0x825B0008, actor));
}

// Combo meter: the script helpers (r4 = actor) -> the meter's function.
extern "C" REX_FUNC(__imp__sub_8225D948);
extern "C" REX_FUNC(__imp__sub_8225D840);
extern "C" REX_FUNC(__imp__sub_821661E8);
extern "C" REX_FUNC(__imp__sub_82166240);
extern "C" REX_FUNC(sub_821661E8) {
  const int p = PlayerOfActor(ctx, base, ctx.r4.u32);
  if (p < 2) { __imp__sub_821661E8(ctx, base); return; }
  const uint32_t fe = Read32(Read32(0x8259B190) + 52);
  if (const uint32_t meter = ObjectOf(kCombo, fe, p)) CallGame(__imp__sub_8225D948, ctx, base, meter);
}
extern "C" REX_FUNC(sub_82166240) {
  const int p = PlayerOfActor(ctx, base, ctx.r4.u32);
  if (p < 2) { __imp__sub_82166240(ctx, base); return; }
  const uint32_t fe = Read32(Read32(0x8259B190) + 52);
  if (const uint32_t meter = ObjectOf(kCombo, fe, p)) CallGame(__imp__sub_8225D840, ctx, base, meter);
}
// WHERE A COMBO METER STANDS (sub_8225D798: r3 = meter, r4 = out x, y; used
// by both its draw parts). The game: player 0 at the left HUD's x + 50, ANY
// other player at the right HUD's x - 50 (it tests +3148 == 0 only), y 200
// (0x82506B78) for all. So players 3-4's meters were drawn exactly over player
// 2's (found 2026-10-05 from a playtest screenshot: no meter for players
// 3-4). With 3-4 players: the side by player & 1 (players 1 / 3 left, 2 / 4
// right, like their HUDs) and the height next to the player's own HUD
// (more_players_hud::ComboMeterY). With two players the game's own place.
extern "C" REX_FUNC(__imp__sub_8225D798);
extern "C" REX_FUNC(sub_8225D798) {
  if (more_players::LocalPlayerCount() <= 2) { __imp__sub_8225D798(ctx, base); return; }
  const uint32_t meter = ctx.r3.u32, out = ctx.r4.u32;
  const uint32_t player = Read32(meter + 3148);
  Write32(meter + 3148, player & 1);  // the original only picks the side from it
  __imp__sub_8225D798(ctx, base);
  Write32(meter + 3148, player);
  const float y = more_players_hud::ComboMeterY(int(player & 3));
  uint32_t bits;
  std::memcpy(&bits, &y, 4);
  Write32(out + 4, bits);
}

// Debug FIFO "cheat combo <player> [hits]" (cheats.cpp): count hits on a
// player's meter as a landed hit does (sub_8225D840, the script helper's
// target), to see the meters without a fight. Game thread.
namespace more_players_frontend {
void AddComboHit(PPCContext& ctx, uint8_t* base, int player) {
  const uint32_t fe = Read32(Read32(0x8259B190) + 52);
  if (const uint32_t meter = ObjectOf(kCombo, fe, player)) CallGame(__imp__sub_8225D840, ctx, base, meter);
}
// Debug FIFO "cheat counter <player>" (cheats.cpp): show a player's "counter
// now" prompt (the Y of a titan's dodge-counter) for 4 s, to see the four
// prompts' places without a titan fight. The game's show (sub_8225E760(prompt,
// 1), called by sub_822664F8) sets the prompt's timer +36 to 0.1 s and calls
// it again every frame while a titan winds up; the update (slot 3) counts the
// timer down and the prompt shows while it's >= 0. Here: the timer set to 4 s
// directly. Game thread.
void ShowCounterPrompt(PPCContext& ctx, uint8_t* base, int player) {
  (void)ctx; (void)base;
  const uint32_t fe = Read32(Read32(0x8259B190) + 52);
  if (const uint32_t prompt = ObjectOf(kCounter, fe, player)) {
    const float seconds = 4.0f;
    uint32_t bits;
    std::memcpy(&bits, &seconds, 4);
    Write32(prompt + 36, bits);
  }
}
}  // namespace more_players_frontend

// Crash's collect (sub_82198738): "mulli r11,r25,3152 ; add r11,r11,r8 ; addi
// r3,r11,1852 ; bl sub_8225D9E0" -> before the call, players 3-4's meter. If
// there is none, the call goes to a harmless dummy (the meter's fields of a
// zeroed scratch area) -- never to the front end's other fields.
static uint32_t g_dummy_meter = 0;
void MorePlayersComboMeterOf(PPCRegister& player, PPCRegister& meter) {
  const int p = int32_t(player.u32);
  if (p < 2) return;
  const uint32_t ours = ObjectOf(kCombo, g_front_end, p);
  if (ours) { meter.u64 = ours; return; }
  if (!g_dummy_meter) {
    g_dummy_meter = rex::system::kernel_memory()->SystemHeapAlloc(3152);
    std::memset(Guest(g_dummy_meter), 0, 3152);
  }
  meter.u64 = g_dummy_meter;
}

// Counter prompt (sub_822664F8: r3 = front end, r5 = actor): show it.
extern "C" REX_FUNC(__imp__sub_822664F8);
extern "C" REX_FUNC(__imp__sub_8225E760);
// --debug_coop_trace: "player N's counter prompt shown" when its timer (+36,
// shown while >= 0) starts (the game renews it every frame of the wind-up).
extern "C" REX_FUNC(sub_822664F8) {
  const uint32_t fe = ctx.r3.u32;
  const int p = PlayerOfActor(ctx, base, ctx.r5.u32);
  const uint32_t prompt = ObjectOf(kCounter, fe, p);
  const bool was_shown = prompt && int32_t(Read32(prompt + 36)) >= 0;  // float sign bit
  if (p < 2) {
    __imp__sub_822664F8(ctx, base);
  } else {
    if (prompt) CallGame(__imp__sub_8225E760, ctx, base, prompt, 1);
    ctx.r3.u64 = prompt ? 1 : 0;
  }
  if (REXCVAR_GET(debug_coop_trace) && prompt && !was_shown && int32_t(Read32(prompt + 36)) >= 0) {
    REXLOG_INFO("Co-op: player {}'s counter prompt shown (a titan winds up a heavy attack)", p + 1);
  }
}

// Lock-on arrow (sub_82262150: r3 = front end, r4 = player, r5 / r6 passed on).
extern "C" REX_FUNC(__imp__sub_82262150);
extern "C" REX_FUNC(__imp__sub_8226E218);
extern "C" REX_FUNC(sub_82262150) {
  const int p = int32_t(ctx.r4.u32);
  if (p < 2) { __imp__sub_82262150(ctx, base); return; }
  const uint32_t arrow = ObjectOf(kLockOn, ctx.r3.u32, p);
  if (!arrow) return;
  ctx.r3.u64 = arrow;
  ctx.r4.u64 = ctx.r5.u64;
  ctx.r5.u64 = ctx.r6.u64;
  __imp__sub_8226E218(ctx, base);
}

// Reticle by controller: which player uses controller r4 (r3 = front end), -1
// = none. The original looks at players 1-2 only.
extern "C" REX_FUNC(sub_82266130);  // controller of player r4 (four players: more_players.cpp)
extern "C" REX_FUNC(sub_822661B8) {
  const uint32_t fe = ctx.r3.u32;
  const int32_t controller = int32_t(ctx.r4.u32);
  int32_t player = -1;
  for (int p = 0; p < more_players::LocalPlayerCount() && player < 0; ++p) {
    const int32_t c = p < 2 ? int32_t(Read32(fe + 8524 + 4 * p))
                            : int32_t(CallGame(sub_82266130, ctx, base, fe, uint32_t(p)));
    if (c == controller) player = p;
  }
  ctx.r3.u64 = uint32_t(player);
}
// ... its users: "mulli r11,r3,52 ; add r11,r11,r8" then r11 + 8420 = the
// reticle: players 3-4's r11 = ours - 8420 (sub_822ACD78 at 0x822ACE80,
// sub_822AD320 at 0x822AD384), and "addi r11,r3,162 ; mulli ; add r4,r11,r8"
// = the reticle + 4 (sub_822AD0A0, at 0x822AD124).
void MorePlayersReticleBase(PPCRegister& player, PPCRegister& base_register) {
  const int p = int32_t(player.u32);
  if (p < 2) return;
  const uint32_t ours = ObjectOf(kReticle, g_front_end, p);
  if (ours) base_register.u64 = ours - 8420;
}
void MorePlayersReticlePlus4(PPCRegister& player, PPCRegister& pointer) {
  const int p = int32_t(player.u32);
  if (p < 2) return;
  const uint32_t ours = ObjectOf(kReticle, g_front_end, p);
  if (ours) pointer.u64 = ours + 4;
}

// Reticle by character (r4 = the character): sub_8213FF18 turns the player's
// reticle on (+48 bit 0x80), sub_8213FFA8 off. The originals compare against
// players 1-2's characters only: they do nothing for 3-4, then ours.
extern "C" REX_FUNC(__imp__sub_8213FF18);
extern "C" REX_FUNC(__imp__sub_8213FFA8);
static void ReticleActive(PPCContext& ctx, uint8_t* base, uint32_t character, bool on) {
  const int p = PlayerOfActor(ctx, base, character);
  if (p < 2) return;
  const uint32_t reticle = ObjectOf(kReticle, g_front_end, p);
  if (!reticle) return;
  uint8_t* flags = Guest(reticle + 48);
  *flags = on ? (*flags | 0x80) : (*flags & 0x7F);
}
extern "C" REX_FUNC(sub_8213FF18) {
  const uint32_t character = ctx.r4.u32;
  __imp__sub_8213FF18(ctx, base);
  ReticleActive(ctx, base, character, true);
}
extern "C" REX_FUNC(sub_8213FFA8) {
  const uint32_t character = ctx.r4.u32;
  __imp__sub_8213FFA8(ctx, base);
  ReticleActive(ctx, base, character, false);
}
// sub_82140038 (r4 = the character): finds the player's index by comparing with
// players 1-2's characters (0x82140074 "bne" = none: the end). Hooks: at that
// branch a player 3-4 character continues with r6 = its player; then the
// reticle base (0x82140090: r6 / r11), the reticle + 4 (0x8214010C: r6 / r4)
// and the base again from r30 = p * 52 (0x8214015C).
bool MorePlayersReticleCharacter(PPCRegister& character, PPCRegister& player) {
  for (int p = 2; p < more_players::LocalPlayerCount(); ++p) {
    if (more_players::CharacterOfPlayer(p) == character.u32) {
      player.u64 = uint32_t(p);
      return true;  // continue at 0x8214007C
    }
  }
  return false;
}
void MorePlayersReticleBaseFrom52(PPCRegister& offset, PPCRegister& base_register) {
  const uint32_t p = offset.u32 / 52;
  if (p < 2) return;
  const uint32_t ours = ObjectOf(kReticle, g_front_end, int(p));
  if (ours) base_register.u64 = ours - 8420;
}

// The counter prompt's set-up (slot 1, sub_8225E3E8) finds its text in the page
// InGame, which isn't loaded when the front end sets its objects up at boot:
// for player 3 that was a null element (read of guest 0x74, 2026-10-04). Players
// 3-4's prompts are set up when the in-game HUD is (more_players_hud.cpp calls
// this from the HUD controller's set-up), once per front-end set-up.
void more_players_frontend::SetUpInGame(PPCContext& ctx, uint8_t* base) {
  if (g_counters_set_up || !g_front_end) return;
  for (int i = 0; i < Extra(); ++i) {
    if (g_objects[kCounter][i]) CallVirtual(ctx, base, g_objects[kCounter][i], 1);
  }
  g_counters_set_up = true;
}


// --- the markers over the heads (CPlayerIdentifierArrow) ------------------------
// Its set-up (slot 1, sub_8226CF10) takes its picture by name from a two-entry
// table (0x825A5B20: HUD_coop_player_one / two .tga) indexed by its player:
// players 3-4's pictures of their own (or player 1 / 2's banner when there
// are none) are more_players_markers.cpp's. Its draw (slot 4, sub_8226D358)
// tints it with a two-entry colour table (0x825B0120, ARGB) read "lwzx
// r9,r10,r30" with r10 = player * 4: for player 3 that was the next word (an
// init flag: a black, see-through marker). Players 3-4: these colours (also
// their reticles' and their loading-screen paw prints').
REXCVAR_DEFINE_UINT32(marker_colour_player3, 0xFF3CD23C, "CrashMoM",
                      "Players 3-4: the colour of player 3's marker over its head (0xAARRGGBB)");
REXCVAR_DEFINE_UINT32(marker_colour_player4, 0xFFB45AF0, "CrashMoM",
                      "Players 3-4: the colour of player 4's marker over its head (0xAARRGGBB)");
void MorePlayersMarkerColour(PPCRegister& offset, PPCRegister& colour) {
  const uint32_t p = offset.u32 / 4;
  if (p == 2) colour.u64 = REXCVAR_GET(marker_colour_player3);
  if (p == 3) colour.u64 = REXCVAR_GET(marker_colour_player4);
}

// --- the reticles' draw list ------------------------------------------------------
// CReticleAimingBehaviour (the aiming reticle of a masked player) registers
// itself for drawing in a global table 0x8259AFAC of THREE: player 1's, player
// 2's, and one for anyone else (its update sub_82161B40: by comparing its owner
// with players 1-2's character / titan). The draw (sub_82162540) draws the
// three, the third in player 1's colours. With players 3 and 4 both masks only
// one of their reticles fit. Now the original registers as before, and right
// after its update the reticle moves to a table of ours with one entry per
// player and the shared one last; the draw is rewritten over ours.
extern "C" REX_FUNC(__imp__sub_82161B40);
extern "C" REX_FUNC(__imp__sub_82161A08);
extern "C" REX_FUNC(__imp__sub_82262FA8);
extern "C" REX_FUNC(__imp__sub_822ACB88);
extern "C" REX_FUNC(__imp__sub_82263198);
namespace more_players_frontend {
namespace {
constexpr uint32_t kReticleTable = 0x8259AFAC;  // the original's three entries
constexpr int kReticleSlots = 5;                // players 1-4 + the shared one
uint32_t g_reticles[kReticleSlots] = {};        // CReticleAimingBehaviour objects
void ForgetReticle(uint32_t reticle) {
  for (uint32_t& r : g_reticles) if (r == reticle) r = 0;
}
}  // namespace
}  // namespace more_players_frontend

// Update (r3 = the reticle behaviour, r4 = its owner).
extern "C" REX_FUNC(sub_82161B40) {
  const uint32_t reticle = ctx.r3.u32, owner = ctx.r4.u32;
  __imp__sub_82161B40(ctx, base);
  if (more_players::LocalPlayerCount() <= 2) return;
  bool registered = false;
  for (uint32_t i = 0; i < 3; ++i) {
    if (Read32(kReticleTable + 4 * i) == reticle) {
      Write32(kReticleTable + 4 * i, 0);
      registered = true;
    }
  }
  ForgetReticle(reticle);
  if (!registered) return;  // (switched off: the original took it out)
  const int p = PlayerOfActor(ctx, base, owner);
  g_reticles[p >= 0 && p < 4 ? p : kReticleSlots - 1] = reticle;
}
// Destructor body: out of ours too.
extern "C" REX_FUNC(sub_82161A08) {
  ForgetReticle(ctx.r3.u32);
  __imp__sub_82161A08(ctx, base);
}
// Draw (no arguments): as the original, over ours (the shared one in player 1's colours).
extern "C" REX_FUNC(__imp__sub_82162540);
extern "C" REX_FUNC(sub_82162540) {
  if (more_players::LocalPlayerCount() <= 2) { __imp__sub_82162540(ctx, base); return; }
  const uint32_t game = Read32(0x8259B190);
  if (*Guest(game + 168) & 0x20) return;
  for (int slot = 0; slot < kReticleSlots; ++slot) {
    const uint32_t r = g_reticles[slot];
    if (!r) continue;
    const uint8_t flags = *Guest(r + 264);
    if (!(flags & 0x80) && !(flags & 0x40)) continue;
    CallGame(__imp__sub_82262FA8, ctx, base, Read32(game + 52), 1, 0, 1);
    CallGame(__imp__sub_822ACB88, ctx, base, r + 28, uint32_t(slot < 4 ? slot : 0));
    CallGame(__imp__sub_82263198, ctx, base, Read32(Read32(0x8259B190) + 52));
  }
}

// The reticle's colours by player: "lwzx" from two-entry tables (0x825A72FC,
// and 0x82507760 for its flashing variant: player 1 red, player 2 green, alpha
// 0xE1), index = player * 4. Players 3-4: their marker colours.
void MorePlayersReticleColour(PPCRegister& offset, PPCRegister& colour) {
  const uint32_t p = offset.u32 / 4;
  if (p == 2) colour.u64 = REXCVAR_GET(marker_colour_player3);
  if (p == 3) colour.u64 = REXCVAR_GET(marker_colour_player4);
}
void MorePlayersReticleFlashColour(PPCRegister& offset, PPCRegister& colour) {
  const uint32_t p = offset.u32 / 4;
  if (p == 2) colour.u64 = (REXCVAR_GET(marker_colour_player3) & 0x00FFFFFF) | 0xE1000000;
  if (p == 3) colour.u64 = (REXCVAR_GET(marker_colour_player4) & 0x00FFFFFF) | 0xE1000000;
}
