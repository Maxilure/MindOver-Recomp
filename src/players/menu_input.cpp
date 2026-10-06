// =============================================================================
// players/menu_input.cpp -- an in-game menu listens to the player it belongs
// to (findings/26 s.21)
// =============================================================================
//
// HOW THE GAME'S MENUS READ BUTTONS: every front-end screen asks "was button B
// pressed?" through ONE script method, IsButtonPressed (sub_822652B0, r3 =
// front end, r4 = button, r6 = a player or -1, r5/r7/r8 = flags). With a
// player in r6 it reads that player's controller only. With -1 it reads, by
// button and state:
//   * the MENU OWNER's controller (front end +8536, "PlayerWhoPausedGame",
//     -1 = none) while the game global's byte 168 bit 0x10 is set; in that
//     mode a question naming another player is refused too (traced
//     2026-10-05: on in the pause menu and the level-up screen);
//   * player 1's controller only, in the main menus (+8594 bit 0x20, set by
//     the script method SetInMainMenuFE);
//   * else a loop over the PLAYERS (0..1) or over every CONTROLLER (0..3).
// The owner is set by StoreWhoPausedGame (sub_82265968): owner = +8540, the
// player who last pressed START in play, recorded by sub_82265978 inside
// IsButtonPressed's player loop. ResetPlayerWhoPausedGame (sub_82266120) puts
// -1 back when the pause menu closes. The level-up screen goes through the
// same step (so it belonged to who last pressed START: usually player 1, the
// playtest's "only player 1 can close it"); the save totem's question, save
// list and name screen run with owner -1.
// The front end's "which menu is up" flags (+8592): 0x20 map, 0x08 tutorial,
// 0x04 save prompt (set the moment Crash interacts with the totem), 0x02
// mini-game menu, 0x01 game complete (0x40: always on in play). "A menu is on
// screen in play" = +8594 bit 0x80 (traced 2026-10-05: set for the totem's
// question, the pause menu, the whole save list flow; 0x08 back in play).
//
// WHAT WENT WRONG (playtest, 2026-10-04): the player loops stopped at 2, so
// players 3-4's presses never reached a menu ("anyone" menus ignored them, and
// their START never counted as pausing); and nothing tied a menu to the player
// who opened it: anyone in the loop could work the totem's question, the save
// list and the name screen, and a pause menu answered to every player.
//
// THE RULE NOW, while a level is played (game state 5, not the main menus):
//   * the player loops cover every local player (manifest: MenuInputPlayerLoop);
//   * a question with no player named (r6 = -1) is asked of the menu's owner
//     only, when there is one:
//       1. the game's own owner (+8536: who paused);
//       2. the player who last INTERACTED (Crash pressing B next to the
//          totem: CInteractorBehaviour sub_8214A9A8 messages the object with
//          its own character at +28) owns:
//          - a CONFIRM BOX (front end +172: the totem's "Do you want to save
//            the game?"; showing = its byte +4 bit 0x40, 0x80 = enabled;
//            update sub_8225DE68) whose first question comes within 8 s of
//            the interaction (it asks only once its opening animation is
//            over, ~4 s after the press), AND THE WHOLE FLOW BEHIND IT (the
//            save list, the name screen, the overwrite question) until no
//            in-game menu has been up for 2 s;
//          - the save prompt (+8592 bit 0x04) and the mini-game menu (0x02);
//     otherwise (tutorials, the map, the game-complete screen) anyone;
//   * the LEVEL-UP screen (CUpgradeScreenAction) answers to EVERY player:
//     upgrades are shared, like mojos (it would otherwise be "who paused").
// Button checks with a player named, and everything outside play, unchanged.
//
// --debug_menu_input_trace logs every press a menu hears (button, player asked
// and answered for, owner, caller), START presses, owner changes, interactions.
// =============================================================================
#include "more_players.h"

#include "../input/players.h"

#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(debug_menu_input_trace, false, "CrashMoM",
                    "Log every button press a menu hears (button, player asked, menu owner, caller), "
                    "START presses, menu owner changes and interactions (findings/26)");

