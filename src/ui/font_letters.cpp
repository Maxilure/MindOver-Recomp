// =============================================================================
// ui/font_letters.cpp -- see font_letters.h
// =============================================================================
#include "font_letters.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"

namespace font_letters {
namespace {

constexpr uint32_t kChunkTextureFont = 0x22000, kChunkGlyphs = 0x22001;
constexpr uint32_t kChunkTexture = 0x19000, kChunkImage = 0x19001, kChunkImageData = 0x19002;
constexpr size_t kRecord = 40;  // glyph record: page, u0 v0 u1 v1, 4 floats, character
constexpr size_t kTga = 4;      // the image data: u32 size, then the TGA
constexpr size_t kTgaHeader = 18, kPalette = 1024;

std::atomic<uint32_t> g_added_large{0}, g_added_small{0};

// One letter to make: the character, its base letter, the tilde's letter,
// and the plain letter that tells where the tilde ends.
struct Letter {
  char16_t character, base, with_tilde, plain;
};
constexpr Letter kLetters[] = {{u'ã', u'a', u'õ', u'o'}, {u'Ã', u'A', u'Õ', u'O'}};

float F32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}
void PutF32(uint8_t* p, float f) { std::memcpy(p, &f, 4); }

// A font page's pixels (8-bit indices) and palette, inside its node.
struct Page {
  pure3d::Node* data = nullptr;  // the 0x19002 node
  int w = 0, h = 0;
  uint8_t* palette = nullptr;    // 256 x BGRA
  uint8_t* pixels = nullptr;     // w x h, top row first
  uint32_t Colour(int x, int y) const {
    const uint8_t* c = palette + 4 * pixels[y * w + x];
    return uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
  }
  uint8_t Alpha(int x, int y) const { return palette[4 * pixels[y * w + x] + 3]; }
  // The palette entry nearest to a BGRA colour.
  uint8_t Nearest(uint32_t bgra) const {
    int best = 0;
    long best_d = -1;
    for (int i = 0; i < 256; ++i) {
      long dist = 0;
      for (int k = 0; k < 4; ++k) {
        const long a = long(palette[4 * i + k]), b = long(bgra >> (8 * k) & 0xFF);
        dist += (a - b) * (a - b) * (k == 3 ? 4 : 1);  // alpha matters most
      }
      if (best_d < 0 || dist < best_d) best_d = dist, best = i;
    }
    return uint8_t(best);
  }
};

bool OpenPage(pure3d::Node& texture, Page* page) {
  for (pure3d::Node& image : texture.children) {
    if (image.id != kChunkImage) continue;
    for (pure3d::Node& data : image.children) {
      if (data.id != kChunkImageData || data.data.size() < kTga + kTgaHeader + kPalette) continue;
      uint8_t* t = &data.data[kTga];
      // Colour-mapped, uncompressed; 256 entries of 32 bits; 8 bits per pixel; top-left origin.
      if (t[1] != 1 || t[2] != 1 || t[5] != 0 || t[6] != 1 || t[7] != 32 || t[16] != 8 || !(t[17] & 0x20)) {
        return false;
      }
      const int w = t[12] | t[13] << 8, h = t[14] | t[15] << 8;
      if (data.data.size() < kTga + kTgaHeader + kPalette + size_t(w) * size_t(h)) return false;
      page->data = &data;
      page->w = w;
      page->h = h;
      page->palette = t + kTgaHeader;
      page->pixels = t + kTgaHeader + kPalette;
      return true;
    }
  }
  return false;
}

// A glyph record's cell in pixels.
struct Cell {
  int page = -1, x = 0, y = 0, w = 0, h = 0;
};
Cell CellOf(const uint8_t* r, const Page& page) {
  Cell c;
  c.page = int(pure3d::Le32(r) & 0xFFFF);
  c.x = int(F32(r + 4) * float(page.w) + 0.5f);
  c.w = int(F32(r + 12) * float(page.w) + 0.5f) - c.x;
  c.y = int((1.0f - F32(r + 16)) * float(page.h) + 0.5f);
  c.h = int((1.0f - F32(r + 8)) * float(page.h) + 0.5f) - c.y;
  return c;
}

bool PatchFont(pure3d::Node& font, bool* large_out) {
  // Name: u32, then u8 length + bytes.
  if (font.data.size() < 6) return false;
  const std::string name(reinterpret_cast<const char*>(&font.data[5]),
                         strnlen(reinterpret_cast<const char*>(&font.data[5]), font.data[4]));
  const bool large = name.find("Large") != std::string::npos;
  *large_out = large;
  std::vector<size_t> page_nodes;
  pure3d::Node* glyphs = nullptr;
  for (size_t i = 0; i < font.children.size(); ++i) {
    if (font.children[i].id == kChunkTexture) page_nodes.push_back(i);
    if (font.children[i].id == kChunkGlyphs) glyphs = &font.children[i];
  }
  if (!glyphs || page_nodes.empty() || font.data.size() < 4) return false;
  const size_t count_at = font.data.size() - 4;
  uint32_t page_count = pure3d::NodeLe32(font, count_at);
  if (page_count != page_nodes.size()) return false;
  const uint32_t n = pure3d::NodeLe32(*glyphs, 0);
  if (glyphs->data.size() < 4 + kRecord * n) return false;
  auto record = [&](char16_t ch) -> uint8_t* {
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = &glyphs->data[4 + kRecord * i];
      if (pure3d::Le32(r + 36) == ch) return r;
    }
    return nullptr;
  };
  std::vector<Page> pages(page_nodes.size());
  for (size_t i = 0; i < page_nodes.size(); ++i) {
    if (!OpenPage(font.children[page_nodes[i]], &pages[i])) pages[i] = Page{};
  }
  // Text pages and the space their cells take (a cell may be transparent but in use).
  std::vector<std::vector<uint8_t>> used(pages.size());
  std::vector<bool> text(pages.size(), false);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &glyphs->data[4 + kRecord * i];
    const uint32_t p = pure3d::Le32(r);
    if (p & 0x80000000u || (p & 0xFFFF) >= pages.size() || !pages[p & 0xFFFF].data) continue;
    text[p & 0xFFFF] = true;
  }
  for (size_t p = 0; p < pages.size(); ++p) {
    if (!text[p]) continue;
    const Page& pg = pages[p];
    used[p].assign(size_t(pg.w) * size_t(pg.h), 0);
    for (int y = 0; y < pg.h; ++y)
      for (int x = 0; x < pg.w; ++x) used[p][size_t(y * pg.w + x)] = pg.Alpha(x, y) != 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* r = &glyphs->data[4 + kRecord * i];
      if ((pure3d::Le32(r) & 0x8000FFFFu) != p) continue;
      const Cell c = CellOf(r, pg);
      for (int y = std::max(0, c.y - 1); y < std::min(pg.h, c.y + c.h + 1); ++y)
        for (int x = std::max(0, c.x - 1); x < std::min(pg.w, c.x + c.w + 1); ++x) used[p][size_t(y * pg.w + x)] = 1;
    }
  }
  // Compose each letter (BGRA cell).
  struct Made {
    const Letter* letter;
    uint8_t* target;
    std::vector<uint8_t> base_record;
    int w, h, widen;
    std::vector<uint32_t> bgra;
  };
  std::vector<Made> made;
  for (const Letter& l : kLetters) {
    uint8_t *target = record(l.character), *base = record(l.base), *tilde = record(l.with_tilde),
            *plain = record(l.plain);
    if (!target || !base || !tilde || !plain) continue;
    if (F32(target + 12) != F32(target + 4)) continue;  // it has a picture already
    auto page_of = [&](const uint8_t* r) -> const Page* {
      const uint32_t p = pure3d::Le32(r);
      return (p & 0x80000000u) || (p & 0xFFFF) >= pages.size() || !pages[p & 0xFFFF].data ? nullptr
                                                                                          : &pages[p & 0xFFFF];
    };
    const Page *bp = page_of(base), *tp = page_of(tilde), *pp = page_of(plain);
    if (!bp || !tp || !pp) continue;
    const Cell bc = CellOf(base, *bp), tc = CellOf(tilde, *tp), pc = CellOf(plain, *pp);
    // The tilde: rows of the õ cell above the plain o's first inked row.
    int top = pc.h;
    for (int y = 0; y < pc.h && top == pc.h; ++y)
      for (int x = 0; x < pc.w; ++x)
        if (pp->Alpha(pc.x + x, pc.y + y)) { top = y; break; }
    Made m{&l, target, std::vector<uint8_t>(base, base + kRecord), std::max(bc.w, tc.w), bc.h, 0, {}};
    m.widen = m.w - bc.w;
    m.bgra.assign(size_t(m.w) * size_t(m.h), 0);
    const int bx = (m.w - bc.w) / 2, tx = (m.w - tc.w) / 2;
    for (int y = 0; y < bc.h; ++y)
      for (int x = 0; x < bc.w; ++x) m.bgra[size_t(y * m.w + bx + x)] = bp->Colour(bc.x + x, bc.y + y);
    for (int y = 0; y < std::min({top - 1, tc.h, m.h}); ++y)
      for (int x = 0; x < tc.w; ++x) {
        const uint32_t c = tp->Colour(tc.x + x, tc.y + y);
        uint32_t& dst = m.bgra[size_t(y * m.w + tx + x)];
        if ((c >> 24) && (c >> 24) >= (dst >> 24)) dst = c;
      }
    made.push_back(std::move(m));
  }
  if (made.empty()) return false;
  // Places: free space on a text page (summed areas), else one new page.
  auto place_free = [&](int w, int h, int* page_out, int* x_out, int* y_out) {
    for (size_t p = 0; p < pages.size(); ++p) {
      if (!text[p]) continue;
      const int pw = pages[p].w, ph = pages[p].h;
      std::vector<int> s(size_t(pw + 1) * size_t(ph + 1), 0);
      auto S = [&](int x, int y) -> int& { return s[size_t(y) * size_t(pw + 1) + size_t(x)]; };
      for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) S(x + 1, y + 1) = used[p][size_t(y * pw + x)] + S(x, y + 1) + S(x + 1, y) - S(x, y);
      for (int y = 0; y + h + 2 <= ph; ++y)
        for (int x = 0; x + w + 2 <= pw; ++x)
          if (S(x + w + 2, y + h + 2) - S(x, y + h + 2) - S(x + w + 2, y) + S(x, y) == 0) {
            *page_out = int(p);
            *x_out = x + 1;
            *y_out = y + 1;
            for (int yy = y; yy < y + h + 2; ++yy)
              for (int xx = x; xx < x + w + 2; ++xx) used[p][size_t(yy * pw + xx)] = 1;
            return true;
          }
    }
    return false;
  };
  int new_page = -1, new_x = 1;
  for (Made& m : made) {
    int p = -1, x = 0, y = 0;
    if (!place_free(m.w, m.h, &p, &x, &y)) {
      if (new_page < 0) {
        // One new page: a copy of the first text page, cleared, after the last page.
        size_t first = 0;
        while (first < pages.size() && !text[first]) ++first;
        if (first == pages.size() || page_count >= 64) return false;
        pure3d::Node copy = font.children[page_nodes[first]];
        const std::string old_name(pure3d::NodeName(copy));
        const std::string name_new = old_name.substr(0, old_name.rfind('_') + 1) + std::to_string(page_count);
        pure3d::SetNodeName(copy, name_new);
        for (pure3d::Node& child : copy.children)
          if (pure3d::NodeName(child) == old_name) pure3d::SetNodeName(child, name_new);
        font.children.insert(font.children.begin() + std::ptrdiff_t(page_nodes.back() + 1), std::move(copy));
        // (inserting moved the vector's nodes: re-open every page)
        page_nodes.push_back(page_nodes.back() + 1);
        text.push_back(false);  // (not searched for free space: placed by hand below)
        used.emplace_back();
        pages.assign(page_nodes.size(), Page{});
        for (size_t i = 0; i < page_nodes.size(); ++i) OpenPage(font.children[page_nodes[i]], &pages[i]);
        glyphs = nullptr;
        for (pure3d::Node& c : font.children)
          if (c.id == kChunkGlyphs) glyphs = &c;
        Page& np = pages.back();
        uint8_t clear = 0;
        for (int i = 0; i < 256; ++i)
          if (np.palette[4 * i + 3] == 0) { clear = uint8_t(i); break; }
        std::fill(np.pixels, np.pixels + size_t(np.w) * size_t(np.h), clear);
        new_page = int(page_count);
        pure3d::SetNodeLe32(font, count_at, ++page_count);
        // the records' pointers moved with the glyph node: find them again
        for (Made& k : made) {
          for (uint32_t i = 0; i < n; ++i) {
            uint8_t* r = &glyphs->data[4 + kRecord * i];
            if (pure3d::Le32(r + 36) == k.letter->character) k.target = r;
          }
        }
      }
      p = new_page;
      x = new_x;
      y = 1;
      new_x += m.w + 2;
    }
    Page& pg = pages[size_t(p)];
    for (int yy = 0; yy < m.h; ++yy)
      for (int xx = 0; xx < m.w; ++xx) pg.pixels[size_t((y + yy) * pg.w + x + xx)] = pg.Nearest(m.bgra[size_t(yy * m.w + xx)]);
    // The record: the base letter's, at the new place.
    std::vector<uint8_t> r = m.base_record;
    for (int b = 0; b < 4; ++b) r[size_t(b)] = uint8_t(uint32_t(p) >> (8 * b));
    PutF32(&r[4], float(x) / float(pg.w));
    PutF32(&r[12], float(x + m.w) / float(pg.w));
    PutF32(&r[16], 1.0f - float(y) / float(pg.h));
    PutF32(&r[8], 1.0f - float(y + m.h) / float(pg.h));
    PutF32(&r[20], F32(&r[20]) - float(m.widen) / 2.0f);  // left bearing: the widening's half
    PutF32(&r[28], float(m.w));                           // width
    for (int b = 0; b < 4; ++b) r[size_t(36 + b)] = uint8_t(uint32_t(m.letter->character) >> (8 * b));
    std::memcpy(m.target, r.data(), kRecord);
    REXLOG_INFO("Font letters: {} U+{:04X} drawn on page {} at {},{} ({}x{})", name, uint32_t(m.letter->character), p,
                x, y, m.w, m.h);
  }
  (large ? g_added_large : g_added_small) = new_page >= 0 ? 1 : 0;
  return true;
}

bool Patch(const data_patcher::Bytes& original, data_patcher::Bytes* patched) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  pure3d::Bytes children;
  int changed = 0;
  for (const pure3d::Chunk& c : top) {
    if (c.id == kChunkTextureFont) {
      pure3d::Node font = pure3d::ToNode(original, c);
      bool large = false;
      if (PatchFont(font, &large)) {
        ++changed;
        const pure3d::Bytes bytes = pure3d::ToBytes(font);
        children.insert(children.end(), bytes.begin(), bytes.end());
        continue;
      }
    }
    const pure3d::Bytes bytes = pure3d::Copy(original, c);
    children.insert(children.end(), bytes.begin(), bytes.end());
  }
  if (!changed) return false;
  *patched = pure3d::WithChildren(original, root, children);
  return true;
}

}  // namespace

void Register() {
  data_patcher::Register("font letters Ã ã", data_patcher::kCategoryFrontend, "b4c85fe7", &Patch);
}

uint32_t AddedPages(bool large) { return (large ? g_added_large : g_added_small).load(); }

}  // namespace font_letters
