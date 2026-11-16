// =============================================================================
// ui/font_letters.cpp -- see font_letters.h
// =============================================================================
//
// Layout of this file:
//   * Img: a small BGRA picture and the handful of operations the recipes
//     need (crop, paste "the more opaque pixel wins", stretch by repeating
//     the middle row / column, turn, scale);
//   * the RECIPES: one function per letter, each built from the font's own
//     cells (the font's look, outline included, comes along for free);
//   * PatchFont: the glyph table's records (missing ones inserted in
//     character order), free space on the text pages (one page grown to
//     twice its height when there's none left), the pixels mapped to the
//     page's palette, the records filled in.
// =============================================================================
#include "font_letters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"

namespace font_letters {
namespace {

constexpr uint32_t kChunkTextureFont = 0x22000, kChunkGlyphs = 0x22001;
constexpr uint32_t kChunkTexture = 0x19000, kChunkImage = 0x19001, kChunkImageData = 0x19002;
constexpr size_t kRecord = 40;  // glyph record: page, u0 v0 u1 v1, left, right, width, advance, character
constexpr size_t kTga = 4;      // the image data: u32 size, then the TGA
constexpr size_t kTgaHeader = 18, kPalette = 1024;
// The game's two alpha levels in a text cell: the letter (fill) and its
// baked outline (findings/30: 254 / 104). "Fill" below = alpha >= 200.
constexpr uint8_t kFillAlpha = 200;

float F32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}
void PutF32(uint8_t* p, float f) { std::memcpy(p, &f, 4); }

// -----------------------------------------------------------------------------
// Img: BGRA pixels (B in the low byte, A in the high byte), top row first.
struct Box {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x1 / y1 exclusive
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

struct Img {
  int w = 0, h = 0;
  std::vector<uint32_t> px;
  Img() = default;
  Img(int width, int height) : w(std::max(width, 0)), h(std::max(height, 0)), px(size_t(w) * size_t(h), 0) {}
  uint32_t at(int x, int y) const { return px[size_t(y) * size_t(w) + size_t(x)]; }
  uint32_t& at(int x, int y) { return px[size_t(y) * size_t(w) + size_t(x)]; }
  uint8_t alpha(int x, int y) const { return uint8_t(at(x, y) >> 24); }
};

// The box around every pixel at least `threshold` opaque (empty box if none).
Box InkBox(const Img& img, uint8_t threshold = 1) {
  Box b{img.w, img.h, 0, 0};
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x)
      if (img.alpha(x, y) >= threshold) {
        b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y);
        b.x1 = std::max(b.x1, x + 1), b.y1 = std::max(b.y1, y + 1);
      }
  return b.empty() ? Box{} : b;
}

Img Crop(const Img& img, Box b) {
  b.x0 = std::clamp(b.x0, 0, img.w), b.x1 = std::clamp(b.x1, b.x0, img.w);
  b.y0 = std::clamp(b.y0, 0, img.h), b.y1 = std::clamp(b.y1, b.y0, img.h);
  Img out(b.w(), b.h());
  for (int y = 0; y < out.h; ++y)
    for (int x = 0; x < out.w; ++x) out.at(x, y) = img.at(b.x0 + x, b.y0 + y);
  return out;
}
Img Ink(const Img& img) { return Crop(img, InkBox(img)); }

// Pastes `src` at x, y: where both have a pixel, the more opaque one stays
// (the fill of one piece covers the outline of another: the pieces join).
void Put(Img& dst, const Img& src, int x, int y) {
  for (int yy = 0; yy < src.h; ++yy)
    for (int xx = 0; xx < src.w; ++xx) {
      const int X = x + xx, Y = y + yy;
      if (X < 0 || Y < 0 || X >= dst.w || Y >= dst.h) continue;
      const uint32_t c = src.at(xx, yy);
      if ((c >> 24) && (c >> 24) >= (dst.at(X, Y) >> 24)) dst.at(X, Y) = c;
    }
}

