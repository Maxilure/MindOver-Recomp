// =============================================================================
// native/texture_cache.cpp -- see texture_cache.h for the why and how
// =============================================================================

#include "texture_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/util.h>

#include "guest.h"
#include "scan.h"

namespace native {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using rex::ui::vulkan::VulkanDevice;

// guest.h: its non-inline helpers.
const uint8_t* guest::Base() {
  auto* memory = rex::system::kernel_memory();
  return memory ? memory->virtual_membase() : nullptr;
}

const uint8_t* guest::TranslateReadable(uint32_t address, uint32_t size) {
  auto* memory = rex::system::kernel_memory();
  if (!memory || !size) {
    return nullptr;
  }
  // Physical memory (the 0xA0000000 / 0xC0000000 / 0xE0000000 windows, where
  // all graphics data lives): read it the way the emulated GPU does, through
  // the SDK's raw view of all 512 MB, mapped once at start ("physical raw" in
  // its map_info), so no read can fault. The heaps' page bookkeeping isn't
  // asked: it can be wrong for graphics memory. How we found it (2026-09-27,
  // the area scan): meshes went missing natively (a collectible's gear and
  // wrench, an object on a dig spot) because their buffers counted as
  // "unreadable", e.g. an index buffer at EF2DB000 not mapped in the 0xE0
  // window; the emulated GPU drew them from the same memory. (The game's
  // "BaseHeap::Release failed ... not a region start" errors show the
  // bookkeeping getting confused around area changes.)
  const uint32_t physical = memory->GetPhysicalAddress(address);
  if (physical != UINT32_MAX) {
    if (uint64_t(physical) + size > 0x20000000u) {
      return nullptr;
    }
    return memory->TranslatePhysical<const uint8_t*>(physical);
  }
  // Other (virtual) memory: both ends must be in readable pages (a texture
  // never spans a hole).
  for (uint32_t probe : {address, address + size - 1}) {
    auto* heap = memory->LookupHeap(probe);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(probe, &protect) ||
        !(protect & rex::memory::kMemoryProtectRead)) {
      return nullptr;
    }
  }
  return memory->TranslateVirtual<const uint8_t*>(address);
}

uint32_t guest::ReadableAddressOfPhysical(uint32_t physical) {
  physical &= 0x1FFFFFFFu;
  if (physical < 0x1000u) {
    return 0;  // nothing the GPU samples lives in the first page
  }
  // The three windows (guest.h, PhysicalAddress; the 0xE0000000 one is
  // 4 KB off). A page allocated through one window is mapped only there.
  const uint32_t candidates[3] = {0xA0000000u + physical, 0xC0000000u + physical,
                                  0xE0000000u + physical - 0x1000u};
  for (uint32_t candidate : candidates) {
    if (TranslateReadable(candidate, 1)) {
      return candidate;
    }
  }
  return 0;
}