extern "C" REX_FUNC(__imp__sub_822652B0);  // IsButtonPressed
extern "C" REX_FUNC(__imp__sub_82265968);  // StoreWhoPausedGame
extern "C" REX_FUNC(__imp__sub_82266120);  // ResetPlayerWhoPausedGame
extern "C" REX_FUNC(__imp__sub_8214A9A8);  // CInteractorBehaviour: interact with the object nearby
extern "C" REX_FUNC(__imp__sub_8226FCD8);  // player number of an actor (r3 = game object, r4 = actor)
extern "C" REX_FUNC(__imp__sub_820E5350);  // CUpgradeScreenAction: Enter (vtable 0x82025AA4 slot 4)
extern "C" REX_FUNC(__imp__sub_820E5F20);  // CUpgradeScreenAction: Exit (slot 6; slot 5 = Update)

namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;   // the game global (state at +8, front end at +52)
constexpr uint32_t kGameObject = 0x825B0008;   // the game object (characters; sub_82270608)
constexpr uint32_t kCoOpState = 0x8259B11C;    // co-op state per player (4 entries, more_players)
constexpr uint32_t kOwner = 8536;              // front end: the menu's owner (player, -1 = none)
constexpr uint32_t kWhoPressedStart = 8540;    // front end: who last pressed START in play
constexpr uint32_t kMenuFlags = 8592;          // front end: which in-game menu is up
constexpr uint8_t kMenuSavePrompt = 0x04, kMenuMiniGame = 0x02;
constexpr auto kFlowGap = std::chrono::seconds(2);
constexpr uint32_t kFrontEndFlags2 = 8594;     // front end: bit 0x20 = in the main menus
constexpr uint8_t kInMainMenus = 0x20;
constexpr uint8_t kMenuOnScreen = 0x80;       // +8594 bit 0x80: a menu is on screen in play
constexpr uint32_t kConfirmBox = 172 + 4;     // front end: confirm box's flags byte
constexpr uint8_t kConfirmShowing = 0x40;  // (0x80 = the box object is enabled: always)
constexpr auto kConfirmAfterInteraction = std::chrono::seconds(8);

// The player who last interacted with an object (B at the totem), -1 = none,
// and when (game's main thread only).
std::atomic<int> g_last_interactor{-1};
std::chrono::steady_clock::time_point g_interacted_at;
// Is the level-up screen (CUpgradeScreenAction) up? (Its Enter / Exit below.)
std::atomic<bool> g_upgrade_screen{false};

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

bool Tracing() { return REXCVAR_GET(debug_menu_input_trace); }

// Calls a game function with r3/r4; the caller's registers are put back.
uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

int PlayerOfActor(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  return actor ? int(int32_t(CallGame(__imp__sub_8226FCD8, ctx, base, kGameObject, actor))) : -1;
}

// Does player p play right now? (Player 1 always does.)
bool Plays(int p) {
  return p == 0 || (p > 0 && p < 4 && Read32(kCoOpState + 4 * p) != 0);
}

// Who the menu on screen belongs to (rules in the header), -1 = anyone.
// `why` names the rule, for the trace.
int MenuOwner(PPCContext& ctx, uint8_t* base, uint32_t fe, const char** why) {
  *why = "nobody";
  if (!more_players::InPlay() || (*Guest(fe + kFrontEndFlags2) & kInMainMenus)) return -1;
  int owner = -1;
  if (owner < 0) {
    owner = int(int32_t(Read32(fe + kOwner)));
    *why = "paused";
  }
  // A confirm box's owner is decided once, at its first question (it asks
  // only after its opening animation, ~4 s after the B press at the totem):
  // the interactor if the interaction was within 8 s, else nobody. That owner
  // then keeps the whole FLOW behind the box (the totem: its save list, the
  // name screen, the overwrite question), until no in-game menu has been up
  // for 2 s (traced: +8594 bit 0x80 stays set from the box through the
  // storage access to the end of the save list).
  static bool confirm_was_showing = false;
  static int flow_owner = -1;
  static std::chrono::steady_clock::time_point menu_seen;
  const auto now = std::chrono::steady_clock::now();
  const bool confirm_showing = *Guest(fe + kConfirmBox) & kConfirmShowing;
  const uint8_t menus = *Guest(fe + kMenuFlags);
  if (confirm_showing && !confirm_was_showing &&
      now - g_interacted_at < kConfirmAfterInteraction) {
    flow_owner = g_last_interactor.load();  // (only the gap below ends a flow: the box's
    if (Tracing()) {                        // closing animation shows it once more)
      REXLOG_INFO("Menu input: a confirm box for player {} (they interacted)", flow_owner + 1);
    }
  }
  confirm_was_showing = confirm_showing;
  const bool menu_up = confirm_showing || (*Guest(fe + kFrontEndFlags2) & kMenuOnScreen) ||
                       (menus & (kMenuSavePrompt | kMenuMiniGame));
  if (menu_up) {
    menu_seen = now;
  } else if (now - menu_seen > kFlowGap && flow_owner >= 0) {
    if (Tracing()) REXLOG_INFO("Menu input: player {}'s menus are closed", flow_owner + 1);
    flow_owner = -1;
  }
  // (Applied only while one of those menus is up: in between, play goes on,
  // and the gameplay's own "anyone pressed START?" must hear everyone.)
  if (owner < 0 && menu_up) {
    if (flow_owner >= 0) {
      owner = flow_owner;
      *why = "interacted";
    } else if (menus & (kMenuSavePrompt | kMenuMiniGame)) {
      owner = g_last_interactor.load();
      *why = "interacted";
    }
  }
  if (!Plays(owner)) {
    *why = "nobody";
    return -1;
  }
  return owner;
}

}  // namespace

