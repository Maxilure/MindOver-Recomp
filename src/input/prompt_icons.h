// =============================================================================
// input/prompt_icons.h -- keyboard and mouse pictures in the game's own button
// prompts ("Jump ±" shows the Space key instead of the 360's A button)
// =============================================================================
//
// WHAT THE PLAYER SEES: whenever the keyboard or mouse was used last, every
// button prompt the game draws (HUD tips, menus' button bar, "Press ² to
// Jack", the loading screens...) shows the KEY bound to that move, as a
// picture of a keyboard key or a mouse; when a controller was used last, the
// game's own 360 buttons come back. A new binding (F6) shows up at once.
//
// THE PICTURES: Xelu's free controller and key prompts ("Light" keyboard and
// mouse set), by Nicolae "Xelu" Berbece, https://thoseawesomeguys.com/prompts/
// -- Creative Commons 0 (public domain). In assets/prompts/keyboard-mouse/,
// copied next to the exe by CMake. Not game data.
//
// HOW THE GAME DRAWS A PROMPT (findings/23 s.1.2, findings/37): the text bank
// writes prompts with private characters (U+00A5..U+00BE). The game's two
// fonts (Pure3D TextureFont "Titans_Large" / "Titans_Small", in
// package\b4c85fe7.p3d) have one glyph for each of those characters whose
// "page" is a whole PICTURE of its own (bit 31 of the glyph's page number):
// the A button, the RT trigger, the left stick... So the prompt pictures are
// 13 ordinary font pages per font, each an 8-bit palettized texture.
//
// WHAT WE CHANGE: those pages' PIXELS, in the game's memory, nothing else.
// A page is 8-bit indices (64 x 64 for buttons, 128 x 128 for the sticks,
// 128 x 64 for the "rotate" arrow), stored LINEAR (rows of 256 bytes), plus a
// 256-colour palette texture (k_8_8_8_8, one row). We draw the key picture at
// the page's size, reduce it to 256 colours, and write indices + palette in
// place; the game's own code then draws prompts as always, so both pictures
// (emulated and native), F10 photos and every screen show the keys, with no
// change to the game's text or layout. The original bytes are kept and put
// back when a controller is used again.
//
// FINDING THE PAGES: Pure3D's TextureFontLoader (vtable 0x8200E420, slot 5 =
// LoadObject sub_823C5DA0) builds the font and stores it through its r4
// argument. The font (vtable 0x8200E394) keeps its page count at +60 and an
// array of pure3d::Texture pointers at +64; a Texture's +24 is its
// pddiTexture (xnTexture, native/guest.h). Before writing anything, every page
// is CHECKED against the same page read from the disc (indices and palette
// must match exactly): a page we can't recognise is left alone (logged).
//
// WHICH KEY FOR WHICH PICTURE (bindings.h actions; the first key of each):
//   ± A    Jump             ² B    Jack / use
//   ³ X    Light attack     ´ Y    Heavy attack
//   µ Back Map              ¶ Start Pause
//   · RT   Block            ¸ RB   Pocket
//   ½ LT   Titan special    ¾ LB   Left bumper
//   ¹ left stick  = the four Move keys (W above A S D)
//   ¥ right stick = the four Right stick keys
//   ¼ "rotate"    = the Spin key and a circling arrow (the prompts write a
//                   spin as "¹¼": stick + rotate)
//   º » (the menus' red arrows) stay as they are.
// An action without a key keeps the 360 picture.
//
// MENUS: on a controller, A both jumps and confirms; on the keyboard those
// are two keys (Space / Enter, E / Escape for B, Q / E for LB / RB...: the
// menus have keys of their own, input/bindings.h "PLAY AND MENUS"). The menus' prompt
// bar (page FE_Buttons: "Back ²  Select ±") and the texts in the same menus
// ("Stun an enemy and Press ² to Jack", Pause -> Tutorials) are on screen
// TOGETHER, so one picture per character can't be right for both. A data
// patch of the font package (data/data_patcher.h) adds six glyphs to both
// fonts, the menus' A B X Y LB RB (U+E0B1 E0B2 E0B3 E0B4 E0BE E0B8, Unicode's
// private use area), with pages of their own (copies of those pictures);
// each frame, the prompt bar's button texts get them instead of the play
// glyphs (FrontEndFrame), and our own menus write them (MenuGlyph). So the
// bar shows Enter / Escape while the texts show Space / E; on a controller
// both are the 360's A and B.
//
// --button_prompts: 0 = automatic (the device used last), 1 = always keys,
// 2 = always the controller's buttons.
//
// STYLES (2026-10-15): --keyboard_icons 0 / 1 = Xelu's Light / Dark keys;
// --controller_icons = which controller's buttons: 0 automatic (by the maker
// and model of the controller used last, read from its SDL GUID: Sony
// DualSense = PlayStation 5, other Sony = PlayStation 4, Nintendo = Switch,
// Xbox One / Series = Xbox Series; a 360 pad or any other maker = the game's
// own pictures), 1 the game's own Xbox 360 buttons, 2 Xbox Series, 3
// PlayStation 5, 4 PlayStation 4, 5 Switch (Xelu's sets, assets/prompts/).
// The controller sets go into the same pages the keys do, by button
// POSITION (the 360's A, the bottom button, is Cross / Switch's B).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