namespace {

// Upload pages must fit the biggest single texture with its mips (2048x2048
// RGBA8 = 16 MiB, + 1/3 for the mips).
constexpr size_t kUploadPageSize = size_t(24) << 20;

xenos::xe_gpu_texture_fetch_t ToFetch(const GuestTexture& texture) {
  xenos::xe_gpu_texture_fetch_t f;
  f.dword_0 = texture.fetch[0];
  f.dword_1 = texture.fetch[1];
  f.dword_2 = texture.fetch[2];
  f.dword_3 = texture.fetch[3];
  f.dword_4 = texture.fetch[4];
  f.dword_5 = texture.fetch[5];
  return f;
}

// What identifies a texture's image: layout words only, not the sampling
// state (filter/clamp bits) that shares the same words.
uint64_t LayoutKey(const GuestTexture& t) {
  const uint32_t words[6] = {
      t.fetch[0] & 0xFFC00000,  // tiled + pitch
      t.fetch[1] & 0xFFFFF0FF,  // base address + format + endianness
      t.fetch[2],               // size
      t.fetch[3] & 0x00001FFE,  // component swizzle
      t.fetch[4] & 0x000003FC,  // mip min / max level
      t.fetch[5] & 0xFFFFFE00,  // dimension + packed mips + mip address
  };
  return XXH3_64bits(words, sizeof(words));
}

// How each supported Xbox format becomes a Vulkan one.
struct FormatMapping {
  VkFormat format;      // what we upload
  uint32_t components;  // components the Xbox format has (for the swizzle)
  bool expand16;        // 16-bit packed: expanded to R8G8B8A8 on the CPU
};
bool MapFormat(xenos::TextureFormat format, FormatMapping& out) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      out = {VK_FORMAT_R8_UNORM, 1, false};
      return true;
    case F::k_8_8:
      out = {VK_FORMAT_R8G8_UNORM, 2, false};
      return true;
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
      out = {VK_FORMAT_R8G8B8A8_UNORM, 4, false};
      return true;
    case F::k_DXT1:
      out = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, false};
      return true;
    case F::k_DXT2_3:
      out = {VK_FORMAT_BC2_UNORM_BLOCK, 4, false};
      return true;
    case F::k_DXT4_5:
      out = {VK_FORMAT_BC3_UNORM_BLOCK, 4, false};
      return true;
    case F::k_5_6_5:
      out = {VK_FORMAT_R8G8B8A8_UNORM, 3, true};
      return true;
    case F::k_1_5_5_5:
    case F::k_4_4_4_4:
      out = {VK_FORMAT_R8G8B8A8_UNORM, 4, true};
      return true;
    default:
      return false;
  }
}

// One 16-bit Xbox texel -> RGBA8 (red in the low byte). The Xbox packs the
// first component (X) in the lowest bits (the SDK's texture cache swaps the
// same way for the PC's packed formats).
uint32_t Expand16(xenos::TextureFormat format, uint16_t v) {
  auto scale = [](uint32_t value, uint32_t bits) { return value * 255 / ((1u << bits) - 1); };
  uint32_t x, y, z, w;
  switch (format) {
    case xenos::TextureFormat::k_5_6_5:
      x = scale(v & 31, 5), y = scale((v >> 5) & 63, 6), z = scale(v >> 11, 5);
      w = z;  // missing components repeat the last one (the GPU's rule)
      break;
    case xenos::TextureFormat::k_1_5_5_5:
      x = scale(v & 31, 5), y = scale((v >> 5) & 31, 5), z = scale((v >> 10) & 31, 5);
      w = (v >> 15) ? 255 : 0;
      break;
    default:  // k_4_4_4_4
      x = scale(v & 15, 4), y = scale((v >> 4) & 15, 4), z = scale((v >> 8) & 15, 4);
      w = scale(v >> 12, 4);
      break;
  }
  return x | (y << 8) | (z << 16) | (w << 24);
}

// Undo the GPU endian mode over a whole buffer (size a multiple of 4).
void SwapEndian(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

// The fetch constant's swizzle -> a Vulkan component mapping. Xbox rule:
// a texture's missing components repeat its last one before swizzling
// (e.g. k_8 reads as XXXX), so X..W map onto the channels that exist.
VkComponentMapping ToComponentMapping(uint32_t swizzle, uint32_t components) {
  auto channel = [&](uint32_t source) {
    switch (source) {
      case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
        return VK_COMPONENT_SWIZZLE_ZERO;
      case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
        return VK_COMPONENT_SWIZZLE_ONE;
      default: {
        const uint32_t present = std::min(source & 3, components - 1);
        return VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + present);
      }
    }
  };
  return {channel(swizzle & 7), channel((swizzle >> 3) & 7), channel((swizzle >> 6) & 7),
          channel((swizzle >> 9) & 7)};
}

uint32_t Log2(uint32_t v) { return uint32_t(31 - __builtin_clz(v)); }

