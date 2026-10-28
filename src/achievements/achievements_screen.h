// =============================================================================
// achievements/achievements_screen.h -- the Achievements screen: how it is
// reached (main menu, pause menu) and what the menus say
// =============================================================================
//
// WHAT THE PLAYER SEES (2026-10-25, findings/51 s.6): "Achievements" in the
// pause menu (after Options; the main menu had one for a few days: the
// achievements are earned in play, so the pause menu is where it belongs;
// the screen's main menu code paths stay but aren't reached). It opens
// a LIST OF CARDS in the game's usual glass panel (the Options screen's,
// kept centred), three at a time: each achievement's picture, name, text
// (the game's own "how to earn it" text while locked) and "Earned <date>" /
// "Locked"; the selected card solid with a yellow name, locked ones darkened.
//
// HOW IT'S BUILT:
//   * it IS the Options screen (options/options_menu.cpp) in a second mode:
//     the same front-end states (main menu 96 via exit 77, in game 956 via
//     the pause menu's Options exit), so the slide-in, the glass, the
//     prompts and Back come for free. OpenNext() before the exit; the
//     screen's Enter takes it (TakeOpenNext). Its rows, tabs and side card
//     are hidden; the cards are the page's own elements, drawn by the game:
//     the achievements' pictures (from the player's XEX) made into the
//     menus' sprites (achievements.h MenuSprites, ui/menu_pictures.h) and
//     added by the menus' data patches (options/options_page.cpp).
//   * the menu items: data patches (options/options_page.cpp) add copies of
//     the game's items; their words are written here at run time (the text
//     banks have no "Achievements").
//   * the pause menu's decision (sub_82121B48, node 868) maps the selected
//     item to an exit BY INDEX (1 player: 0 Resume 891 ... 3 Options 894,
//     4 Save 897, 5 Quit 898; 2 players: ... 4 Options 883, 5 Save 886,
//     6 Quit 887). The new item sits right after Options, so while that
//     decision runs, the game's GetMenuIndex answers are shifted
//     (mouse_menus::SetMenuIndexFilter): Achievements -> Options' index
//     (and OpenNext once the Options exit comes back), the items after it
//     -> one less.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace achievements_screen {

// The next Options screen opens as the Achievements screen (main menu).
void OpenNext();
// The Options screen's Enter: was it asked for the Achievements screen?
// (Clears the request.)
bool TakeOpenNext();
// The same without clearing it (the in-game screen's own Enter, which may
// run before the menu's, leaves the glass panel centred for the list).
bool OpenNextPending();

// Every front-end update (players/more_players_frontend.cpp): the
// "Achievements" items' words in the main menu and the pause menus.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// At start-up (CrashMomApp::OnPreLaunchModule, with the other hooks).
void Register();

}  // namespace achievements_screen
