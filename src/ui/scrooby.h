// =============================================================================
// ui/scrooby.h -- small helpers to drive the game's own menu library from C++
// =============================================================================
//
// WHAT: the game draws every menu with "Scrooby", Radical's 2D front-end
// library (docs/findings/30): pages of texts, pictures and menus, laid out
// in data and changed by code that finds elements BY NAME. Our menus (the
// Options screen, options/options_menu.h) are built the same way, so they
// look and behave exactly like the game's; these helpers wrap the handful of
// game functions they need. Everything here runs on the game's main thread,
// from inside one of our hooks (the PPCContext of that hook is borrowed for
// the calls and put back afterwards).
//
// THE GAME FUNCTIONS (findings/24, 26, 30):
//   sub_82371AD0 / A90 / A50 (page, name)  find a text / picture / menu
//   sub_82373360 (text, UTF-16)            set a text's string (copied into
//                                           the CURRENT heap: heap 7 around it,
//                                           as the game's own screens do)
//   sub_823574E8 (heap)                    make a heap current, returns the old
//   element +110 bit 0x80                  visible
//   menu item +137 bit 0x80                selectable (up / down skip the rest)
//   menu +112 / +116                       the item list (begin / end)
//   menu +152 (signed byte)                the selected item
//   sub_8237A4C0 (menu, index)             select an item
//   sub_82370A58 (element)                 reset its transform
//   sub_82371198 (element, f1 scale)       scale it around its centre
//   sub_82370EE8 (element, dx, dy)         move it (Scrooby units)
//   sub_823710C8 (element, f1 angle, r5 axis)  turn it around its centre
//   sub_82373F28 (picture, frame)          show one of a picture's frames
//   sub_82370840 (element, colour)         its colour (0xAARRGGBB)
//   sub_82261800 (front end, bank, sound, f1, f2, f3)  play a menu sound
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc/context.h>

namespace scrooby {

// A game function as the recompiled code defines it.
using GuestFunction = void (*)(PPCContext&, uint8_t*);

// Calls a game function with r3-r5 (and f1-f3) set; the caller's registers
// are put back afterwards. Returns r3.
uint32_t Call(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3,
              uint32_t r4 = 0, uint32_t r5 = 0);
uint32_t CallF(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3, double f1,
               double f2 = 0.0, double f3 = 0.0);

// Strings in guest memory for the game's functions (a pool from the system
// heap, each distinct string stored once, never freed).
uint32_t GuestAscii(std::string_view text);
uint32_t GuestUtf16(std::u16string_view text);

// Elements of a page by name (0 if missing).
uint32_t FindText(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name);
uint32_t FindPicture(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name);
uint32_t FindMenu(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name);

// A text's string (no-op on 0). "\n" breaks lines; the font draws button
// pictures for some characters (findings/23: U+00B1 A, U+00B2 B, U+00B3 X,
// U+00B4 Y, U+00B8 RB, U+00BE LB ...).
void SetText(PPCContext& ctx, uint8_t* base, uint32_t text, std::u16string_view value);

void SetVisible(uint8_t* base, uint32_t element, bool visible);
bool Visible(uint8_t* base, uint32_t element);
// Scales an element around its centre (1 = its own size); resets any other
// transform it had.
void SetScale(PPCContext& ctx, uint8_t* base, uint32_t element, float scale);
// Turns an element around its centre by `degrees` (in the screen's plane;
// the game turns its cogs this way: sub_823710C8 with the axis (0, 0, 1)).
// Adds to its current transform.
void Rotate(PPCContext& ctx, uint8_t* base, uint32_t element, float degrees);
void SetFrame(PPCContext& ctx, uint8_t* base, uint32_t picture, uint32_t frame);
void SetColour(PPCContext& ctx, uint8_t* base, uint32_t element, uint32_t argb);

// Menus.
int ItemCount(uint8_t* base, uint32_t menu);
uint32_t Item(uint8_t* base, uint32_t menu, int index);
int Selection(uint8_t* base, uint32_t menu);
void Select(PPCContext& ctx, uint8_t* base, uint32_t menu, int index);
void SetSelectable(uint8_t* base, uint32_t item, bool selectable);

// The front end's menu sounds (the "FE_SFX" bank).
enum class Sound { kMove, kChange };
void PlaySound(PPCContext& ctx, uint8_t* base, Sound sound);

}  // namespace scrooby