// Base-level geometry of a texture, in blocks.
struct Layout {
  const rex::graphics::FormatInfo* info;
  uint32_t width, height;          // texels
  uint32_t blocks_x, blocks_y;     // visible blocks
  uint32_t offset_x, offset_y;     // blocks, if the base sits in a packed mip tail
  uint32_t pitch_blocks;           // row pitch in blocks
  uint32_t bytes_per_block, bpb_log2;
  uint32_t linear_row_bytes;       // row pitch of linear textures
  bool tiled;
};

bool GetLayout(const xenos::xe_gpu_texture_fetch_t& f, Layout& l) {
  if (f.dimension != xenos::DataDimension::k2DOrStacked) {
    return false;
  }
  l.info = rex::graphics::FormatInfo::Get(f.format);
  if (!l.info || !l.info->bits_per_pixel) {
    return false;
  }
  l.width = f.size_2d.width + 1;
  l.height = f.size_2d.height + 1;
  const uint32_t bw = l.info->block_width, bh = l.info->block_height;
  l.blocks_x = (l.width + bw - 1) / bw;
  l.blocks_y = (l.height + bh - 1) / bh;
  l.bytes_per_block = l.info->bytes_per_block();
  if (!l.bytes_per_block || (l.bytes_per_block & (l.bytes_per_block - 1))) {
    return false;
  }
  l.bpb_log2 = Log2(l.bytes_per_block);
  // Pitch: in texels / 32 in the fetch constant.
  l.pitch_blocks = ((uint32_t(f.pitch) << 5) + bw - 1) / bw;
  l.linear_row_bytes = (l.pitch_blocks * l.bytes_per_block + 255) & ~255u;
  l.tiled = f.tiled != 0;
  l.offset_x = l.offset_y = 0;
  if (f.packed_mips && texture_util::GetPackedMipLevel(l.width, l.height) == 0) {
    // Tiny texture (16 px or less): its base level is inside a mip tail.
    uint32_t z = 0;
    texture_util::GetPackedMipOffset(l.width, l.height, 1, f.format, 0, l.offset_x, l.offset_y,
                                     z);
  }
  return true;
}

// Byte count from the base address that the base level may touch.
uint32_t SourceBytes(const Layout& l) {
  if (l.tiled) {
    return texture_util::GetTiledAddressUpperBound2D(l.offset_x + l.blocks_x,
                                                     l.offset_y + l.blocks_y, l.pitch_blocks,
                                                     l.bpb_log2);
  }
  return (l.offset_y + l.blocks_y - 1) * l.linear_row_bytes +
         (l.offset_x + l.blocks_x) * l.bytes_per_block;
}

// Where one mip level's blocks are, in a byte-swapped copy of its storage.
struct LevelSource {
  const uint8_t* data;
  size_t size;
  bool tiled;
  uint32_t pitch_blocks;        // tiled: row pitch in blocks (for the addressing)
  size_t row_bytes;             // linear: bytes from one row of blocks to the next
  uint32_t offset_x, offset_y;  // blocks: the level's corner (inside a packed mip tail)
  uint32_t blocks_x, blocks_y;  // the level's size in blocks
};

// Copies a level's blocks into tightly packed rows at `out` (untiling a
// tiled one). False if a block falls outside the storage (shouldn't happen).
bool CopyBlocks(const LevelSource& s, uint32_t bpb, uint32_t bpb_log2, uint8_t* out) {
  const size_t row = size_t(s.blocks_x) * bpb;
  for (uint32_t y = 0; y < s.blocks_y; ++y) {
    uint8_t* dst = out + size_t(y) * row;
    if (!s.tiled) {
      const size_t at = size_t(s.offset_y + y) * s.row_bytes + size_t(s.offset_x) * bpb;
      if (at + row > s.size) {
        return false;
      }
      std::memcpy(dst, s.data + at, row);
      continue;
    }
    for (uint32_t x = 0; x < s.blocks_x; ++x) {
      const int32_t offset = texture_util::GetTiledOffset2D(
          int32_t(s.offset_x + x), int32_t(s.offset_y + y), s.pitch_blocks, bpb_log2);
      if (offset < 0 || size_t(offset) + bpb > s.size) {
        return false;
      }
      std::memcpy(dst + size_t(x) * bpb, s.data + offset, bpb);
    }
  }
  return true;
}