namespace menu_input {
int CurrentMenuOwner(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = Read32(kGameGlobal);
  if (!game) return -1;
  const uint32_t fe = Read32(game + 52);
  if (!fe) return -1;
  const char* why = "";
  return MenuOwner(ctx, base, fe, &why);
}
bool PlayerPlays(int p) { return Plays(p); }
}  // namespace menu_input

// IsButtonPressed: a question for "anyone" goes to the menu's owner.
extern "C" REX_FUNC(sub_822652B0) {
  const uint32_t fe = ctx.r3.u32, button = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const int32_t asked = int32_t(ctx.r6.u32);
  const char* why = "";
  const int32_t who_before = int32_t(Read32(fe + kWhoPressedStart));
  int owner = -1;
  // A menu whose owner has NO controller (unplugged, or moved to another
  // player in F6, while their pause menu was up): every player may answer it,
  // like the level-up screen, or nobody could ever close it
  // (players/lost_controller.h: its own box waits while a pause menu is up).
  bool owner_without_device = false;
  if (asked == -1 && more_players::InPlay() && !g_upgrade_screen.load()) {
    const char* unused = "";
    const int menu_owner = MenuOwner(ctx, base, fe, &unused);
    const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
    owner_without_device = menu_owner >= 0 && assignment && !assignment->HasDeviceFor(menu_owner);
  }
  if (asked == -1 && (g_upgrade_screen.load() || owner_without_device) && more_players::InPlay()) {
    // The level-up screen: upgrades are SHARED (like mojos), so every player
    // may answer it. Asked player by player: the game's own "anyone" answer
    // would go to who paused, or to player 1 only.
    // The game's owner check refuses everyone but +8536 (the screen sets it
    // up like a pause menu: owner = who last pressed START, usually player
    // 1), so each player is asked AS the owner, and the owner is put back.
    why = owner_without_device ? "its owner has no controller" : "level-up screen";
    const PPCContext saved = ctx;
    const uint32_t game_owner = Read32(fe + kOwner);
    bool heard = false;
    for (int p = 0; p < more_players::LocalPlayerCount() && !heard; ++p) {
      if (!Plays(p)) continue;
      ctx = saved;
      ctx.r6.u64 = uint32_t(p);
      Write32(fe + kOwner, uint32_t(p));
      __imp__sub_822652B0(ctx, base);
      Write32(fe + kOwner, game_owner);
      heard = ctx.r3.u32 & 0xFF;
      if (heard) owner = p;
    }
    if (!heard) ctx.r3.u64 = 0;
  } else {
    owner = asked == -1 ? MenuOwner(ctx, base, fe, &why) : -1;
    if (owner >= 0) ctx.r6.u64 = uint32_t(owner);
    __imp__sub_822652B0(ctx, base);
  }
  if (!Tracing()) return;
  static uint32_t last_state = ~0u;
  const uint32_t state = Read32(Read32(kGameGlobal) + 8);
  if (state != last_state) {
    REXLOG_INFO("Menu input: game state {} -> {}", int32_t(last_state), state);
    last_state = state;
  }
  static uint32_t last_flags = ~0u;
  const uint32_t flags = uint32_t(*Guest(fe + kMenuFlags)) << 16 | uint32_t(*Guest(fe + kFrontEndFlags2)) << 8 |
                         *Guest(fe + kConfirmBox);
  if (flags != last_flags) {
    REXLOG_INFO("Menu input: flags 8592 {:02X} 8594 {:02X} confirm {:02X}", flags >> 16,
                (flags >> 8) & 0xFF, flags & 0xFF);
    last_flags = flags;
  }
  const int32_t who_after = int32_t(Read32(fe + kWhoPressedStart));
  if (who_after != who_before) {
    REXLOG_INFO("Menu input: player {} pressed START (the next pause belongs to them)",
                who_after + 1);
  }
  if (ctx.r3.u32 & 0xFF) {
    REXLOG_INFO("Menu input: button {} pressed (asked player {}, answered for {} = {}) from {:08X} "
                "[confirm {:02X} menus {:02X} interactor {} state {} 8594 {:02X}]",
                button, asked + 1, owner >= 0 ? "player " + std::to_string(owner + 1) : "anyone",
                why, lr, *Guest(fe + kConfirmBox), *Guest(fe + kMenuFlags), g_last_interactor.load() + 1,
                Read32(Read32(kGameGlobal) + 8), *Guest(fe + kFrontEndFlags2));
  }
}

