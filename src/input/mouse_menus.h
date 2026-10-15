// =============================================================================
// input/mouse_menus.h -- the mouse in the game's menus: point, click, scroll
// =============================================================================
//
// WHAT THE PLAYER GETS: in every menu built on the game's generic menu
// driver (main menu, pause menu, Options, difficulty, question boxes...):
//   * the pointer over an item SELECTS it (with the game's move sound);
//   * a left click on an item CHOOSES it (the menu gets an A press);
//   * a right click goes BACK (B), anywhere;
//   * a wheel notch is one step up / down (the D-pad);
//   * a left click on a prompt of the menus' prompt bar presses its button
//     ("Back", "Rename", "Delete"...); the Options screen's tabs and the
//     save list's panels are clickable too.
// The keys keep working; hovering follows the pointer only when it MOVES,
// so a resting pointer never fights the arrow keys.
//
// HOW (findings/39): the game's menus are Scrooby menus (findings/30) driven
// by CMenuAction (Update = sub_820D2A00, run every frame the menu's screen is
// up; +20 = its Scrooby menu). A menu's items are at menu +112 (list); each
// item's look, the text drawn for it, is at item +140, and an element's box
// is +92 / +96 (x / y of its bottom-left corner, in 1/640 of the width:
// divide by the constant at 0x8204701C) and +88 / +90 (width / height, u16),
// in the 640 x 480 canvas, y up. The canvas is drawn 4:3 in the middle of the
// 16:9 picture: picture x = (160 + 1.5 x) / 1280, y = (720 - 1.5 y) / 720.
// The pointer (input/keyboard_mouse.h Pointer) is mapped the other way and
// tested against each visible, selectable item's box.
//
// The buttons go into the game like the keys do (KeyboardMouseDriver::Pulse,
// merged into the keyboard's socket), so the game's own menu code does the
// selecting, the sounds and the screen changes.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace mouse_menus {

// From the CMenuAction::Update wrapper (options/options_menu.cpp), before
// the game's Update runs: `action` = the CMenuAction. Main thread.
void OnMenuUpdate(PPCContext& ctx, uint8_t* base, uint32_t action);

// Hover selects, a left click chooses, for any Scrooby menu: item i is hit
// by targets[i] (an element on the page, e.g. a save panel's glass), or by
// its own look when `targets` is null or targets[i] is 0. `page` (0 = none):
// the Scrooby page they're on, whose own move / scale applies to them (the
// Options page is drawn at 0.85x in game); `layer` (0 = none): a layer of
// that page with a move / scale of its own (the main menu's Options shrinks
// its "OptionsMenu" layer instead), applied first. Main thread.
void HoverAndClick(PPCContext& ctx, uint8_t* base, uint32_t menu, const uint32_t* targets,
                   uint32_t page = 0, uint32_t layer = 0);

// The same for a menu the PORT drives (the Options screen): hover selects,
// and a left click on an item is RETURNED (its index, -1 = none) instead of
// pressing A, so its code can act at once (each click counts, however fast).
int HoverAndTakeClick(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t page = 0,
                      uint32_t layer = 0);

// Every front-end update (main thread): the game's question box when it
// shows (its Yes / No; nothing under it takes the mouse then), else the
// menus' prompt bar (page FE_Buttons: "Back B", "Select A", "Rename X"...):
// a left click on a prompt presses its button.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// For screens with clickable pieces of their own (the Options tabs): is the
// pointer over `element`? / take a waiting left click made over it.
// `page` as for HoverAndClick.
bool PointerOver(uint8_t* base, uint32_t element, uint32_t page = 0, uint32_t layer = 0);
// Goes up at every pointer move (hover only follows a pointer that moved).
uint32_t PointerMoves();
bool TakeClickOver(uint8_t* base, uint32_t element, uint32_t page = 0, uint32_t layer = 0);

}  // namespace mouse_menus