// A texture's mip levels (texture_util.h explains the Xbox's layout): their
// storage starts at the fetch constant's mip address; GetGuestTextureLayout
// says where each stored level sits from there and its row pitch. Levels
// from `packed_level` on share one "packed mip tail" stored like that level,
// each at its own corner (GetPackedMipOffset).
struct MipLayout {
  uint32_t address = 0;    // guest address of the mip storage (virtual, like the base's)
  uint32_t size = 0;       // bytes from there the stored levels may touch
  uint32_t max_level = 0;  // the last level; 0 = no mips
  texture_util::TextureGuestLayout guest;
};
bool GetMipLayout(const xenos::xe_gpu_texture_fetch_t& f, const Layout& l, MipLayout& m) {
  // The level range, cleaned up by the SDK (no mip address = no mips,
  // levels clamped to the size). Its page numbers are physical; we read
  // through the fetch constant's own (virtual) address instead.
  uint32_t mip_page = 0, min_level = 0, max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(f, nullptr, nullptr, nullptr, nullptr, &mip_page,
                                                 &min_level, &max_level);
  if (!mip_page || max_level == 0) {
    return false;
  }
  m.guest = texture_util::GetGuestTextureLayout(f.dimension, f.pitch, l.width, l.height, 1,
                                                f.tiled != 0, f.format, f.packed_mips != 0,
                                                true, max_level);
  m.max_level = m.guest.max_level;
  m.address = uint32_t(f.mip_address) << 12;
  m.size = (m.guest.mips_total_extent_bytes + 3) & ~3u;
  return m.max_level > 0 && m.size > 0 && m.address != 0;
}

// Log each unsupported thing once (Get runs every frame).
void ReportOnce(const std::string& what) {
  static std::mutex mutex;
  static std::unordered_set<std::string> reported;
  std::lock_guard<std::mutex> lock(mutex);
  if (reported.insert(what).second) {
    REXLOG_WARN("NativeRenderer: texture not supported yet: {}", what);
    native::scan::NewGap("texture: " + what);  // with --trace: record this spot (scan.h)
  }
}

}  // namespace

TextureCache::TextureCache(const VulkanDevice* device)
    : device_(device),
      upload_pool_(std::make_unique<rex::ui::vulkan::VulkanUploadBufferPool>(
          device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kUploadPageSize)) {}

TextureCache::~TextureCache() {
  for (auto& [key, entry] : entries_) {
    DestroyImage(entry.image);
  }
  for (Image& image : graveyard_) {
    DestroyImage(image);
  }
}

void TextureCache::DestroyImage(Image& image) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  using rex::ui::vulkan::util::DestroyAndNullHandle;
  DestroyAndNullHandle(dfn.vkDestroyImageView, device, image.view);
  DestroyAndNullHandle(dfn.vkDestroyImage, device, image.image);
  DestroyAndNullHandle(dfn.vkFreeMemory, device, image.memory);
}

