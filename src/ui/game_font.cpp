// =============================================================================
// game_font.cpp -- see game_font.h
// =============================================================================
#include "game_font.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"

namespace game_font {
namespace {

using pure3d::Bytes;
using pure3d::Chunk;
using pure3d::Le32;

constexpr std::string_view kPackage = "package\\b4c85fe7.p3d";
constexpr uint32_t kChunkTextureFont = 0x22000;
constexpr uint32_t kChunkGlyphs = 0x22001;
constexpr uint32_t kChunkTexture = 0x19000, kChunkImage = 0x19001, kChunkImageData = 0x19002;

// The two alpha levels baked into the pages (game_font.h): letter, outline.
constexpr int kFillAlpha = 254, kOutlineAlpha = 104;

float LeFloat(const uint8_t* p) {
  const uint32_t v = Le32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The font chunk's own name sits after a version word (+16 length, +17 text).
std::string_view FontName(const Bytes& d, const Chunk& c) {
  if (c.data_size < 17) return {};
  const size_t length = d[c.offset + 16];
  if (17 + length > c.data_size) return {};
  std::string_view name(reinterpret_cast<const char*>(&d[c.offset + 17]), length);
  return name.substr(0, name.find('\0'));
}

// The first chunk with `id` among `c`'s children (null: none).
const Chunk* ChildWith(const std::vector<Chunk>& list, uint32_t id) {
  for (const Chunk& c : list) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

// A texture chunk's TGA -> width, height and the alpha of every pixel.
bool DecodePage(const Bytes& d, const Chunk& texture, uint32_t* width, uint32_t* height,
                std::vector<uint8_t>* alpha) {
  const std::vector<Chunk> in_texture = pure3d::ChildrenOf(d, texture);
  const Chunk* image = ChildWith(in_texture, kChunkImage);
  if (!image) return false;
  const std::vector<Chunk> in_image = pure3d::ChildrenOf(d, *image);
  const Chunk* data = ChildWith(in_image, kChunkImageData);
  if (!data || data->data_size < 16 + 18) return false;
  const uint8_t* tga = &d[data->offset + 16];
  const size_t size = std::min<size_t>(Le32(&d[data->offset + 12]), data->data_size - 16);
  // Header: id length, colour map type 1, image type 1, map start/length
  // (u16), entry bits, x/y origin, width, height (u16), pixel bits, descriptor.
  if (size < 18 || tga[1] != 1 || tga[2] != 1 || tga[7] != 32 || tga[16] != 8) return false;
  const uint32_t map_length = uint32_t(tga[5] | tga[6] << 8);
  *width = uint32_t(tga[12] | tga[13] << 8);
  *height = uint32_t(tga[14] | tga[15] << 8);
  const bool top_down = (tga[17] & 0x20) != 0;
  const size_t palette_at = 18 + tga[0];
  const size_t pixels_at = palette_at + 4 * map_length;
  if (map_length == 0 || pixels_at + size_t(*width) * *height > size) return false;
  alpha->resize(size_t(*width) * *height);
  for (uint32_t y = 0; y < *height; ++y) {
    const uint32_t row = top_down ? y : *height - 1 - y;  // ours = top to bottom
    for (uint32_t x = 0; x < *width; ++x) {
      const uint8_t index = tga[pixels_at + size_t(row) * *width + x];
      // Palette entries are B, G, R, A.
      (*alpha)[size_t(y) * *width + x] = index < map_length ? tga[palette_at + 4 * index + 3] : 0;
    }
  }
  return true;
}

}  // namespace

bool Font::Load(std::string_view name) {
  Bytes d;
  std::string error;
  if (!data_patcher::ReadArchiveFile(kPackage, &d, &error)) {
    REXLOG_WARN("Game font: can't read {} ({})", kPackage, error);
    return false;
  }
  Chunk root;
  std::vector<Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) {
    REXLOG_WARN("Game font: {} isn't a Pure3D file", kPackage);
    return false;
  }
  for (const Chunk& font : top) {
    if (font.id != kChunkTextureFont || FontName(d, font) != name) continue;
    pages_.clear();
    glyphs_.clear();
    const std::vector<Chunk> parts = pure3d::ChildrenOf(d, font);
    for (const Chunk& c : parts) {
      if (c.id == kChunkTexture) {
        Page page;
        if (!DecodePage(d, c, &page.width, &page.height, &page.alpha)) {
          REXLOG_WARN("Game font: {}'s page {} isn't readable", name, pages_.size());
          return false;
        }
        pages_.push_back(std::move(page));
      } else if (c.id == kChunkGlyphs && c.data_size >= 16) {
        const size_t count = Le32(&d[c.offset + 12]);
        for (size_t i = 0; i < count && 16 + 40 * (i + 1) <= c.data_size; ++i) {
          const uint8_t* r = &d[c.offset + 16 + 40 * i];
          const uint32_t page = Le32(r);
          const uint32_t character = Le32(r + 36);
          if (page & 0x80000000u || character > 0xFFFF) continue;  // a button picture
          Glyph g;
          g.page = page;
          g.u0 = LeFloat(r + 4);
          g.v0 = 1.0f - LeFloat(r + 16);  // v from the bottom -> rows from the top
          g.u1 = LeFloat(r + 12);
          g.v1 = 1.0f - LeFloat(r + 8);
          g.left = LeFloat(r + 20);
          g.width = LeFloat(r + 28);
          g.advance = LeFloat(r + 32);
          // Row 0 of a cell can hold a pixel or two of the glyph above it on
          // the page (alpha ~60 on Titans_Small's '4'): the outline layer
          // would turn it into a dot over the letter. Every cell starts one
          // row lower (its row 0 is empty ink-wise otherwise).
          const float page_height = page < pages_.size() ? float(pages_[page].height) : 256.0f;
          g.top = 1.0f;
          g.height = (g.v1 - g.v0) * page_height - g.top;
          g.v0 += g.top / page_height;
          glyphs_[char16_t(character)] = g;
        }
      }
    }
    // Every glyph's page must exist; the cell height from 'H' (all text
    // cells share it), the capital height from its ink (the letter layer).
    for (const auto& [c, g] : glyphs_) {
      if (g.page >= pages_.size()) {
        REXLOG_WARN("Game font: {}'s glyph U+{:04X} names a missing page", name, uint32_t(c));
        glyphs_.clear();
        return false;
      }
    }
    const Glyph* h = Find(u'H');
    if (!h) {
      REXLOG_WARN("Game font: {} has no 'H'", name);
      glyphs_.clear();
      return false;
    }
    const Page& p = pages_[h->page];
    cell_height_ = h->top + h->height;
    const uint32_t x0 = uint32_t(h->u0 * p.width), x1 = uint32_t(h->u1 * p.width);
    const uint32_t y0 = uint32_t(h->v0 * p.height), y1 = uint32_t(h->v1 * p.height);
    int top_row = -1, bottom_row = -1;
    for (uint32_t y = y0; y < y1 && y < p.height; ++y) {
      for (uint32_t x = x0; x < x1 && x < p.width; ++x) {
        if (p.alpha[size_t(y) * p.width + x] > (kFillAlpha + kOutlineAlpha) / 2) {
          if (top_row < 0) top_row = int(y);
          bottom_row = int(y);
          break;
        }
      }
    }
    cap_height_ = top_row >= 0 ? float(bottom_row - top_row + 1) : cell_height_ * 0.6f;
    REXLOG_INFO("Game font: {} read ({} glyphs, {} pages, cell {:.0f} px, capitals {:.0f} px)",
                name, glyphs_.size(), pages_.size(), cell_height_, cap_height_);
    return true;
  }
  REXLOG_WARN("Game font: no font {} in {}", name, kPackage);
  return false;
}

const Glyph* Font::Find(char16_t c) const {
  const auto it = glyphs_.find(c);
  return it != glyphs_.end() ? &it->second : nullptr;
}

float Font::Measure(std::u16string_view text) const {
  float width = 0;
  for (char16_t c : text) {
    if (const Glyph* g = Find(c)) width += g->advance;
  }
  return width;
}

const PageLayers& Font::Layers(uint32_t index) {
  Page& page = pages_[index];
  PageLayers& l = page.layers;
  if (l.width) return l;
  l.width = page.width;
  l.height = page.height;
  l.fill.resize(page.alpha.size() * 4);
  l.outline.resize(page.alpha.size() * 4);
  for (size_t i = 0; i < page.alpha.size(); ++i) {
    const int a = page.alpha[i];
    // Letter: the ramp from the outline level up to full (its soft edge).
    const int fill = std::clamp((a - kOutlineAlpha) * 255 / (kFillAlpha - kOutlineAlpha), 0, 255);
    // Outline: everything inked, the ramp below the outline level (its edge).
    const int outline = std::clamp(a * 255 / kOutlineAlpha, 0, 255);
    std::memset(&l.fill[4 * i], 255, 3);
    l.fill[4 * i + 3] = uint8_t(fill);
    std::memset(&l.outline[4 * i], 255, 3);
    l.outline[4 * i + 3] = uint8_t(outline);
  }
  return l;
}

}  // namespace game_font