extern "C" REX_FUNC(sub_82265968) {
  const uint32_t fe = ctx.r3.u32;
  __imp__sub_82265968(ctx, base);
  if (Tracing()) {
    REXLOG_INFO("Menu input: StoreWhoPausedGame: menu owner = player {} (from {:08X})",
                int32_t(Read32(fe + kOwner)) + 1, uint32_t(ctx.lr));
  }
}

extern "C" REX_FUNC(sub_82266120) {
  __imp__sub_82266120(ctx, base);
  if (Tracing()) {
    REXLOG_INFO("Menu input: ResetPlayerWhoPausedGame: menu owner = none (from {:08X})",
                uint32_t(ctx.lr));
  }
}

// CInteractorBehaviour (Crash's side of "press B at the totem / an object"):
// sends message 2 to the object it's next to (this +32) with its own actor
// (this +28, the player's character). Remembered: who interacted last.
extern "C" REX_FUNC(sub_8214A9A8) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t actor = Read32(self + 28), target = Read32(self + 32);
  __imp__sub_8214A9A8(ctx, base);
  if (!target) return;
  const int player = PlayerOfActor(ctx, base, actor);
  if (player < 0) return;
  g_interacted_at = std::chrono::steady_clock::now();
  if (player != g_last_interactor.exchange(player) && Tracing()) {
    REXLOG_INFO("Menu input: player {} interacted with {:08X}", player + 1, target);
  }
}

// The front end's "for each player" loops in IsButtonPressed (sub_822652B0:
// "addi r28,r28,1 ; cmpwi r28,2 ; blt top", at 0x822655C4 / 0x822656A0 /
// 0x82265740): hooked on the blt, true = go round again while players remain
// (for players 1-2 exactly the original). The first one records who pressed
// START (sub_82265978), the others answer "anyone" questions in play.
bool MenuInputPlayerLoop(PPCRegister& r28) {
  return int32_t(r28.u32) < more_players::LocalPlayerCount();
}

// THE LEVEL-UP SCREEN (CUpgradeScreenAction, vtable 0x82025AA4: slot 4 Enter,
// slot 5 Update, slot 6 Exit). Playtest 2026-10-05: only player 1 could close
// it. Upgrades are shared, like mojos, so the screen belongs to everyone:
// while it's up, IsButtonPressed above asks every playing player. (It ran as
// a pause menu: StoreWhoPausedGame made "who last pressed START" its owner,
// usually player 1.)
extern "C" REX_FUNC(sub_820E5350) {
  g_upgrade_screen.store(true);
  if (Tracing()) REXLOG_INFO("Menu input: level-up screen up (any player may answer)");
  __imp__sub_820E5350(ctx, base);
}
extern "C" REX_FUNC(sub_820E5F20) {
  __imp__sub_820E5F20(ctx, base);
  g_upgrade_screen.store(false);
  if (Tracing()) REXLOG_INFO("Menu input: level-up screen closed");
}

