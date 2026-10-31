// =============================================================================
// ui/font_letters.h -- letters the game's font has no picture for (Ã ã)
// =============================================================================
//
// WHY: the game's two fonts (Titans_Large / Titans_Small in
// package\b4c85fe7.p3d, findings/49 s.7) list "Ã" and "ã" but their glyph
// records are EMPTY placeholders: zero width, no picture, only an advance
// (the same as " * < > ^ _ ` { | } ~, never used by the game's own text).
// Brazilian Portuguese needs ã everywhere ("não", "missão"): it drew a gap.
//
// HOW (a data patch of b4c85fe7, after prompt_icons' "menu button glyphs"):
// each letter is composed from the font's OWN pictures: the "a" / "A" cell
// plus the tilde of "õ" / "Õ" (the rows of the õ cell above the top of a
// plain "o"), centred, the cell widened to the wider of the two. It is drawn
// into empty space of a text page (Small: the bottom of page 6), else into
// one new page (Large: no free spot 61 x 91 pixels anywhere), a copy of a
// text page cleared to transparent, appended after the last page. Colours
// are mapped to the destination page's palette (nearest entry; the pages'
// palettes differ). The glyph record takes the base letter's metrics, the
// new place, the wider width, the left bearing moved by half the widening.
//
// Page format (0x19000 texture > 0x19001 image > 0x19002 data = u32 size +
// a TGA: colour-mapped 8 bit, 256 x 32-bit BGRA palette, origin top left);
// glyph v coordinates run bottom-up: pixel row = (1 - v) x 256.
// =============================================================================
#pragma once

#include <cstdint>

namespace font_letters {

// Registers the data patch (after prompt_icons::Register: it runs on that
// patch's output).
void Register();

// Pages this patch added to a font (0 or 1): prompt_icons checks a loaded
// font's page count against the disc's + its own + these.
uint32_t AddedPages(bool large);

}  // namespace font_letters
