// =============================================================================
// ui/font_letters.h -- letters the game's font has no picture for
// =============================================================================
//
// WHY: the game's two fonts (Titans_Large / Titans_Small in
// package\b4c85fe7.p3d, findings/49 s.7-8) lack some characters:
//   * LISTED BUT EMPTY on the disc (zero width, no picture, only an advance):
//     Ã ã " * < > ^ _ ` { | } ~ † -- never used by the game's own text, but
//     translations and subtitles need them (Brazilian Portuguese: "não");
//   * NOT LISTED AT ALL: Ё ё (Russian), « » (quotes in French, Russian...).
//     « » can't even be added under their own codes: U+00A5-00BE are the
//     font's BUTTON PICTURES (» U+00BB = an arrow): they get private codes
//     (kLeftQuote / kRightQuote), and ForFont() swaps them in texts the port
//     loads (translations, subtitles) before the game ever sees them.
//
// HOW (a data patch of b4c85fe7, after prompt_icons' "menu button glyphs";
// game_font.cpp runs the same patch on the disc copy it reads for our
// overlays): each letter is COMPOSED FROM THE FONT'S OWN PICTURES, the
// outline included, so it has the game's look:
//     Ã ã Ё ё   A a Е е + the tilde of Õ õ / the dots of Ë ë
//     " ~ ^ `   two apostrophes / õ's tilde (at the dash's height) / â's hat / à's accent
//     _ | †     the dash widened under the baseline / I's bar made tall / both crossed
//     * < >     three dashes turned 90/30/150 degrees / the v on its side
//     « »       two v's on their side, 0.8x, overlapping
//     { }       ( ) + a short nub of the dash at the middle
// (findings/49 s.8.4 has pictures of both sizes.) The cells are drawn into
// free space of the text pages; when that runs out, the last text page is
// made twice as tall (Titans_Small already has the game's maximum of 64
// pages, so no new page). Colours are mapped to the page's palette.
//
// Page format (0x19000 texture > 0x19001 image > 0x19002 data = u32 size +
// a TGA: colour-mapped 8 bit, 256 x 32-bit BGRA palette, origin top left);
// glyph v coordinates run bottom-up: pixel row = (1 - v) x page height.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "../data/data_patcher.h"

namespace font_letters {

// « and » in the font (private codes: the real ones are button pictures).
constexpr char16_t kLeftQuote = 0xE140, kRightQuote = 0xE141;

// Registers the data patch (after prompt_icons::Register: it runs on that
// patch's output).
void Register();

// The patch itself, on a whole b4c85fe7 package (false = nothing changed).
// game_font.cpp calls it on the disc's copy, so the overlays draw the same letters.
bool PatchPackage(const data_patcher::Bytes& original, data_patcher::Bytes* patched);

// Pages this patch added to a font: always 0 now (it grows a page instead);
// prompt_icons checks a loaded font's page count with it.
uint32_t AddedPages(bool large);

// « » -> kLeftQuote / kRightQuote; anything else unchanged. For texts the
// port gives the game (or draws with its font): never for the game's own
// text banks, where U+00AB-00BB are button pictures.
char16_t ForFont(char16_t c);
void ForFont(std::u16string& text);

}  // namespace font_letters