void TextureCache::BeginFrame(uint64_t completed) {
  completed_ = completed;
  ++round_;
  upload_pool_->Reclaim(completed);
  // Replaced images: free those the GPU finished with.
  auto done = std::remove_if(graveyard_.begin(), graveyard_.end(), [&](Image& image) {
    if (image.last_submission > completed) {
      return false;
    }
    DestroyImage(image);
    return true;
  });
  graveyard_.erase(done, graveyard_.end());
  // Forget textures unused for a while (the game frees and reuses memory;
  // a stale entry would only waste VRAM). Checked every 64 frames.
  constexpr uint64_t kUnusedRounds = 600;  // ~10 s at 60 fps
  if ((round_ & 63) == 0) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (round_ - it->second.checked_round > kUnusedRounds) {
        if (it->second.image.image) {
          graveyard_.push_back(it->second.image);
        }
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void TextureCache::FlushUploads() { upload_pool_->FlushWrites(); }

bool TextureCache::SourceExtent(const GuestTexture& texture, uint32_t& address,
                                uint32_t& size) const {
  const xenos::xe_gpu_texture_fetch_t f = ToFetch(texture);
  Layout l;
  if (!GetLayout(f, l)) {
    return false;
  }
  // The D3D texture object holds the VIRTUAL address (its SetTexture turns it
  // into a physical one for the GPU); we read through the virtual mapping.
  address = uint32_t(f.base_address) << 12;
  size = (SourceBytes(l) + 3) & ~3u;
  return address != 0 && size != 0;
}

bool TextureCache::MipExtent(const GuestTexture& texture, uint32_t& address,
                             uint32_t& size) const {
  const xenos::xe_gpu_texture_fetch_t f = ToFetch(texture);
  Layout l;
  MipLayout m;
  if (!GetLayout(f, l) || !GetMipLayout(f, l, m)) {
    return false;
  }
  address = m.address;
  size = m.size;
  return true;
}

bool TextureCache::Decode(const GuestTexture& texture, const uint8_t* source,
                          const uint8_t* mip_source, std::vector<uint8_t>& out,
                          std::vector<Level>& levels, VkFormat& format,
                          VkComponentMapping& swizzle) const {
  const xenos::xe_gpu_texture_fetch_t f = ToFetch(texture);
  Layout l;
  FormatMapping mapping;
  if (!GetLayout(f, l) || !MapFormat(f.format, mapping)) {
    ReportOnce(std::string("format ") + (l.info ? l.info->name : "?") + " (" +
               std::to_string(uint32_t(f.format)) + "), dimension " +
               std::to_string(uint32_t(f.dimension)));
    return false;
  }
  format = mapping.format;
  swizzle = ToComponentMapping(f.swizzle, mapping.components);
  const uint32_t bpb = l.bytes_per_block;
  const uint32_t bw = l.info->block_width, bh = l.info->block_height;

  // 1. Copy each storage (base, mips) and undo the endian mode on the copy
  //    (tiling keeps 16/32-bit words intact, so swapping before untiling is
  //    safe).
  const uint32_t source_size = (SourceBytes(l) + 3) & ~3u;
  std::vector<uint8_t> swapped(source, source + source_size);
  SwapEndian(swapped.data(), swapped.size(), f.endianness);
  MipLayout m;
  std::vector<uint8_t> mips_swapped;
  if (mip_source && GetMipLayout(f, l, m)) {
    mips_swapped.assign(mip_source, mip_source + m.size);
    SwapEndian(mips_swapped.data(), mips_swapped.size(), f.endianness);
  } else {
    m.max_level = 0;
  }

  // 2. Untile / de-pitch every level into tightly packed rows of blocks,
  //    one after the other: level 0 from the base storage, levels 1+ from
  //    the mip storage (inside the packed tail from its level on).
  std::vector<uint8_t> packed;
  levels.clear();
  for (uint32_t level = 0; level <= m.max_level; ++level) {
    const uint32_t width = std::max(l.width >> level, 1u);
    const uint32_t height = std::max(l.height >> level, 1u);
    LevelSource s;
    s.tiled = l.tiled;
    s.blocks_x = (width + bw - 1) / bw;
    s.blocks_y = (height + bh - 1) / bh;
    if (level == 0) {
      s.data = swapped.data();
      s.size = swapped.size();
      s.pitch_blocks = l.pitch_blocks;
      s.row_bytes = l.linear_row_bytes;
      s.offset_x = l.offset_x;
      s.offset_y = l.offset_y;
    } else {
      // The storage level holding this one: itself, or the packed tail.
      const uint32_t stored = std::min(level, m.guest.packed_level);
      const auto& stored_layout = m.guest.mips[stored];
      const uint32_t offset = m.guest.mip_offsets_bytes[stored];
      if (offset >= mips_swapped.size() || !stored_layout.row_pitch_bytes) {
        break;  // shouldn't happen: keep the levels decoded so far
      }
      s.data = mips_swapped.data() + offset;
      s.size = mips_swapped.size() - offset;
      s.pitch_blocks = stored_layout.row_pitch_bytes / bpb;
      s.row_bytes = stored_layout.row_pitch_bytes;
      s.offset_x = s.offset_y = 0;
      if (level >= m.guest.packed_level) {
        uint32_t z = 0;
        texture_util::GetPackedMipOffset(l.width, l.height, 1, f.format, level, s.offset_x,
                                         s.offset_y, z);
      }
    }
    const size_t level_offset = packed.size();
    packed.resize(level_offset + size_t(s.blocks_x) * bpb * s.blocks_y);
    if (!CopyBlocks(s, bpb, l.bpb_log2, packed.data() + level_offset)) {
      packed.resize(level_offset);
      if (level == 0) {
        return false;  // shouldn't happen: SourceBytes covers every block
      }
      break;  // a mip we can't read: keep the levels before it
    }
    levels.push_back({level_offset, width, height});
  }

  // 3. 16-bit packed formats -> RGBA8 (each level's offset doubles).
  if (mapping.expand16) {
    std::vector<uint8_t> expanded;
    for (Level& level : levels) {
      const size_t texels = size_t(level.width) * level.height;
      const uint8_t* src = packed.data() + level.offset;
      level.offset = expanded.size();
      expanded.resize(level.offset + texels * 4);
      for (size_t i = 0; i < texels; ++i) {
        uint16_t v;
        std::memcpy(&v, src + 2 * i, 2);
        const uint32_t rgba = Expand16(f.format, v);
        std::memcpy(expanded.data() + level.offset + 4 * i, &rgba, 4);
      }
    }
    out.swap(expanded);
  } else {
    out.swap(packed);
  }
  return true;
}

bool TextureCache::TexelAddress(const GuestTexture& texture, uint32_t x, uint32_t y,
                                uint32_t& address) const {
  const xenos::xe_gpu_texture_fetch_t f = ToFetch(texture);
  Layout l;
  if (!GetLayout(f, l) || x >= l.width || y >= l.height) {
    return false;
  }
  const uint32_t bx = x / l.info->block_width + l.offset_x;
  const uint32_t by = y / l.info->block_height + l.offset_y;
  const int32_t offset =
      l.tiled ? texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by), l.pitch_blocks,
                                               l.bpb_log2)
              : int32_t(by * l.linear_row_bytes + bx * l.bytes_per_block);
  if (offset < 0) {
    return false;
  }
  address = (uint32_t(f.base_address) << 12) + uint32_t(offset);
  return true;
}

