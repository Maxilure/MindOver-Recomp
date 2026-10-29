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
//   InGameOptionsMenu           items OptRow0..N-1: label OptLabel<r> (left
//                               aligned, glass's left) + value OptValue<r>
//                               (right aligned, glass's right)
//   OptStar<r>_<k>              5 star pictures on each row (volumes), at
//                               the row's right
//   OptTitle                    (main menu page) "Options", where the in-game
//                               page has the game's "Paused" title
//   OptPageUp / OptPageDown / OptPage  under the rows: more rows above /
//                               below, and "1/2" (pages)
//   THE CARD (2026-10-11): a glass panel on the right, beside the
//   game's glass (moved left by kGlassShift), about the selected row:
//   OptCardSmall / Large / Square  the glass in the game's three shapes
//                               (4:1, 2:1, 1:1): the one nearest the card's
//                               shape is shown, sized to fit the contents
//   OptCardValue                its value (yellow)
//   OptGuide                    a picture: the Brightness row's guide, the
//                               original's calibration grey scale
//   OptDesc                     its description (cyan, small print)
//   OptWarn                     its warning (yellow, small print)
// =============================================================================
#pragma once

namespace options_page {

// Rows shown at a time (the page has this many; unused ones are hidden). A
// tab with more rows is shown in PAGES of this many (options_menu.cpp).
constexpr int kRows = 7;
constexpr int kStars = 5;
// THE LAYOUT, two glass panels centred together on the 16:9 picture (x
// -107-747 in Scrooby units), the same margin (~66) on both sides; inside
// the main menu's purple frame too (its opening spans about x -53-690:
// a first layout 22 units wider went under the frame on the right):
//   the game's glass panel  500 wide, moved LEFT by kGlassShift: x -40-460
//   (in game options_menu.cpp moves the game's own glass page, shared with
//   the pause menu, and the button prompts, while the screen is up)
//   a gap of 10, then the CARD  kCardWidth wide: x 470-680, its height fitted
//   to its contents by options_menu.cpp, centred on the glass's centre (y 211)
constexpr int kGlassShift = -110;
constexpr int kCardX = 470, kCardWidth = 210;
constexpr int kCardCentreY = 211;
constexpr int kCardPadding = 14;  // text's room from the card's edges (each side)
// A row's value text: a box this wide, right aligned, ending at x
// kRowValueRight (the glass's right edge, 30 inside: options_page.cpp).
constexpr int kRowValueWidth = 250;
constexpr int kRowValueRight = 530 + kGlassShift;

// Registers the patch (before data_patcher::Install()).
void Register();

// Whether the game got our page (the in-game menus package was patched).
bool Available();
// Whether the main menu got its "Options" item and our page (as its third
// item, the page in place of the unused PS2 Options page: options_page.cpp).
bool MainMenuAvailable();
// The main menu's items with it: New Game, Load Game, OPTIONS, PLAYERS,
// Credits, QUIT GAME (ui/quit_game.h). (Achievements: the pause menu only.)
constexpr int kMainMenuOptionsItem = 2;
constexpr int kMainMenuPlayersItem = 3;
constexpr int kMainMenuQuitItem = 5;
// Whether the pause pages got their "Players" and "Achievements" items
// (right after Options, in that order; options_page.cpp AddPauseItems).
bool PauseAchievementsAvailable();
// Whether the pages have the Players screen's cards (the device pictures
// were made: players/players_setup.h MenuSprites).
bool PlayersCardsAvailable();
// THE ACHIEVEMENTS LIST (options_page.cpp; options_menu.cpp fills it), in
// page units, y up: kAchCards glass cards INSIDE the game's glass panel,
// which stays centred for the list (x 70-570, y 34-388: the Options screen
// moves it left for its side card, the list doesn't), under the heading
// (the tabs' line, y 312), the list's arrows beside them on the right.
constexpr int kAchCards = 3;
// (Full size since 2026-10-29, like the pause menu: 70 tall, 78 apart from
// y 304, so the last card ends at y 78, above the prompts' line, y 42-69.)
constexpr int kAchCardX = 85, kAchCardW = 438, kAchCardH = 70, kAchCardStep = 78, kAchCardsTop = 304;
constexpr int kAchIconSide = 60, kAchIconX = kAchCardX + 12;
constexpr int kAchTextLeft = kAchIconX + kAchIconSide + 14;    // 171
constexpr int kAchTextRight = kAchCardX + kAchCardW - 16;     // 507
constexpr int kAchHeadingY = 312;
constexpr int kAchArrowX = 530;  // the arrows' box left (32 wide), beside the cards (glass edge 570)
// THE PLAYERS SCREEN (players/players_setup.h; options_page.cpp builds,
// options_menu.cpp fills; the design of 2026-10-29, its 1280 x 720 pixels
// / 1.5 = units): four cards, two by two, filling the game's glass panel
// (x 70-570, y 34-388: 17 inside it, 12 apart), each a centred column:
// "Player N", the device picture, the device's name. Card k: column k % 2,
// row k / 2. The prompts in a centred row under the panel.
constexpr int kPlyCardW = 227, kPlyCardH = 154;
constexpr int kPlyCardX[2] = {87, 326};
constexpr int kPlyCardTop[2] = {371, 205};
constexpr int kPlyRingMargin = 5;    // the selection ring round a card
constexpr int kPlyPicSide = 84;      // the device picture's box (square; the picture is ~70 x 45 inside)
// Each card's places, below the card's top, per ROW (2026-10-29, the
// screen's design: the player's NUMBER in the card's outer corner of the
// screen, over a glow of their colour, instead of a "Player N" title; the
// bottom row is the top row upside down: lines first, picture below, the
// number in the bottom corner, so the number never meets the lines).
// [0] = top row (players 1-2), [1] = bottom row (players 3-4).
constexpr int kPlyPicCentre[2] = {58, 94};  // the picture's centre
constexpr int kPlyInfoTop[2] = {100, 6};    // the name line's box top
constexpr int kPlyStyleTop[2] = {122, 28};  // the button pictures line (PlyStyle)
constexpr int kPlyNumberCentreY[2] = {30, 124};  // the number's centre
constexpr int kPlyNumberInset = 30;         // its centre, from the card's outer side
constexpr int kPlyArrowSide = 28;    // the chooser's arrows, beside the picture
constexpr int kPlyArrowInset = 26;   // from the card's left / right edge
constexpr int kPlyPrompts = 4;       // the prompt row: up to four
constexpr int kPlyPromptTop = 36;    // its boxes' top (the panel's bottom = 34)

}  // namespace options_page