// THE PAUSE MENU'S TITLE for players 3-4 ("P3 Paused" / "P4 Paused").
// The pause menu (CPauseScreenAction, vtable 0x82024484, Enter = slot 4
// sub_820D9770) and the Options screen opened from it (CInGameOptionsScreen-
// Action, vtable 0x82023594, Enter sub_820CDAD0: it shows the same title) pick
// the title by the menu owner (front end +8536, who paused): 0 -> the text
// "InGame_Pause_Player1" ("P1 Paused"), 1 -> "InGame_Pause_Player2", anything
// else -> "InGame_Pause_Paused" (plain "Paused"). The game's text list has no
// player 3 or 4 entries, so players 3-4 got "Paused".
// Both Enters keep the title text element at action +24 and set it with
// sub_82371CE0(element, key): looks the key up in the text list and keeps a
// pointer to its UTF-16 string at element +156 (a missing key shows the key
// itself). After the original, for owner 2-3, we take player 1's text the same
// way, change its digit 1 to 3 / 4 and set that (sub_82373360 copies a
// string), so the title follows the language of the game's own text. A text
// without a '1' (no such language seen) keeps "Paused".
// Found 2026-10-06 from the strings' references (lis 0x8202 + 0x3540..0x3584).
extern "C" REX_FUNC(__imp__sub_820D9770);  // CPauseScreenAction: Enter
extern "C" REX_FUNC(__imp__sub_820CDAD0);  // CInGameOptionsScreenAction: Enter
extern "C" REX_FUNC(__imp__sub_82371CE0);  // text element: string from the text list by key
extern "C" REX_FUNC(__imp__sub_82373360);  // text element: set its string (UTF-16, copied)

namespace {
constexpr uint32_t kPauseTitle = 24;                    // both actions: the title text element
constexpr uint32_t kTextString = 156;                   // text element: its current string
constexpr uint32_t kKeyPausePlayer1 = 0x82023558;       // "InGame_Pause_Player1"
constexpr uint32_t kKeyPaused = 0x82023570;             // "InGame_Pause_Paused"
constexpr uint32_t kTitleChars = 63;                    // our copy's room (+ the NUL)

void PauseTitleForOwner(PPCContext& ctx, uint8_t* base, uint32_t action) {
  const uint32_t front_end = Read32(Read32(kGameGlobal) + 52);
  const int owner = int32_t(Read32(front_end + kOwner));
  if (owner < 2 || owner > 3) return;  // players 1-2: the game's own; 3-4 here
  const uint32_t title = Read32(action + kPauseTitle);
  if (!title) return;
  CallGame(__imp__sub_82371CE0, ctx, base, title, kKeyPausePlayer1);
  const uint32_t text = Read32(title + kTextString);
  // Our copy of it, in guest memory (made once; game thread only).
  static uint32_t copy = 0;
  if (!copy) copy = rex::system::kernel_memory()->SystemHeapAlloc(2 * (kTitleChars + 1));
  bool digit_changed = false;
  if (text && copy) {
    uint32_t i = 0;
    for (; i < kTitleChars; ++i) {
      const uint8_t* c = Guest(text + 2 * i);  // big-endian UTF-16
      uint16_t ch = uint16_t(c[0] << 8 | c[1]);
      if (!ch) break;
      if (ch == u'1' && !digit_changed) {
        ch = uint16_t(u'1' + owner);
        digit_changed = true;
      }
      Guest(copy + 2 * i)[0] = uint8_t(ch >> 8);
      Guest(copy + 2 * i)[1] = uint8_t(ch);
    }
    Guest(copy + 2 * i)[0] = Guest(copy + 2 * i)[1] = 0;
  }
  if (digit_changed) {
    CallGame(__imp__sub_82373360, ctx, base, title, copy);
  } else {
    CallGame(__imp__sub_82371CE0, ctx, base, title, kKeyPaused);  // as before: "Paused"
  }
  if (Tracing()) {
    REXLOG_INFO("Menu input: pause title for player {} ({})", owner + 1,
                digit_changed ? "player 1's text, digit changed" : "no digit: \"Paused\"");
  }
}
}  // namespace

extern "C" REX_FUNC(sub_820D9770) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_820D9770(ctx, base);
  PauseTitleForOwner(ctx, base, action);
}
extern "C" REX_FUNC(sub_820CDAD0) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_820CDAD0(ctx, base);
  PauseTitleForOwner(ctx, base, action);
}