Img Transpose(const Img& img) {
  Img out(img.h, img.w);
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) out.at(y, x) = img.at(x, y);
  return out;
}
Img FlipH(const Img& img) {
  Img out(img.w, img.h);
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) out.at(img.w - 1 - x, y) = img.at(x, y);
  return out;
}
Img FlipV(const Img& img) {
  Img out(img.w, img.h);
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) out.at(x, img.h - 1 - y) = img.at(x, y);
  return out;
}
// A quarter turn, exact (no resampling): clockwise moves the bottom to the left.
Img TurnClockwise(const Img& img) { return FlipH(Transpose(img)); }
Img TurnAnticlockwise(const Img& img) { return FlipV(Transpose(img)); }

// Made `w` wide by repeating the middle column (wider) or leaving middle
// columns out (narrower): the two ends, with their rounded outline, stay.
Img StretchH(const Img& img, int w) {
  if (img.w == 0 || w <= 0) return Img(std::max(w, 0), img.h);
  Img out(w, img.h);
  const int left = std::min(img.w / 2, w / 2), right = std::min(img.w - img.w / 2, w - left);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < left; ++x) out.at(x, y) = img.at(x, y);
    for (int x = left; x < w - right; ++x) out.at(x, y) = img.at(img.w / 2, y);
    for (int x = 0; x < right; ++x) out.at(w - right + x, y) = img.at(img.w - right + x, y);
  }
  return out;
}
Img StretchV(const Img& img, int h) { return Transpose(StretchH(Transpose(img), h)); }

// Premultiplied bilinear sample (outside = transparent).
void Sample(const Img& img, float fx, float fy, float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
  const float tx = fx - float(x0), ty = fy - float(y0);
  for (int k = 0; k < 4; ++k) {
    const int x = x0 + (k & 1), y = y0 + (k >> 1);
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
    const float weight = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
    const uint32_t c = img.at(x, y);
    const float a = float(c >> 24) / 255.0f;
    for (int i = 0; i < 3; ++i) out[i] += weight * a * float(c >> (8 * i) & 0xFF);
    out[3] += weight * float(c >> 24);
  }
}
uint32_t Unpremultiply(const float c[4]) {
  if (c[3] < 0.5f) return 0;
  const float a = c[3] / 255.0f;
  uint32_t out = uint32_t(std::lround(std::min(c[3], 255.0f))) << 24;
  for (int i = 0; i < 3; ++i) out |= uint32_t(std::lround(std::clamp(c[i] / a, 0.0f, 255.0f))) << (8 * i);
  return out;
}

// Turned by `degrees` (anticlockwise on screen), on a canvas big enough, then
// cropped to its ink. Bilinear: the outline's soft edge survives.
Img Rotate(const Img& img, float degrees) {
  const float r = degrees * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
  const int w = int(std::ceil(std::abs(float(img.w) * c) + std::abs(float(img.h) * s))) + 2;
  const int h = int(std::ceil(std::abs(float(img.w) * s) + std::abs(float(img.h) * c))) + 2;
  Img out(w, h);
  const float cx = float(w) / 2, cy = float(h) / 2, sx = float(img.w) / 2, sy = float(img.h) / 2;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      // Screen y runs down: an anticlockwise turn maps back with +s on x.
      const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
      const float fx = c * dx - s * dy + sx - 0.5f, fy = s * dx + c * dy + sy - 0.5f;
      float p[4];
      Sample(img, fx, fy, p);
      out.at(x, y) = Unpremultiply(p);
    }
  return Ink(out);
}

// Made `factor` times as big (box filter over the covered source area).
Img Scale(const Img& img, float factor) {
  Img out(std::max(1, int(float(img.w) * factor)), std::max(1, int(float(img.h) * factor)));
  const float step_x = float(img.w) / float(out.w), step_y = float(img.h) / float(out.h);
  for (int y = 0; y < out.h; ++y)
    for (int x = 0; x < out.w; ++x) {
      float sum[4] = {0, 0, 0, 0};
      int n = 0;
      for (float fy = float(y) * step_y; fy < float(y + 1) * step_y; fy += 0.5f)
        for (float fx = float(x) * step_x; fx < float(x + 1) * step_x; fx += 0.5f) {
          float p[4];
          Sample(img, fx, fy, p);
          for (int i = 0; i < 4; ++i) sum[i] += p[i];
          ++n;
        }
      for (float& v : sum) v /= float(std::max(n, 1));
      out.at(x, y) = Unpremultiply(sum);
    }
  return out;
}

