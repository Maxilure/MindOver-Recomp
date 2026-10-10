// =============================================================================
// game_font.h -- the game's own text font, read from the disc data, for text
// the port draws ITSELF (over the finished picture, not through the game)
// =============================================================================
//
// WHY: some of our text must show no matter what the game is doing: the
// frame-rate counter (fps_overlay.h) once lived on the game's own menu and HUD
// pages, and vanished whenever the game hid them (every menu transition, the
// moment between two screens, movies). Text drawn by us, on the layer over the
// picture, depends on nothing; this keeps it in the game's look.
//
// WHERE THE FONT IS: package\b4c85fe7.p3d in default.rcf (the "persistent"
// menu package, findings/30). Two Pure3D texture fonts, chunk 0x22000:
//   +12 u32 version, +16 name length, +17 name ("Titans_Large" / "Titans_Small")
//   children: 0x19000 textures (the font's PAGES, in page-number order:
//     "Titans_final_64_texture_0".."_21" for Small), each 0x19001 image ->
//     0x19002 image data = u32 size + a whole TGA file: type 1 (colour
//     mapped), 256 BGRA palette entries, 8-bit pixels, descriptor 0x20 (rows
//     top to bottom). Text pages are 256 x 256.
//   0x22001 glyph table: +12 u32 count, then 40 bytes per glyph:
//     u32 page (bit 31 = a BUTTON PICTURE, a whole page: the prompt glyphs)
//     f32 u0, v0, u1, v1   the glyph's cell on its page (v counted from the
//                          BOTTOM: image row = (1 - v) x height)
//     f32 left, right      bearings in pixels (the space around the ink)
//     f32 width, advance   cell width and pen advance (= left + width + right)
//     u32 character        (UTF-16 code: 'A', the prompts' U+00A5..U+00BE)
//   Titans_Small: cell 59 px high; Titans_Large: 89 px.
// The pages are WHITE: the palette's colour is (255, 255, 255) everywhere and
// only alpha changes: ~254 = the letter, ~104 = a baked outline ring around it
// (the game tints it). FillAndOutline() splits that into two layers, like the
// game's look: a dark outline under the coloured letter.
//
// Game data: read from the player's own disc files at run time, never saved.
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace game_font {

// One character of the font.
struct Glyph {
  uint32_t page = 0;            // which page picture
  float u0 = 0, v0 = 0;         // its cell, TOP-LEFT origin (0..1 of the page)
  float u1 = 0, v1 = 0;
  float left = 0, width = 0;    // pixels at the font's own size
  float top = 0, height = 0;    // the part of the cell drawn: from `top` pixels below
                                // the cell's top (Load skips row 0: game_font.cpp)
  float advance = 0;
};

// A page as two RGBA pictures (R8G8B8A8, rows top to bottom), white with
// the alpha of one layer each.
struct PageLayers {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> fill;     // the letters only
  std::vector<uint8_t> outline;  // letters + their outline ring (drawn under, dark)
};

class Font {
 public:
  // Reads `name` ("Titans_Small") out of the game's archive. False (and a
  // log line) if the data isn't there or isn't as expected.
  bool Load(std::string_view name);
  bool loaded() const { return !glyphs_.empty(); }

  // The glyph of a character (null: the font has none).
  const Glyph* Find(char16_t c) const;

  // Cell height in pixels (every text glyph's cell is this tall), and the
  // height of a capital letter's ink inside it (for sizing text).
  float cell_height() const { return cell_height_; }
  float cap_height() const { return cap_height_; }

  // Width in pixels of a line at the font's own size.
  float Measure(std::u16string_view text) const;

  size_t page_count() const { return pages_.size(); }
  // Page `i` split into its two layers (made on first use, kept).
  const PageLayers& Layers(uint32_t page);

 private:
  struct Page {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> alpha;  // one byte per pixel (the palette's alpha)
    PageLayers layers;
  };
  std::vector<Page> pages_;
  std::unordered_map<char16_t, Glyph> glyphs_;
  float cell_height_ = 0, cap_height_ = 0;
};

}  // namespace game_font