bool TextureCache::ReadRgba8(const GuestTexture& texture, std::vector<uint8_t>& rgba,
                             uint32_t& width, uint32_t& height) const {
  uint32_t address, size;
  if (!SourceExtent(texture, address, size)) {
    return false;
  }
  const uint8_t* source = guest::TranslateReadable(address, size);
  std::vector<uint8_t> pixels;
  std::vector<Level> levels;
  VkFormat format;
  VkComponentMapping swizzle;
  if (!source || !Decode(texture, source, nullptr, pixels, levels, format, swizzle) ||
      format != VK_FORMAT_R8G8B8A8_UNORM || levels.empty()) {
    return false;
  }
  width = levels[0].width;
  height = levels[0].height;
  // Apply the fetch constant's swizzle (what the image view would do).
  auto pick = [](const uint8_t* texel, VkComponentSwizzle s, uint32_t identity) -> uint8_t {
    switch (s) {
      case VK_COMPONENT_SWIZZLE_ZERO: return 0;
      case VK_COMPONENT_SWIZZLE_ONE: return 255;
      case VK_COMPONENT_SWIZZLE_R: return texel[0];
      case VK_COMPONENT_SWIZZLE_G: return texel[1];
      case VK_COMPONENT_SWIZZLE_B: return texel[2];
      case VK_COMPONENT_SWIZZLE_A: return texel[3];
      default: return texel[identity];
    }
  };
  rgba.resize(size_t(width) * height * 4);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    const uint8_t* texel = pixels.data() + 4 * i;
    rgba[4 * i + 0] = pick(texel, swizzle.r, 0);
    rgba[4 * i + 1] = pick(texel, swizzle.g, 1);
    rgba[4 * i + 2] = pick(texel, swizzle.b, 2);
    rgba[4 * i + 3] = pick(texel, swizzle.a, 3);
  }
  return true;
}