struct PPCContext;

REXCVAR_DECLARE(int32_t, button_prompts);
REXCVAR_DECLARE(int32_t, keyboard_icons);
REXCVAR_DECLARE(int32_t, controller_icons);

namespace prompt_icons {

// Registers the font patch (the two menu glyphs). Before data_patcher::Install().
void Register();

// Registers the frame-end listener that keeps the pages up to date. Once,
// before the game starts (CrashMomApp).
void Install();

// Every front-end update (players/more_players_frontend.cpp, sub_82261C40;
// main thread, in menus and in game): the prompt bar's ± and ² become the
// menu glyphs.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// The menus' own glyph for a prompt character (± -> menu A, ² -> menu B, ³ ´
// ¾ ¸ likewise), or `c` itself (another character, or the font patch isn't
// in). For menus the port builds (options/options_menu.cpp). Any thread.
char16_t MenuGlyph(char16_t c);

// A picture of `key` (a keyboard key / mouse button, kbm::Input) as a
// character of the game's fonts, for texts the port writes (the Options
// screen's key rows): one of 16 KEY SLOTS our font patch adds (U+E100..),
// lent to the key until 16 others were asked for since; its page shows the
// key in the chosen key set from the next frame on. 0 = no slots (the font
// patch isn't in): write the key's name instead. Game main thread.
char16_t KeyGlyph(uint16_t key);
// The same for a CONTROLLER button, by its 360 name as input/bindings.h
// writes it ("A", "RT", "Back", "Left stick"...): drawn in the Controller
// Icons style (Automatic: the controller used last; the game's own 360
// picture when that's the style or nothing better is known). 0 = no slots
// or an unknown name. Game main thread.
char16_t ButtonGlyph(std::string_view pad);

// Are the prompts showing keys right now (the keyboard / mouse was used last,
// or --button_prompts says so)? For screens that pick their own prompts
// (input/name_typing.h). Any thread.
bool KeysShown();

// The name ("Titans_Large", "Titans_Small") of the game's font object at
// `font` (a pure3d::TextureFont the loader built), "" if it isn't one of
// them. For measuring texts (input/mouse_menus.h). Any thread.
std::string FontNameAt(uint32_t font);

// The pad state the game just got for `user` (guest X_INPUT_GAMEPAD, 12
// big-endian bytes), from the XInputGetState wrapper (saves/save_library.cpp):
// input the keyboard didn't make = a controller was used. Game threads.
void SeePad(uint32_t user, const uint8_t* gamepad);

}  // namespace prompt_icons
