// =============================================================================
// input/mouse_minigame.cpp -- the mouse on the minigame level picker
// =============================================================================
//
// WHAT THE PLAYER GETS: on the minigame screen (the panel a minigame spot
// opens in play: "Mini Game N", its goal on the left, "Level 1" .. "Level 10"
// on the right; front-end state 970 "MiniGameScreen", fig_tree.py numbers),
// pointing at a level selects it (with the game's move sound) and a left
// click starts it, like in every other menu (input/mouse_menus.h). Locked
// levels (greyed) can't be pointed at or clicked: the game marks them not
// selectable, and the shared code skips those. Right click = Back and the
// prompt bar's "Back" / "Select" clicks already worked (mouse_menus
// FrontEndFrame: they're the menus' shared prompt page, not this screen's).
// The keys already worked too (Up / Down, Enter, Esc: the keyboard's menu
// keys reach the game like a pad's D-pad, A and B).
//
// WHY IT WAS MISSING: the shared mouse code hooks CMenuAction, the game's
// generic menu screen. This screen is its own class, CMiniGameScreenAction
// (vtable 0x82023CE4, RTTI), which owns a Scrooby menu without being a
// CMenuAction, so nobody told the mouse code about its menu.
//
// HOW WE FOUND THE PIECES (xexdis of the class's slots):
//   Enter  sub_820D3660 (slot 4): finds its page by name and keeps it at
//          +20, finds the "levels" menu on that page (sub_82371988 = page:
//          find a menu) and keeps it at +24 (a holder: the menu is the word
//          at +24); then, for the 10 items, sets each one's selectable flag
//          by "item index <= levels won" (the count comes from the save's
//          stats, [*(0x8259B190)+96] + 4 * (table value + 105)).
//   Update sub_820D3A98 (slot 5, r3 = this, f1/f2 = time): steps the screen
//          by its state at +56 (2 opening, 3 open, 4/5 closing), returns 1.
//   Exit   sub_820D3B38 (slot 6).
// The wheel is left alone here: ten levels fit on screen, nothing scrolls
// (mouse_menus.h WHAT THE WHEEL DOES).
// =============================================================================
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../guest_memory.h"
#include "mouse_menus.h"

extern "C" REX_FUNC(__imp__sub_820D3A98);  // CMiniGameScreenAction::Update (r3 this, f1 / f2 time)

namespace {

constexpr uint32_t kScreenPage = 20;  // CMiniGameScreenAction +20: its Scrooby page
constexpr uint32_t kScreenMenu = 24;  // +24: its level list (Scrooby menu)

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

// The screen's Update: the mouse first (hover / click turned into a select +
// an A press the game's own Update then reads), then the original.
extern "C" REX_FUNC(sub_820D3A98) {
  const uint32_t screen = ctx.r3.u32;
  if (screen) {
    const uint32_t menu = Read32(base, screen + kScreenMenu);
    static uint32_t logged = 0;
    if (menu && menu != logged) {
      logged = menu;
      REXLOG_INFO("Mouse in menus: the minigame level list (screen {:08X}, menu {:08X})", screen, menu);
    }
    // (The game calls HoverAndClick makes, select + sound, put every register
    // back after them: f1 / f2 still hold the time for the original below.)
    mouse_menus::HoverAndClick(ctx, base, menu, nullptr, Read32(base, screen + kScreenPage));
  }
  __imp__sub_820D3A98(ctx, base);
}