bool TextureCache::Upload(Entry& entry, const GuestTexture& texture, const uint8_t* source,
                          const uint8_t* mip_source, VkCommandBuffer cmd, uint64_t submission) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  VkFormat format;
  VkComponentMapping swizzle;
  if (!Decode(texture, source, mip_source, scratch_, scratch_levels_, format, swizzle) ||
      scratch_levels_.empty()) {
    return false;
  }
  const uint32_t width = scratch_levels_[0].width, height = scratch_levels_[0].height;
  const uint32_t level_count = uint32_t(scratch_levels_.size());

  // A new image every time the pixels change: the old one may still be in
  // use by frames in flight, so it goes to the graveyard (BeginFrame).
  if (entry.image.image) {
    graveyard_.push_back(entry.image);
    entry.image = Image{};
  }
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = level_count;  // the base + every stored mip level
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          entry.image.image, entry.image.memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = entry.image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.components = swizzle;
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &entry.image.view) != VK_SUCCESS) {
    DestroyImage(entry.image);
    return false;
  }

  // Staging copy, then GPU copy into the image.
  if (scratch_.size() > kUploadPageSize) {
    ReportOnce("texture bigger than the upload page (" + std::to_string(width) + "x" +
               std::to_string(height) + ")");
    DestroyImage(entry.image);
    return false;
  }
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = upload_pool_->Request(submission, scratch_.size(), 16, buffer, offset);
  if (!mapping) {
    DestroyImage(entry.image);
    return false;
  }
  std::memcpy(mapping, scratch_.data(), scratch_.size());

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = entry.image.image;
  barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
  // One copy per level, each tightly packed in the upload (Decode's layout).
  // Every level's offset is a multiple of its block size: fine for Vulkan.
  VkBufferImageCopy regions[xenos::kTextureMaxMips]{};
  for (uint32_t i = 0; i < level_count && i < xenos::kTextureMaxMips; ++i) {
    regions[i].bufferOffset = offset + scratch_levels_[i].offset;
    regions[i].bufferRowLength = 0;  // tightly packed
    regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    regions[i].imageExtent = {scratch_levels_[i].width, scratch_levels_[i].height, 1};
  }
  dfn.vkCmdCopyBufferToImage(cmd, buffer, entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             std::min<uint32_t>(level_count, xenos::kTextureMaxMips), regions);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  entry.width = width;
  entry.height = height;
  // Debug level (--log_level=debug): which formats and layouts the game uses.
  const xenos::xe_gpu_texture_fetch_t f = ToFetch(texture);
  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(f.format);
  REXLOG_DEBUG("NativeRenderer: texture {}x{} {} {} endian {} at {:08X}, {} level(s)", width,
               height, info ? info->name : "?", f.tiled ? "tiled" : "linear",
               uint32_t(f.endianness), uint32_t(f.base_address) << 12, level_count);
  return true;
}

