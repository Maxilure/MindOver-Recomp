// =============================================================================
// options/options_page.h -- the Options screen's PAGE, rebuilt in the game's
// own menu data (a data patch, data/data_patcher.h)
// =============================================================================
//
// WHAT: the in-game Options screen (Pause -> Options; front-end state 956,
// findings/30 s.3) shows the Scrooby page InGame_Options_XENON: a menu of
// four rows (Dialogue / Music / SFX stars, Invert Axis) on the glass panel.
// The port's Options screen (options_menu.h) needs more: a row of tabs, up
// to kRows rows per tab, star pictures on any row, a description and a
// warning line. This patch replaces that page, in every in-game menus
// package (one per level group: InGame.prj), with one made of COPIES of the
// page's own elements (the row texts, a star, the "Paused" title), renamed
// and moved. Same fonts, colours, pictures and menu behaviour as the game.
//
// THE MAIN MENU gets the same page (a second patch, of the main menus'
// package GameStart.prj): "Options" as the main menu's third item (in place
// of Calibration, a TV brightness test screen: its grey scale is now the
// Brightness row's guide), and our
// page in place of the PS2 Options page that the front end's OptionsScreen
// state (96, a PS2 route still in the Xbox data) shows; with the in-game
// glass panel, cogs and hinge drawn by the page itself (options_page.cpp).
//
// WHAT STAYS: the page's name, the menu's name InGameOptionsMenu (the
// front end's tracks name both, so the state, its screen action and its
// CMenuAction find them as before) and the "Paused" title (the screen
// action writes "P1 Paused" into it). The game's own code for that menu
// (stars, volumes, invert axis by row number) is kept away from our rows by
// options_menu.cpp.
//
// THE ELEMENTS (names = the contract with options_menu.cpp), Scrooby units
// (640 x 480, y up; at 1280 x 720: x = 160 + 1.5 u, y = 720 - 1.5 u):
//   OptTabPrev / OptTab / OptTabNext  the previous (faded), current (yellow)
//                               and next (faded) tab names
//   OptTabLB / OptTabRB         the shoulder button pictures beside them
//   InGameOptionsMenu           items OptRow0..N-1: label OptLabel<r>
//                               (right aligned) + value OptValue<r>
//   OptStar<r>_<k>              5 star pictures on each row (volumes)
//   OptArrowL<r> / OptArrowR<r> the selector's arrows on each row (the map
//                               screen's arrow picture, turned by code)
//   OptTitle                    (main menu page) "Options", where the in-game
//                               page has the game's "Paused" title
//   OptGuide                    the Brightness row's guide: the original's
//                               calibration grey scale (shown on that row)
//   OptPageUp / OptPageDown / OptPage  beside the glass on the right: more
//                               rows above / below, and "1/2" (pages)
//   OptDesc / OptWarn           the selected row's description (cyan, 2
//                               lines) and warning (yellow)
// =============================================================================
#pragma once

namespace options_page {

// Rows shown at a time (the page has this many; unused ones are hidden). A
// tab with more rows is shown in PAGES of this many (options_menu.cpp).
constexpr int kRows = 5;
constexpr int kStars = 5;

// Registers the patch (before data_patcher::Install()).
void Register();

// Whether the game got our page (the in-game menus package was patched).
bool Available();
// Whether the main menu got its "Options" item and our page (as its third
// item, the page in place of the unused PS2 Options page: options_page.cpp).
bool MainMenuAvailable();
// The main menu's items with it: New Game, Load Game, OPTIONS, Credits.
constexpr int kMainMenuOptionsItem = 2;

}  // namespace options_page
