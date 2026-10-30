// =============================================================================
// ui/main_menu_extras.h -- the main menu's "Extras": a second set of items
// in the same list (Players, Credits; B goes back)
// =============================================================================
//
// WHY (2026-10-30): the port's additions had grown the main menu to six
// items (New Game, Load Game, Options, Players, Credits, Quit Game), close to
// the prompt bar. The things a player looks for less often go one step in:
//   first set:   New Game, Load Game, Options, Extras, Quit Game
//   Extras set:  Players, Credits
// It stays the game's own menu: the same list, font, cursor, sounds and
// slide; picking Extras swaps the items shown, B swaps them back. (A "Back"
// item was there for a day: B already says Back on the prompt bar.) Each set
// is centred on the same middle, and the set coming in slides in from the
// side and fades in (0.22 s).
//
// HOW:
//   * the items: options/options_page.cpp (PatchMainMenu) puts all seven in
//     the StartMenu (order and indices in options_page.h), each set centred
//     on the first set's middle. Extras is a copy of Options' item, its words
//     written here at run time (the menus' text bank has no such string).
//   * which set shows: every front-end update, the other set's items are
//     hidden (their text, item +140, not drawn) and not selectable (item
//     +137 bit 0x80, scrooby::SetSelectable: the list's up / down step over
//     them, and the mouse only finds shown texts). A selection left on a
//     hidden item moves to the set's first.
//   * the picks: the main menu SCREEN's Update (sub_820CEB58, wrapped in
//     ui/quit_game.cpp) goes from open (state 3) to picked (5) on A and to
//     closing (4) on B, turning the menus' input off. A pick of Extras, or B
//     while the Extras set shows, is taken back (state 3, input on once the
//     transition is over) and the set swapped. B's own sound there is the
//     screen's closing "shutter" (as when leaving for the title): dropped
//     (DropSound, asked by the front end's sound player in
//     saves/save_library.cpp), the menu's move sound plays instead. The
//     main menu's decision answers "stay" for Extras should it reach it
//     (options/options_menu.cpp).
//   * Players and Credits leave the menu as before; back from them the
//     Extras set is still the one shown (the menu keeps its selection).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace main_menu_extras {

// Every front-end update (players/more_players_frontend.cpp, sub_82261C40):
// Extras' words, which set shows, the swap's transition.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// The main menu screen's Update, after the original (ui/quit_game.cpp's
// wrapper of sub_820CEB58): `before` = its state before the call. Takes a
// pick of Extras (or B in the Extras set) back and swaps the sets.
void AfterScreenUpdate(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t before);

// The front end's sound player (sub_82261800, saves/save_library.cpp): true =
// don't play this one (the main menu screen's B sound while the Extras set
// shows; `return_address` = the player's caller).
bool DropSound(uint32_t return_address);

}  // namespace main_menu_extras