TextureCache::View TextureCache::Get(const GuestTexture& texture, VkCommandBuffer cmd,
                                     uint64_t submission) {
  if (!texture.present()) {
    return {};
  }
  const uint64_t key = LayoutKey(texture);
  Entry& entry = entries_[key];
  if (entry.checked_round != round_) {
    // First use this frame: have the pixels changed?
    entry.checked_round = round_;
    uint32_t address = 0, size = 0;
    const bool has_base = SourceExtent(texture, address, size);
    uint32_t mip_address = 0, mip_size = 0;
    const bool has_mips = MipExtent(texture, mip_address, mip_size);
    // The write watch's answer, if the texture is watched (texture_cache.h,
    // "when it re-uploads").
    const bool watched = use_write_watch_ && entry.watched && has_base;
    const bool written =
        watched && (watch_.Written(address, size, entry.ticket) ||
                    (has_mips && watch_.Written(mip_address, mip_size, entry.ticket)));
    // Safety net: every kVerifyRounds frames each watched texture is hashed
    // anyway (spread over frames by its key, so not all at once).
    constexpr uint64_t kVerifyRounds = 32;
    const bool verify = ((round_ + key) % kVerifyRounds) == 0;
    if (watched && !written && !verify) {
      ++stats_.unchanged;  // nothing wrote there: same pixels, no hash needed
    } else {
      ++stats_.hashed;
      entry.watched = false;
      // Written while watched: probably a texture that changes (a movie
      // frame, a palette), so it's hashed every frame for a while instead
      // of faulting on every write. ~2 s at 60 fps.
      constexpr uint32_t kCooldownChecks = 120;
      if (written) {
        entry.cooldown = kCooldownChecks;
      } else if (entry.cooldown > 0) {
        --entry.cooldown;
      }
      const uint8_t* source = has_base ? guest::TranslateReadable(address, size) : nullptr;
      if (!source) {
        ReportOnce("unreadable texture memory");
        entry.content_hash = 0;
        return {};
      }
      // The mips, if the texture has them and their memory is readable (else
      // only the base level is used).
      const uint8_t* mip_source = nullptr;
      if (has_mips) {
        mip_source = guest::TranslateReadable(mip_address, mip_size);
        if (!mip_source) {
          ReportOnce("unreadable mip memory (base level used alone)");
        }
      }
      // Arm the watch BEFORE reading the pixels (write_watch.h, ordering):
      // one ticket for both ranges, taken before either is protected.
      if (use_write_watch_ && entry.cooldown == 0 && watch_.Watchable(address, size) &&
          (!mip_source || watch_.Watchable(mip_address, mip_size))) {
        entry.ticket = watch_.Arm(address, size);
        if (mip_source) {
          watch_.Arm(mip_address, mip_size);
        }
        entry.watched = true;
      }
      uint64_t hash = XXH3_64bits(source, size);
      if (mip_source) {
        hash = XXH3_64bits_withSeed(mip_source, mip_size, hash);
      }
      if (hash != entry.content_hash) {
        if (watched && !written) {
          // Only the safety net got here: the pixels changed without the
          // watch noticing. Should never happen; if it does, a writer
          // bypasses the protection (write_watch.h, "what it doesn't see").
          ++stats_.missed;
          if (missed_logged_ < 16) {  // the first few are enough to investigate
            ++missed_logged_;
            REXLOG_WARN("NativeRenderer: a texture at {:08X} changed unseen by the write watch "
                        "(found by the periodic re-hash)",
                        address);
          }
        }
        ++stats_.uploaded;
        // On failure (unsupported format, logged once) the hash is still
        // remembered, so it's only retried once the pixels change.
        entry.content_hash = hash;
        Upload(entry, texture, source, mip_source, cmd, submission);
      }
    }
  }
  if (!entry.image.view) {
    return {};
  }
  entry.image.last_submission = submission;
  return {entry.image.view, entry.width, entry.height};
}

}  // namespace native
