// =============================================================================
// native/texture_cache.h -- the game's textures, as Vulkan images
// =============================================================================
//
// WHAT A GAME TEXTURE LOOKS LIKE IN MEMORY
//   The Xbox 360 GPU describes a texture with a 6-word "fetch constant":
//   format (k_8_8_8_8, k_DXT1...), size, the address of its pixels, whether
//   they're TILED (stored in 32x32-block tiles in a swizzled order the GPU
//   reads fast) or linear, and an ENDIAN mode (the pixels are big-endian
//   words; e.g. DXT blocks are stored as byte-swapped 16-bit words).
//   D3D keeps that constant in each texture object (guest.h, d3d_texture).
//
// WHAT THIS DOES
//   Get() takes such a fetch constant, finds the pixels in guest memory,
//   untiles them (texture_util::GetTiledOffset2D, the SDK's copy of the
//   GPU's addressing), swaps the bytes back, and uploads the result into a
//   Vulkan image in a format the PC GPU samples directly:
//     k_8 -> R8, k_8_8 -> R8G8, k_8_8_8_8 -> R8G8B8A8,
//     k_DXT1/2_3/4_5 -> BC1/BC2/BC3 (same compressed blocks, PC GPUs read them),
//     16-bit k_5_6_5 / k_1_5_5_5 / k_4_4_4_4 -> expanded to R8G8B8A8 on the CPU.
//   The fetch constant's component swizzle (which channel is red...) goes
//   into the Vulkan image view, so shaders just sample RGBA.
//
// MIPMAPS (docs/findings/17)
//   Most of the 3D world's textures come with MIP LEVELS: the same picture at
//   1/2, 1/4, 1/8... the size, which the GPU uses for surfaces far away or
//   seen at a shallow angle (sampling the full-size picture there picks a
//   few texels out of many: grain and shimmer). The Xbox keeps level 0 at
//   the fetch constant's base address and levels 1+ at its MIP ADDRESS,
//   each level padded to a power of two, and the smallest ones (16 texels
//   or less) squeezed together into one "packed mip tail". The SDK's
//   texture_util knows that layout (GetGuestTextureLayout,
//   GetPackedMipOffset); every stored level is decoded like the base one and
//   uploaded into the image's own mip levels.
//
// WHEN IT RE-UPLOADS
//   Textures are keyed by what the fetch constant says (address, format,
//   size, layout) and the HASH OF THEIR PIXELS (base and mips): the game
//   rewrites some textures (palettes, movie frames, memory reused after an
//   area change), and a hash notices every change. Hashing every used
//   texture every frame cost ~3 ms of the game's main thread in busy scenes,
//   so since 2026-09-29 (docs/findings/20) a texture is WRITE-WATCHED after
//   its hash (write_watch.h: the SDK write-protects its pages and tells us
//   when anything writes there): while nothing was written, the frame's
//   check is a look at a few page counters instead of a hash. Textures that
//   do change get a cool-down (hashed every frame for a while, like before)
//   so memory written every frame doesn't fault every frame; and every
//   watched texture is still hashed once every kVerifyRounds frames as a
//   safety net, which logs if it ever finds a change the watch missed.
//
// LIMITS: 2D textures only.
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

#include "frame.h"
#include "write_watch.h"

namespace native {

class TextureCache {
 public:
  explicit TextureCache(const rex::ui::vulkan::VulkanDevice* device);
  ~TextureCache();  // the GPU must be done with everything (caller awaits)

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Start of recording a frame: frees images and upload space the GPU no
  // longer uses (`completed` = last finished submission) and starts a new
  // "checked this frame" round.
  void BeginFrame(uint64_t completed);

  struct View {
    VkImageView view = VK_NULL_HANDLE;  // null: couldn't decode (logged once)
    uint32_t width = 0;
    uint32_t height = 0;
  };
  // The image of a guest texture. If it's new or its pixels changed, records
  // the upload into `cmd`, which must not be inside a render pass.
  // `submission` = the tracker index `cmd` will be submitted as.
  View Get(const GuestTexture& texture, VkCommandBuffer cmd, uint64_t submission);

