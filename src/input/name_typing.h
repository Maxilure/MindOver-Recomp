// =============================================================================
// input/name_typing.h -- typing a name on the keyboard (New Game's name, the
// save list's rename screen), and the mouse on the on-screen keyboard
// =============================================================================
//
// WHAT THE PLAYER GETS (2026-10-15): on the game's name screen (findings/24
// s.5: CNameEntryScreenAction, a 4 x 11 on-screen keyboard), the physical
// keyboard TYPES: letters, digits, space and punctuation go straight into the
// name, in the keyboard's own layout, Shift for capitals. Enter = Done, Esc =
// Back, Backspace = erase; the arrow keys still move the on-screen cursor.
// The gameplay keys are quiet meanwhile (typing W types a W). The mouse:
// pointing at an on-screen key moves the cursor there, a click chooses it.
// A controller works as before. The prompt bar says Backspace / Done /
// Select with the KEYS' pictures while the keyboard is in use.
//
// HOW: the keyboard driver's text mode (keyboard_mouse.h SetTextMode: SDL
// text input on, typed characters kept). Each typed character goes in
// through the screen's OWN "type the selected key" (sub_820D6618: the
// 16-letter room check, the buffer at 0x825A06F8, the name field refresh,
// the letter_select sound), with the "character under the cursor"
// (sub_820D6450, a table lookup by character set, caps, row, column) answering
// that character for the one call. A character the game's font (Titans_Large)
// can't draw is dropped. The on-screen keyboard's SPACE: its erase key (row
// 1, column 10: the left arrow; Backspace, X and the prompt erase already)
// types a space, and its picture is the game's arrow mirrored to point right
// (a data patch of the menu packages: name_typing.cpp "The space key").
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace name_typing {

// The name screen's Update while it's TYPING (state 3; saves/save_library.cpp
// wraps sub_820D58B8): `self` = the CNameEntryScreenAction. Before the
// game's Update. Main thread.
void Frame(PPCContext& ctx, uint8_t* base, uint32_t self);

// The screen isn't typing any more (closing, or its Exit): the keys play again.
void Leave();

// Registers the space key's picture (a data patch). Before data_patcher::Install(),
// after saves/rename_screen.h Register (whose patch copies the keyboard's
// pictures into the in-game menus).
void Register();

}  // namespace name_typing
