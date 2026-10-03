// =============================================================================
// data/pure3d.h -- reading and rebuilding Radical's Pure3D package files
// =============================================================================
//
// WHAT: the few helpers the data patches need to take a package (.p3d)
// apart and put a changed one together (data/data_patcher.h). Header-only:
// small, used by more than one patch.
//
// THE FORMAT (Pure3D, little-endian, findings/24 section 7.1): a file is
// ONE root chunk 0xFF443350 ("P3D\xFF") holding all the others. A chunk =
//   u32 id, u32 size of its header + data, u32 total size including its
//   child chunks, then its data, then its children (chunks again).
// Most chunks start their data with a name: u8 length + the bytes (NUL
// padded to the length). Ids used by the front end's packages:
//   0x19005 picture (sprite; a PNG inside), 0x1800D Scrooby font (glyph
//   tables per language), 0x18000 Scrooby project (.prj) whose children are
//   pages 0x18002 (.pag: groups 0x18020 of pictures 0x18022 / texts 0x18023
//   / menus 0x18010) and screens 0x18001 (.scr: a name + a list of pages).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pure3d {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kChunkRoot = 0xFF443350;  // "P3D\xFF"
constexpr uint32_t kChunkProject = 0x18000;  // Scrooby project (.prj)
constexpr uint32_t kChunkScreen = 0x18001;   // Scrooby screen (.scr)
constexpr uint32_t kChunkPage = 0x18002;     // Scrooby page (.pag)
constexpr uint32_t kChunkFont = 0x1800D;     // Scrooby font
constexpr uint32_t kChunkSprite = 0x19005;   // a picture (PNG inside)

inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
inline void PutLe32(Bytes& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

// One chunk of a file: where it starts, its id, header + data size, total.
struct Chunk {
  size_t offset;
  uint32_t id;
  uint32_t data_size;
  uint32_t total_size;
};

// The chunks directly inside [begin, end) (stops at the first one that
// doesn't fit: a damaged or foreign file gives a short list, never a crash).
inline std::vector<Chunk> Children(const Bytes& d, size_t begin, size_t end) {
  std::vector<Chunk> out;
  for (size_t p = begin; p + 12 <= end;) {
    const Chunk c{p, Le32(&d[p]), Le32(&d[p + 4]), Le32(&d[p + 8])};
    if (c.total_size < 12 || c.data_size < 12 || c.data_size > c.total_size ||
        p + c.total_size > end) {
      break;
    }
    out.push_back(c);
    p += c.total_size;
  }
  return out;
}
inline std::vector<Chunk> ChildrenOf(const Bytes& d, const Chunk& c) {
  return Children(d, c.offset + c.data_size, c.offset + c.total_size);
}

// A chunk's name (empty if it has none).
inline std::string_view NameOf(const Bytes& d, const Chunk& c) {
  if (c.data_size < 13) return {};
  const size_t length = d[c.offset + 12];
  if (13 + length > c.data_size) return {};
  std::string_view name(reinterpret_cast<const char*>(&d[c.offset + 13]), length);
  return name.substr(0, name.find('\0'));
}

// A whole chunk (header, data, children) as bytes.
inline Bytes Copy(const Bytes& d, const Chunk& c) {
  return Bytes(d.begin() + c.offset, d.begin() + c.offset + c.total_size);
}

// A file's root chunk and the chunks inside it; false if it isn't Pure3D.
inline bool Parse(const Bytes& d, Chunk* root, std::vector<Chunk>* top) {
  const std::vector<Chunk> roots = Children(d, 0, d.size());
  if (roots.size() != 1 || roots[0].id != kChunkRoot) {
    return false;
  }
  *root = roots[0];
  *top = ChildrenOf(d, *root);
  return true;
}

// Chunk `c` with new children: its own header + data, then `children`
// (the sizes in the header follow).
inline Bytes WithChildren(const Bytes& d, const Chunk& c, const Bytes& children) {
  Bytes out;
  PutLe32(out, c.id);
  PutLe32(out, c.data_size);
  PutLe32(out, uint32_t(c.data_size + children.size()));
  out.insert(out.end(), d.begin() + c.offset + 12, d.begin() + c.offset + c.data_size);
  out.insert(out.end(), children.begin(), children.end());
  return out;
}

}  // namespace pure3d
