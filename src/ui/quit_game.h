// =============================================================================
// ui/quit_game.h -- "Quit Game" in the main menu: closes the game, after the
// game's own "are you sure?" question
// =============================================================================
//
// WHY: on the Xbox 360 a game never closes itself: the console's Guide
// button goes back to the dashboard. A PC game closes from its own main
// menu. The port's main menu (New Game, Load Game, Options, Credits) gets a
// fifth item, "Quit Game".
//
// HOW (2026-10-25):
//   * the item: a data patch of the main menus' package (options/
//     options_page.cpp, PatchMainMenu) adds a copy of the Credits item named
//     QuitItem. The menus' text bank has no "Quit" string (the pause menu's
//     "Quit Game" lives in the in-game bank, not loaded in the main menu),
//     so FrontEndFrame writes the words into the item's text (Scrooby menu
//     item +140 = its look, a Text object; scrooby::SetText under heap 7).
//   * the pick: the main menu SCREEN's Update (sub_820CEB58, wrapped here)
//     moves from open (state 3) to picked (5) on A, fades its list out and
//     only then raises the accept flag the main menu's decision acts on. A
//     pick of item 4 is taken back at once (state 3 again: the list never
//     fades) and the question asked.
//   * the question: the game's own Yes / No box, the one the pause menu's
//     "Quit Game" uses (front end +172, ShowConfirmMessage sub_8225F758;
//     the same box Options' "Default Keys" asks with). While it shows, the
//     main menu's list is hidden (as the pause menu goes under the pause
//     menu's question) and deaf (front end +8592 bit 0x40 off), the
//     decision answers "stay" (-1).
//   * Yes: the log is flushed and the process ends the way closing the
//     window does (ReXApp::OnClosing: std::_Exit(0)). Nothing is lost: the
//     main menu has no game in progress, settings are saved when Options
//     closes.
//   * No / B: the list comes back with "Quit Game" still selected; its
//     input comes back 0.3 s later (the press that answered must not count
//     for the list too: B would leave to the title, A ask again).
//
// Not translated yet: the item and the question are English in every text
// language (as the port's Options screen is).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace quit_game {

// Every front-end update (players/more_players_frontend.cpp, sub_82261C40):
// the item's words.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// The main menu's decision (options/options_menu.cpp, sub_8211B6C8) while
// the question is up or just answered No: true = answer "stay" this frame,
// no pick counts. (Yes ends the game from the screen's Update.)
bool HoldDecision(PPCContext& ctx, uint8_t* base);

}  // namespace quit_game
