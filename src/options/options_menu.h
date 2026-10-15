// =============================================================================
// options/options_menu.h -- the port's in-game OPTIONS screen (Pause ->
// Options), in the game's own menu look, with tabs; every change is live
// =============================================================================
//
// WHAT THE PLAYER SEES: Pause -> Options opens the glass panel as before,
// now with a row of tabs (Display, Audio, Controls, Camera; LB / RB or Back
// switch them), up to five rows per tab (left / right changes a value on the
// spot), and beside them a CARD (a square glass panel on the right, since
// 2026-10-11): the selected value, its description, a yellow warning when a
// value changes how the game plays, and a picture where a row has one. The game's own
// options (the three volumes as stars, Invert Axis) are rows of the Audio
// and Controls tabs. What the rows are and do: options_settings.h. The page
// itself: options_page.h (a data patch of the in-game menus package).
//
// HOW IT HOOKS IN (traced 2026-10-10, findings/30): the Options state (956)
// runs two actions: CInGameOptionsScreenAction (the panel's slide-in, the
// "P1 Paused" title) and a CMenuAction (the generic menu driver every Scrooby menu screen uses: Enter sub_820D2828, Update
// sub_820D2A00, Exit sub_820D2F08). CMenuAction does up / down (with the
// "down_left" / "up_right" sounds), left / right on value lists, and has
// SPECIAL CODE for a menu named InGameOptionsMenu (its name hash is compared
// with the global 0x825A0598): stars by row number 0-2, Invert Axis on row
// 3, the Music preview on row 1. Our page keeps that menu name (the state's
// tracks find the menu by it), so around the original Enter / Update of OUR
// menu that global is zeroed: the special code doesn't recognize the menu,
// the generic part runs, and the rows are ours.
//
// Input: up / down = the game's (CMenuAction). Left / right = the front
// end's IsButtonPressed buttons 27 / 40 (the same ones CMenuAction asks;
// through players/menu_input.cpp's hook: the menu's owner answers). Tabs (LB
// / RB / Back) and A on an action row = the owner's controller as the game
// last read it (SeePad), pressed this frame.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace options_menu {

// Registers the page patch (CrashMomApp::OnPostSetup, before
// data_patcher::Install()).
void Register();

// Every controller state the game reads (the XInputGetState wrapper,
// saves/save_library.cpp), after every filter: `gamepad` = XINPUT_GAMEPAD in
// guest memory (big-endian).
void SeePad(uint32_t user, const uint8_t* gamepad);

// The Options screen is up (for other code that wants to stay out of its way).
bool Open();

// A key row is waiting for a new key (A on an action's row, Controls tab):
// the controllers must say nothing to the game meanwhile. Any thread.
bool Capturing();
// From the XInputGetState wrapper, after SeePad (which still sees a B =
// cancel): zeroes the pad while Capturing(), and afterwards hides the
// buttons that were held then until they're let go. Game threads.
void FilterPad(uint32_t user, uint8_t* gamepad);

// PAGES: the menu's own move (Scrooby Menu::MoveNext sub_8237A668 / Previous
// sub_8237A550, hooked by saves/save_library.cpp) asks this first: moving
// past the last / first row of a tab's page turns to the next / previous page
// (wrapping around) and returns true (= "moved": the menu plays its sound).
// False for any other menu, or when the move stays on the page.
bool TurnPage(uint32_t menu, int direction, uint8_t* base);

// THE GLASS MOVES LEFT (2026-10-11): the in-game screen's action
// (CInGameOptionsScreenAction) Enter, after the original (players/
// menu_input.cpp wraps it for its title): the game's glass panel page, which
// the pause menu shares, moves left by options_page::kGlassShift until the
// screen's Exit (sub_820CDD60, wrapped here) puts it back. `action` = r3.
void MoveGlassForCard(PPCContext& ctx, uint8_t* base, uint32_t action);

// The main menu's Options: B starts the panel's slide-out (the PS2 screen
// action's closing, whose end sets the cancel flag). False if the page has
// no slide (then the caller sets the flag itself).
bool CloseMainMenuOptions(PPCContext& ctx, uint8_t* base);

}  // namespace options_menu
