// =============================================================================
// ui/menu_pictures.cpp -- see menu_pictures.h
// =============================================================================
#include "menu_pictures.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../data/pure3d.h"

namespace menu_pictures {
namespace {

constexpr uint32_t kSprite = 0x19005, kImage = 0x19001, kImageData = 0x19002;

// A name field: length byte (with the NUL, rounded up to 4) + the bytes, NUL
// padded (as the game's files have it; players/more_players_markers.cpp).
Bytes NameField(std::string_view name) {
  const size_t length = (name.size() + 1 + 3) & ~size_t(3);
  Bytes out = {uint8_t(length)};
  out.insert(out.end(), name.begin(), name.end());
  out.resize(1 + length, 0);
  return out;
}

Bytes MakeChunk(uint32_t id, const Bytes& data, const Bytes& children) {
  Bytes out;
  pure3d::PutLe32(out, id);
  pure3d::PutLe32(out, uint32_t(12 + data.size()));
  pure3d::PutLe32(out, uint32_t(12 + data.size() + children.size()));
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), children.begin(), children.end());
  return out;
}

}  // namespace

// MEDIAN CUT: every colour in one box; split the box with the widest spread
// (of R, G, B, A) where half its PIXELS fall on each side, until 255 boxes;
// each box's pixel-weighted average is a palette entry. Entry 0 =
// transparent (pixels with alpha < 4).
Bytes PaletteTga(uint32_t width, uint32_t height, const uint8_t* rgba) {
  const size_t pixels = size_t(width) * height;
  struct Colour {
    uint8_t c[4];
    uint32_t count;
  };
  std::unordered_map<uint32_t, uint32_t> counts;
  for (size_t i = 0; i < pixels; ++i) {
    if (rgba[4 * i + 3] < 4) continue;
    uint32_t key;
    std::memcpy(&key, &rgba[4 * i], 4);
    ++counts[key];
  }
  std::vector<Colour> colours;
  colours.reserve(counts.size());
  for (const auto& [key, count] : counts) {
    Colour c;
    std::memcpy(c.c, &key, 4);
    c.count = count;
    colours.push_back(c);
  }
  struct Box {
    size_t begin, end;
  };
  std::vector<Box> boxes;
  if (!colours.empty()) boxes.push_back({0, colours.size()});
  while (boxes.size() < 255) {
    int pick = -1, pick_channel = 0, pick_spread = 0;
    for (size_t b = 0; b < boxes.size(); ++b) {
      if (boxes[b].end - boxes[b].begin < 2) continue;
      for (int ch = 0; ch < 4; ++ch) {
        uint8_t lo = 255, hi = 0;
        for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
          lo = std::min(lo, colours[i].c[ch]);
          hi = std::max(hi, colours[i].c[ch]);
        }
        if (hi - lo > pick_spread) {
          pick_spread = hi - lo;
          pick = int(b);
          pick_channel = ch;
        }
      }
    }
    if (pick < 0) break;  // every colour has a box of its own
    const Box b = boxes[size_t(pick)];
    std::sort(colours.begin() + long(b.begin), colours.begin() + long(b.end),
              [&](const Colour& x, const Colour& y) { return x.c[pick_channel] < y.c[pick_channel]; });
    uint64_t total = 0, half = 0;
    for (size_t i = b.begin; i < b.end; ++i) total += colours[i].count;
    size_t split = b.begin + 1;
    for (size_t i = b.begin; i < b.end - 1; ++i) {
      half += colours[i].count;
      split = i + 1;
      if (half * 2 >= total) break;
    }
    boxes[size_t(pick)] = {b.begin, split};
    boxes.push_back({split, b.end});
  }
  // The palette (R G B A) and each colour's entry.
  std::array<uint8_t, 256 * 4> palette{};
  std::unordered_map<uint32_t, uint8_t> entry;
  for (size_t b = 0; b < boxes.size(); ++b) {
    uint64_t sum[4] = {}, n = 0;
    for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
      for (int ch = 0; ch < 4; ++ch) sum[ch] += uint64_t(colours[i].c[ch]) * colours[i].count;
      n += colours[i].count;
      uint32_t key;
      std::memcpy(&key, colours[i].c, 4);
      entry[key] = uint8_t(b + 1);
    }
    for (int ch = 0; ch < 4; ++ch) palette[(b + 1) * 4 + ch] = uint8_t(n ? sum[ch] / n : 0);
  }
  // The TGA: 18-byte header, 256 x B G R A, then one byte per pixel.
  Bytes tga(18, 0);
  tga[1] = 1;      // a colour map
  tga[2] = 1;      // colour-mapped image
  tga[5] = 0x00;   // map length 256 (u16 LE at 5)
  tga[6] = 0x01;
  tga[7] = 32;     // map entry bits
  tga[12] = uint8_t(width), tga[13] = uint8_t(width >> 8);
  tga[14] = uint8_t(height), tga[15] = uint8_t(height >> 8);
  tga[16] = 8;     // bits per pixel
  tga[17] = 0x20;  // rows top to bottom
  for (int i = 0; i < 256; ++i) {
    tga.push_back(palette[i * 4 + 2]);
    tga.push_back(palette[i * 4 + 1]);
    tga.push_back(palette[i * 4 + 0]);
    tga.push_back(palette[i * 4 + 3]);
  }
  for (size_t i = 0; i < pixels; ++i) {
    if (rgba[4 * i + 3] < 4) {
      tga.push_back(0);
      continue;
    }
    uint32_t key;
    std::memcpy(&key, &rgba[4 * i], 4);
    tga.push_back(entry[key]);
  }
  return tga;
}

Bytes MakeSprite(std::string_view name, uint32_t width, uint32_t height, const Bytes& tga) {
  Bytes image_data;
  pure3d::PutLe32(image_data, uint32_t(tga.size()));
  image_data.insert(image_data.end(), tga.begin(), tga.end());
  Bytes image_fields = NameField(name);
  for (uint32_t v : {0x36B0u, width, height, 8u, 1u, 1u, 2u}) pure3d::PutLe32(image_fields, v);
  // name, u32 640, u32 480, u32 0, u32 0, an EMPTY name (one length byte
  // 0: the game's own sprites have it; a first version wrote a u32 there,
  // everything after it slid 3 bytes and the loader read the image count as
  // 64 MB: the allocator crashed), u32 1 (images), u32 0.
  Bytes sprite_fields = NameField(name);
  for (uint32_t v : {640u, 480u, 0u, 0u}) pure3d::PutLe32(sprite_fields, v);
  sprite_fields.push_back(0);
  for (uint32_t v : {1u, 0u}) pure3d::PutLe32(sprite_fields, v);
  return MakeChunk(kSprite, sprite_fields, MakeChunk(kImage, image_fields, MakeChunk(kImageData, image_data, {})));
}

}  // namespace menu_pictures