  // Before submitting `cmd`: makes the upload data visible to the GPU.
  void FlushUploads();

  // Write watching on/off (on by default). Off = hash every texture every
  // frame. Must be off while textures are read from memory the EMULATED GPU
  // writes (--debug_native_emulated_resolves): its writes bypass the
  // protection (write_watch.h, "what it doesn't see").
  void UseWriteWatch(bool on) { use_write_watch_ = on; }

  // How the per-frame checks went since the last call (the renderer's cost
  // log): `unchanged` = write watch said nothing was written (no hash),
  // `hashed` = hashed (new, changed, cooling down, unwatchable, or the
  // safety net), `uploaded` = the pixels had changed, `missed` = the safety
  // net found a change the watch hadn't reported (should stay 0).
  struct CheckStats {
    uint64_t unchanged = 0, hashed = 0, uploaded = 0, missed = 0;
  };
  CheckStats TakeCheckStats() {
    CheckStats s = stats_;
    stats_ = {};
    return s;
  }

  // Debug aid (ab_capture.cpp): where a texture's base level lives in guest
  // memory, and its pixels as tightly packed R, G, B, A bytes, decoded on
  // the CPU (32-bit RGBA formats only; false otherwise).
  bool GuestExtent(const GuestTexture& texture, uint32_t& address, uint32_t& size) const {
    return SourceExtent(texture, address, size);
  }
  // Guest address of the block holding texel (x, y) of the base level
  // (untiling included); false if outside or unsupported.
  bool TexelAddress(const GuestTexture& texture, uint32_t x, uint32_t y,
                    uint32_t& address) const;
  bool ReadRgba8(const GuestTexture& texture, std::vector<uint8_t>& rgba, uint32_t& width,
                 uint32_t& height) const;

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t last_submission = 0;  // last frame that sampled it
  };
  struct Entry {
    Image image;
    uint32_t width = 0, height = 0;
    uint64_t content_hash = 0;
    uint64_t checked_round = 0;  // BeginFrame round when the hash was last compared
    // Write watch (write_watch.h): armed at the last hash, with this
    // ticket; `cooldown` = checks to go before it may be armed again (set
    // when the watched memory got written: likely a texture that changes).
    bool watched = false;
    uint64_t ticket = 0;
    uint32_t cooldown = 0;
  };

  void DestroyImage(Image& image);
  // One decoded mip level inside the decode buffer.
  struct Level {
    size_t offset;           // bytes from the start of the buffer
    uint32_t width, height;  // texels
  };
  // Decodes the base level, and with `mip_source` (the mips' storage, see
  // MipExtent) the stored mip levels too, into tightly packed host pixels one
  // level after the other. False (and logs once per format) if the format or
  // layout isn't supported yet.
  bool Decode(const GuestTexture& texture, const uint8_t* source, const uint8_t* mip_source,
              std::vector<uint8_t>& out, std::vector<Level>& levels, VkFormat& format,
              VkComponentMapping& swizzle) const;
  // Guest address + byte count of the base level (for hashing and checks).
  bool SourceExtent(const GuestTexture& texture, uint32_t& address, uint32_t& size) const;
  // The same for the mip levels' storage; false if the texture has no mips.
  bool MipExtent(const GuestTexture& texture, uint32_t& address, uint32_t& size) const;
  bool Upload(Entry& entry, const GuestTexture& texture, const uint8_t* source,
              const uint8_t* mip_source, VkCommandBuffer cmd, uint64_t submission);

  const rex::ui::vulkan::VulkanDevice* device_;
  std::unique_ptr<rex::ui::vulkan::VulkanUploadBufferPool> upload_pool_;
  std::unordered_map<uint64_t, Entry> entries_;  // key: hash of the layout words
  std::vector<Image> graveyard_;                 // replaced images, freed when unused
  WriteWatch watch_;
  bool use_write_watch_ = true;
  CheckStats stats_;
  uint32_t missed_logged_ = 0;
  uint64_t round_ = 0;
  uint64_t completed_ = 0;
  std::vector<uint8_t> scratch_;  // decode buffer, reused
  std::vector<Level> scratch_levels_;
};

}  // namespace native