// -----------------------------------------------------------------------------
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
  *page = Page{};
  for (pure3d::Node& image : texture.children) {
    if (image.id != kChunkImage) continue;
    for (pure3d::Node& data : image.children) {
      if (data.id != kChunkImageData || data.data.size() < kTga + kTgaHeader + kPalette) continue;
      uint8_t* t = &data.data[kTga];
      // Colour-mapped, uncompressed; 256 entries of 32 bits; 8 bits per pixel; top-left origin.
      if (t[0] != 0 || t[1] != 1 || t[2] != 1 || t[5] != 0 || t[6] != 1 || t[7] != 32 || t[16] != 8 ||
          !(t[17] & 0x20)) {
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

// Makes a page `extra` rows taller, the new rows transparent: the texture
// (0x19000) and image (0x19001) chunks hold width, height after the name and
// a version word, the TGA its own at 12 / 14 (the same three places
// prompt_icons' EnlargePage changes). The glyphs already on the page keep
// their pixels; their v coordinates are fixed by the caller.
bool GrowPage(pure3d::Node& texture, int extra) {
  auto set_height = [extra](pure3d::Node& n) {
    const size_t at = pure3d::NameEnd(n) + 4 + 4;  // name, version, width -> height
    if (n.data.size() < at + 4) return false;
    pure3d::SetNodeLe32(n, at, pure3d::NodeLe32(n, at) + uint32_t(extra));
    return true;
  };
  Page page;
  if (!OpenPage(texture, &page)) return false;
  uint8_t clear = 0;
  for (int i = 0; i < 256; ++i)
    if (page.palette[4 * i + 3] == 0) { clear = uint8_t(i); break; }
  const int new_h = page.h + extra;
  if (new_h > 0xFFFF || !set_height(texture)) return false;
  for (pure3d::Node& image : texture.children)
    if (image.id == kChunkImage && !set_height(image)) return false;
  pure3d::Node& data = *page.data;
  const size_t end = kTga + kTgaHeader + kPalette + size_t(page.w) * size_t(page.h);
  data.data.resize(end);  // (anything after the pixels: none in these fonts)
  data.data.insert(data.data.end(), size_t(page.w) * size_t(extra), clear);
  uint8_t* t = &data.data[kTga];
  t[14] = uint8_t(new_h), t[15] = uint8_t(new_h >> 8);
  pure3d::SetNodeLe32(data, 0, uint32_t(data.data.size() - kTga));
  return true;
}

// A glyph record's cell in pixels.
Box CellOf(const uint8_t* r, const Page& page) {
  Box c;
  c.x0 = int(F32(r + 4) * float(page.w) + 0.5f);
  c.x1 = int(F32(r + 12) * float(page.w) + 0.5f);
  c.y0 = int((1.0f - F32(r + 16)) * float(page.h) + 0.5f);
  c.y1 = int((1.0f - F32(r + 8)) * float(page.h) + 0.5f);
  return c;
}

// -----------------------------------------------------------------------------
// THE RECIPES. Every one gets the font (cells by character, a few measures)
// and returns a picture as tall as a text cell (the cell's rows ARE the
// letter's vertical place: all text cells share one height, drawn from the
// line's top), plus its bearings. Shapes were tried out on both fonts in a
// prototype before being written here (findings/49 s.8.4: pictures).
struct Font {
  std::function<std::optional<Img>(char16_t)> cell;  // a character's cell, nullopt = none
  int height = 0;    // the cell height (Small 59, Large 89 px)
  int baseline = 0;  // the row under the fill of 'H'
  int cap_top = 0;   // the first fill row of 'H'
  int dash_mid = 0;  // the middle row of '-' (its ink with the outline)
  int bearing = 1;   // the gap left and right of a made letter
};

struct Made {
  Img img;
  // The record: the base letter's (Ã = A's, Ё = Е's) with its left bearing
  // moved by half the widening, else ours (font.bearing on both sides).
  std::optional<std::vector<uint8_t>> base_record;
  int widen = 0;
};

// Rows of `with`'s cell above where `plain`'s ink starts (less one row): the
// accent of õ / â / à / ë. `y` = where its ink begins in the cell.
std::optional<Img> Accent(const Font& f, char16_t with, char16_t plain, int* y) {
  const auto a = f.cell(with), p = f.cell(plain);
  if (!a || !p) return std::nullopt;
  const Box pb = InkBox(*p);
  if (pb.empty()) return std::nullopt;
  const Img top = Crop(*a, Box{0, 0, a->w, pb.y0 - 1});
  const Box b = InkBox(top);
  if (b.empty()) return std::nullopt;
  *y = b.y0;
  return Crop(top, b);
}

Img Canvas(const Font& f, int w) { return Img(w, f.height); }

// Ã ã Ё ё: the base letter's whole cell + an accent above it, both centred
// in the wider of the two (the way Ã / ã were made first, 2026-10-31).
std::optional<Made> AccentOver(const Font& f, char16_t base, char16_t with, char16_t plain,
                               const std::vector<uint8_t>& base_record) {
  const auto b = f.cell(base), a = f.cell(with), p = f.cell(plain);
  if (!b || !a || !p) return std::nullopt;
  const Box pb = InkBox(*p);
  if (pb.empty()) return std::nullopt;
  const int w = std::max(b->w, a->w);
  Made m{Canvas(f, w), base_record, w - b->w};
  Put(m.img, *b, (w - b->w) / 2, 0);
  Put(m.img, Crop(*a, Box{0, 0, a->w, pb.y0 - 1}), (w - a->w) / 2, 0);
  return m;
}

// A piece pasted into a cell-tall canvas of its own width at row y.
Made Alone(const Font& f, const Img& piece, int y) {
  Made m{Canvas(f, piece.w), std::nullopt, 0};
  Put(m.img, piece, 0, y);
  return m;
}

std::optional<Made> Recipe(const Font& f, char16_t c, const std::vector<uint8_t>& record) {
  auto ink_of = [&](char16_t k) -> std::optional<Img> {
    const auto cell = f.cell(k);
    if (!cell) return std::nullopt;
    const Img i = Ink(*cell);
    return i.w ? std::optional<Img>(i) : std::nullopt;
  };
  switch (c) {
    case u'ã': return AccentOver(f, u'a', u'õ', u'o', record);  // ã = a + õ's tilde
    case u'Ã': return AccentOver(f, u'A', u'Õ', u'O', record);  // Ã = A + Õ's tilde
    // (Ё ё: PatchFont calls AccentOver itself, with Ë / ë's dots.)
    case u'"': {  // two apostrophes, 2 px apart (outline to outline)
      const auto q = f.cell(u'\'');
      if (!q) return std::nullopt;
      const Box b = InkBox(*q);
      const Img i = Crop(*q, b);
      Made m{Canvas(f, 2 * i.w + 2), std::nullopt, 0};
      Put(m.img, i, 0, b.y0);
      Put(m.img, i, i.w + 2, b.y0);
      return m;
    }
    case u'~': {  // õ's tilde, centred on the dash's height
      int y = 0;
      const auto t = Accent(f, u'õ', u'o', &y);
      if (!t) return std::nullopt;
      return Alone(f, *t, f.dash_mid - t->h / 2);
    }
    case u'^':   // â's hat, where â has it
    case u'`': { // à's accent, where à has it
      int y = 0;
      const auto t = Accent(f, c == u'^' ? u'â' : u'à', u'a', &y);
      if (!t) return std::nullopt;
      return Alone(f, *t, y);
    }
    case u'_': {  // the dash, as wide as an n (+4), its top 2 rows above the baseline
      const auto d = ink_of(u'-'), n = ink_of(u'n');
      if (!d || !n) return std::nullopt;
      return Alone(f, StretchH(*d, std::max(d->w, n->w + 4)), f.baseline - 2);
    }
    case u'|': {  // the I's bar, from the top of '(' to its bottom
      const auto i = ink_of(u'I'), p = f.cell(u'(');
      if (!i || !p) return std::nullopt;
      const Box pb = InkBox(*p);
      return Alone(f, StretchV(*i, pb.h()), pb.y0);
    }
    case u'†': {  // † = a bar from the capitals' top to the bottom of '(' + a dash across
      const auto i = ink_of(u'I'), d = ink_of(u'-'), p = f.cell(u'('), h = f.cell(u'H');
      if (!i || !d || !p || !h) return std::nullopt;
      const int top = InkBox(*h).y0;
      const Img bar = StretchV(*i, InkBox(*p).y1 - top), cross = StretchH(*d, i->w * 3);
      Made m{Canvas(f, cross.w), std::nullopt, 0};
      Put(m.img, bar, (cross.w - bar.w) / 2, top);
      Put(m.img, cross, 0, top + bar.h / 4 - cross.h / 2);
      return m;
    }
    case u'*': {  // three dashes 0.72 capitals long, turned 90 / 30 / 150 degrees
      const auto d = ink_of(u'-'), h = f.cell(u'H');
      if (!d || !h) return std::nullopt;
      const Img arm = StretchH(*d, int(float(f.baseline - f.cap_top) * 0.72f));
      const Img pieces[3] = {Rotate(arm, 90), Rotate(arm, 30), Rotate(arm, 150)};
      int w = 0, ht = 0;
      for (const Img& p : pieces) w = std::max(w, p.w), ht = std::max(ht, p.h);
      Img star(w, ht);
      for (const Img& p : pieces) Put(star, p, (w - p.w) / 2, (ht - p.h) / 2);
      return Alone(f, star, InkBox(*h).y0 - 1);
    }
    case u'<':
    case u'>': {  // the v on its side, centred on the dash's height
      const auto v = ink_of(u'v');
      if (!v) return std::nullopt;
      const Img r = Ink(c == u'<' ? TurnClockwise(*v) : TurnAnticlockwise(*v));
      return Alone(f, r, f.dash_mid - r.h / 2);
    }
    case kLeftQuote:
    case kRightQuote: {  // « » = two v's on their side at 0.8x, overlapping
      const auto v = ink_of(u'v');
      if (!v) return std::nullopt;
      const Img r = Scale(Ink(c == kLeftQuote ? TurnClockwise(*v) : TurnAnticlockwise(*v)), 0.8f);
      const int step = int(float(r.w) * 0.55f), y = f.dash_mid - r.h / 2 + 1;
      Made m{Canvas(f, r.w + step), std::nullopt, 0};
      Put(m.img, r, 0, y);
      Put(m.img, r, step, y);
      return m;
    }
    case u'{':
    case u'}': {  // ( / ) + a short nub of the dash at its middle, fills joined
      const bool left = c == u'{';
      const auto p = f.cell(left ? u'(' : u')'), d = ink_of(u'-');
      if (!p || !d) return std::nullopt;
      const Box pb = InkBox(*p);
      if (pb.empty()) return std::nullopt;
      const Img stem = Crop(*p, Box{pb.x0, 0, pb.x1, p->h});
      const int mid = (pb.y0 + pb.y1) / 2;
      Img nub = Crop(*d, Box{0, 0, std::min(d->w, int(float(d->h) * 1.1f)), d->h});
      if (!left) nub = FlipH(nub);
      // The stem's fill on the middle row, the nub's fill: 2 px overlap.
      int fill0 = -1, fill1 = -1;
      for (int x = 0; x < stem.w; ++x)
        if (stem.alpha(x, mid) >= kFillAlpha) fill1 = x, fill0 = fill0 < 0 ? x : fill0;
      const Box nb = InkBox(nub, kFillAlpha);
      if (fill0 < 0 || nb.empty()) return std::nullopt;
      int stem_x = 0, nub_x = 0;
      if (left) {
        stem_x = std::max(0, (nb.x1 - 2) - fill0);
        nub_x = stem_x + fill0 + 2 - nb.x1;
      } else {
        nub_x = fill1 - 2 - nb.x0;
      }
      Made m{Canvas(f, std::max(stem_x + stem.w, nub_x + nub.w)), std::nullopt, 0};
      Put(m.img, stem, stem_x, 0);
      Put(m.img, nub, nub_x, mid - nub.h / 2);
      return m;
    }
    default:
      return std::nullopt;
  }
}

// The letters this patch makes. `base` = the letter whose record (bearings,
// advance) a made letter takes (0 = its own: font.bearing each side).
struct Letter {
  char16_t character;
  char16_t base;
};
constexpr Letter kLetters[] = {
    {u'ã', u'a'}, {u'Ã', u'A'},            // ã Ã (Brazilian Portuguese)
    {u'Ё', u'Е'}, {u'ё', u'е'},  // Ё ё (Russian)
    {u'"', 0}, {u'~', 0}, {u'^', 0}, {u'`', 0}, {u'_', 0}, {u'|', 0}, {u'†', 0},
    {u'*', 0}, {u'<', 0}, {u'>', 0}, {u'{', 0}, {u'}', 0},
    {kLeftQuote, 0}, {kRightQuote, 0},               // « » (private codes: font_letters.h)
};

// -----------------------------------------------------------------------------
bool PatchFont(pure3d::Node& font) {
  // Name: u32, then u8 length + bytes.
  if (font.data.size() < 6) return false;
  const std::string name(reinterpret_cast<const char*>(&font.data[5]),
                         strnlen(reinterpret_cast<const char*>(&font.data[5]), font.data[4]));
  std::vector<size_t> page_nodes;
  pure3d::Node* glyphs = nullptr;
  for (size_t i = 0; i < font.children.size(); ++i) {
    if (font.children[i].id == kChunkTexture) page_nodes.push_back(i);
    if (font.children[i].id == kChunkGlyphs) glyphs = &font.children[i];
  }
  if (!glyphs || page_nodes.empty() || glyphs->data.size() < 4) return false;
  auto count = [&] { return pure3d::NodeLe32(*glyphs, 0); };
  auto record = [&](char16_t ch) -> uint8_t* {
    for (uint32_t i = 0; i < count() && 4 + kRecord * (i + 1) <= glyphs->data.size(); ++i) {
      uint8_t* r = &glyphs->data[4 + kRecord * i];
      if (pure3d::Le32(r + 36) == ch) return r;
    }
    return nullptr;
  };
  // Characters the font doesn't list at all (Ё ё « »): an empty record each,
  // IN CHARACTER ORDER (the game binary-searches the table, prompt_icons.cpp).
  for (const Letter& l : kLetters) {
    if (record(l.character)) continue;
    std::vector<uint8_t> r(kRecord, 0);
    for (int b = 0; b < 4; ++b) r[size_t(36 + b)] = uint8_t(uint32_t(l.character) >> (8 * b));
    const uint32_t n = count();
    size_t at = 4 + kRecord * n;
    for (uint32_t i = 0; i < n; ++i)
      if (pure3d::NodeLe32(*glyphs, 4 + kRecord * i + 36) > l.character) { at = 4 + kRecord * i; break; }
    glyphs->data.insert(glyphs->data.begin() + std::ptrdiff_t(at), r.begin(), r.end());
    pure3d::SetNodeLe32(*glyphs, 0, n + 1);
  }
  const uint32_t n = count();
  if (glyphs->data.size() < 4 + kRecord * n) return false;

  std::vector<Page> pages(page_nodes.size());
  auto open_pages = [&] {
    for (size_t i = 0; i < page_nodes.size(); ++i) OpenPage(font.children[page_nodes[i]], &pages[i]);
  };
  open_pages();
  auto page_of = [&](const uint8_t* r) -> const Page* {
    const uint32_t p = pure3d::Le32(r);
    return (p & 0x80000000u) || (p & 0xFFFF) >= pages.size() || !pages[p & 0xFFFF].data ? nullptr
                                                                                        : &pages[p & 0xFFFF];
  };
  // The font, as the recipes see it: cells read from the pages as they are now.
  Font f;
  f.cell = [&](char16_t ch) -> std::optional<Img> {
    const uint8_t* r = record(ch);
    const Page* p = r ? page_of(r) : nullptr;
    if (!p || F32(r + 12) == F32(r + 4)) return std::nullopt;  // (no picture)
    const Box c = CellOf(r, *p);
    if (c.x0 < 0 || c.y0 < 0 || c.x1 > p->w || c.y1 > p->h || c.empty()) return std::nullopt;
    Img img(c.w(), c.h());
    for (int y = 0; y < img.h; ++y)
      for (int x = 0; x < img.w; ++x) img.at(x, y) = p->Colour(c.x0 + x, c.y0 + y);
    return img;
  };
  const auto h = f.cell(u'H'), dash = f.cell(u'-');
  if (!h || !dash) return false;
  f.height = h->h;
  f.baseline = InkBox(*h, kFillAlpha).y1;
  f.cap_top = InkBox(*h, kFillAlpha).y0;
  const Box db = InkBox(*dash);
  f.dash_mid = (db.y0 + db.y1) / 2;
  f.bearing = std::max(1, f.height / 40);

  // Every letter first (cells are read from the pages as the disc has them),
  // then placed.
  struct Job {
    const Letter* letter;
    Made made;
  };
  std::vector<Job> jobs;
  for (const Letter& l : kLetters) {
    const uint8_t* target = record(l.character);
    if (!target || F32(target + 12) != F32(target + 4)) continue;  // it has a picture already
    std::optional<Made> m;
    if (l.character == u'Ё' || l.character == u'ё') {  // Ё ё = Е е + Ë ë's dots
      const uint8_t* base = record(l.base);
      if (base) {
        const bool capital = l.character == u'Ё';
        m = AccentOver(f, l.base, capital ? u'Ë' : u'ë', capital ? u'E' : u'e',
                       std::vector<uint8_t>(base, base + kRecord));
      }
    } else {
      const uint8_t* base = l.base ? record(l.base) : nullptr;
      m = Recipe(f, l.character, base ? std::vector<uint8_t>(base, base + kRecord) : std::vector<uint8_t>());
    }
    if (!m || m->img.w == 0 || m->img.h != f.height) {
      REXLOG_WARN("Font letters: {} U+{:04X} couldn't be made (a piece is missing)", name,
                  uint32_t(l.character));
      continue;
    }
    jobs.push_back({&l, std::move(*m)});
  }
  if (jobs.empty()) return false;
  // Biggest first: the small ones fill the gaps the big ones leave.
  std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.made.img.w > b.made.img.w; });

  // Space in use on the text pages: every inked pixel, every text cell + 1
  // pixel around it (a filtered sample reaches one pixel out).
  std::vector<bool> text(pages.size(), false);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &glyphs->data[4 + kRecord * i];
    if (page_of(r)) text[pure3d::Le32(r) & 0xFFFF] = true;
  }
  std::vector<std::vector<uint8_t>> used(pages.size());
  auto mark_used = [&](size_t p) {
    const Page& pg = pages[p];
    used[p].assign(size_t(pg.w) * size_t(pg.h), 0);
    for (int y = 0; y < pg.h; ++y)
      for (int x = 0; x < pg.w; ++x) used[p][size_t(y * pg.w + x)] = pg.Alpha(x, y) != 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* r = &glyphs->data[4 + kRecord * i];
      if ((pure3d::Le32(r) & 0x8000FFFFu) != p || F32(r + 12) == F32(r + 4)) continue;
      const Box c = CellOf(r, pg);
      for (int y = std::max(0, c.y0 - 1); y < std::min(pg.h, c.y1 + 1); ++y)
        for (int x = std::max(0, c.x0 - 1); x < std::min(pg.w, c.x1 + 1); ++x) used[p][size_t(y * pg.w + x)] = 1;
    }
  };
  for (size_t p = 0; p < pages.size(); ++p)
    if (text[p]) mark_used(p);
  // A w x h spot + 1 free pixel around it (summed-area table per page).
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
            *page_out = int(p), *x_out = x + 1, *y_out = y + 1;
            for (int yy = y; yy < y + h + 2; ++yy)
              for (int xx = x; xx < x + w + 2; ++xx) used[p][size_t(yy * pw + xx)] = 1;
            return true;
          }
    }
    return false;
  };
  // NO ROOM: the last text page is made twice as tall (a NEW page isn't an
  // option: Titans_Small already has the game's 64 with prompt_icons' pages,
  // findings/37 s.6). Its glyphs' v coordinates (rows from the bottom, as a
  // share of the height) are rewritten for the new height.
  bool grown = false;
  auto grow = [&]() {
    size_t last = pages.size();
    for (size_t p = 0; p < pages.size(); ++p)
      if (text[p]) last = p;
    if (last == pages.size()) return false;
    const int old_h = pages[last].h;
    if (!GrowPage(font.children[page_nodes[last]], old_h)) return false;
    open_pages();
    const float k = float(old_h) / float(pages[last].h);
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = &glyphs->data[4 + kRecord * i];
      if ((pure3d::Le32(r) & 0x8000FFFFu) != last) continue;
      PutF32(r + 8, 1.0f - (1.0f - F32(r + 8)) * k);
      PutF32(r + 16, 1.0f - (1.0f - F32(r + 16)) * k);
    }
    // (the records of letters placed there earlier in this run are among them)
    std::vector<uint8_t> old = std::move(used[last]);
    used[last].assign(size_t(pages[last].w) * size_t(pages[last].h), 0);
    std::copy(old.begin(), old.end(), used[last].begin());
    REXLOG_INFO("Font letters: {} page {} grown to {}x{} (no free space left)", name, last, pages[last].w,
                pages[last].h);
    return true;
  };

  int made_count = 0;
  for (Job& job : jobs) {
    const Img& img = job.made.img;
    int p = -1, x = 0, y = 0;
    if (!place_free(img.w, img.h, &p, &x, &y)) {
      if (grown || !grow() || !place_free(img.w, img.h, &p, &x, &y)) {
        REXLOG_WARN("Font letters: {} has no room for U+{:04X}", name, uint32_t(job.letter->character));
        continue;
      }
      grown = true;
    }
    Page& pg = pages[size_t(p)];
    for (int yy = 0; yy < img.h; ++yy)
      for (int xx = 0; xx < img.w; ++xx) pg.pixels[size_t((y + yy) * pg.w + x + xx)] = pg.Nearest(img.at(xx, yy));
    uint8_t* target = record(job.letter->character);
    std::vector<uint8_t> r(kRecord, 0);
    if (job.made.base_record && job.made.base_record->size() == kRecord) {
      r = *job.made.base_record;
      PutF32(&r[20], F32(&r[20]) - float(job.made.widen) / 2.0f);  // left bearing: the widening's half
      PutF32(&r[24], F32(&r[24]) - float(job.made.widen - job.made.widen / 2));  // (right: the rest)
      PutF32(&r[32], F32(&r[20]) + float(img.w) + F32(&r[24]));                   // advance unchanged
    } else {
      PutF32(&r[20], float(f.bearing));
      PutF32(&r[24], float(f.bearing));
      PutF32(&r[32], float(img.w + 2 * f.bearing));
    }
    for (int b = 0; b < 4; ++b) r[size_t(b)] = uint8_t(uint32_t(p) >> (8 * b));
    PutF32(&r[4], float(x) / float(pg.w));
    PutF32(&r[12], float(x + img.w) / float(pg.w));
    PutF32(&r[16], 1.0f - float(y) / float(pg.h));
    PutF32(&r[8], 1.0f - float(y + img.h) / float(pg.h));
    PutF32(&r[28], float(img.w));  // width
    for (int b = 0; b < 4; ++b) r[size_t(36 + b)] = uint8_t(uint32_t(job.letter->character) >> (8 * b));
    std::memcpy(target, r.data(), kRecord);
    ++made_count;
    REXLOG_DEBUG("Font letters: {} U+{:04X} drawn on page {} at {},{} ({}x{})", name,
                 uint32_t(job.letter->character), p, x, y, img.w, img.h);
  }
  REXLOG_INFO("Font letters: {} got {} of {} letters{}", name, made_count, std::size(kLetters),
              grown ? " (one page grown)" : "");
  return made_count > 0;
}

}  // namespace

bool PatchPackage(const data_patcher::Bytes& original, data_patcher::Bytes* patched) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  pure3d::Bytes children;
  int changed = 0;
  for (const pure3d::Chunk& c : top) {
    if (c.id == kChunkTextureFont) {
      pure3d::Node font = pure3d::ToNode(original, c);
      if (PatchFont(font)) {
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

void Register() {
  data_patcher::Register("font letters", data_patcher::kCategoryFrontend, "b4c85fe7", &PatchPackage);
}

uint32_t AddedPages(bool /*large*/) { return 0; }  // (a page is grown, never added: see PatchFont)

char16_t ForFont(char16_t c) {
  if (c == u'«') return kLeftQuote;
  if (c == u'»') return kRightQuote;
  return c;
}

void ForFont(std::u16string& text) {
  for (char16_t& c : text) c = ForFont(c);
}

}  // namespace font_letters
