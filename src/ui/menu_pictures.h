// =============================================================================
// ui/menu_pictures.h -- pictures of the port's own for the game's menu pages
// (Scrooby), in the game's own picture format
// =============================================================================
//
// WHY: a picture element on a menu page shows a SPRITE by name, and the
// sprites live in the same package as the page. The port's pages (the
// Options / Achievements screen, options/options_page.cpp) need pictures the
// disc's packages don't have (the achievements' own pictures, read from the
// player's XEX at run time): made here, added to the package by its data
// patch, drawn by the game like any of its own (in the picture, under our
// ImGui windows, sliding and fading with the page).
//
// THE GAME'S SPRITE (read 2026-10-25 from FE_star_open.tga in an in-game
// menus package):
//   0x19005 sprite   name, u32 640, u32 480, u32 0, u32 0, an empty name (a
//                    single 0 byte), u32 1 (images), u32 0
//     0x19001 image  name, u32 0x36B0 (version), u32 width, u32 height,
//                    u32 8 (bits per pixel), u32 1 (palettized), u32 1, u32 2
//       0x19002 data u32 size + a whole TGA file: type 1 (colour mapped),
//                    256 palette entries of 32 bits (B G R A), 8-bit pixels,
//                    descriptor 0x20 (rows top to bottom)
// Every menu picture on the disc is palettized like that, so ours are too
// (256 colours by median cut: plenty for a 64 x 64 picture).
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace menu_pictures {

using Bytes = std::vector<uint8_t>;

// RGBA (rows top to bottom) -> a TGA as the game's sprites hold it: 256
// colours (median cut), entry 0 fully transparent.
Bytes PaletteTga(uint32_t width, uint32_t height, const uint8_t* rgba);

// A whole 0x19005 sprite chunk named `name` ("AchIcon17.tga") with that TGA.
Bytes MakeSprite(std::string_view name, uint32_t width, uint32_t height, const Bytes& tga);

}  // namespace menu_pictures
