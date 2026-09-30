// =============================================================================
// native/buffer_cache.h -- the game's vertex and index buffers, as Vulkan buffers
// =============================================================================
//
// WHAT THIS IS FOR
//   The 3D world is made of static meshes (xnPrimBuffer): the game fills a
//   D3D vertex buffer and a 16-bit index buffer once, at load time, and then
//   draws them every frame. The native renderer uploads each buffer to the
//   PC's GPU once and draws from that copy, so a frame's cost doesn't grow
//   with the size of the level.
//
// WHAT AN UPLOAD DOES
//   Guest memory is big-endian. Vertex data goes through the same "endian
//   mode" the Xbox GPU would apply (from the buffer's fetch constant; 8in32 =
//   swap the 4 bytes of every 32-bit word, which suits floats and D3DCOLOR
//   alike), indices through a 16-bit swap. The swapped copy lands in an
//   upload page and a GPU copy moves it into a device-local buffer. After
//   that the data is ordinary little-endian floats and shorts Vulkan reads
//   directly.
//
// WHEN IT RE-UPLOADS
//   Like the texture cache: every buffer used in a frame is hashed once per
//   frame (XXH3, many GB/s), and a changed hash means a new upload into a new
//   buffer (the old one may still be in use by frames in flight, so it's
//   freed later). Simple and always correct, also for buffers the game
//   rewrites (animated or reused memory). If hashing ever shows up in
//   profiles, the game's own Lock/Unlock calls (xnPrimBuffer vtable slots
//   4/5, 7/8) are the natural dirty signal.
//
// MEMORY
//   Buffers come from the SDK's Vulkan Memory Allocator (VMA), which packs
//   many small buffers into few large allocations (a level has hundreds of
//   meshes; Vulkan drivers limit the number of separate allocations).
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/mem_alloc.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

#include "frame.h"

namespace native {

class BufferCache {
 public:
  explicit BufferCache(const rex::ui::vulkan::VulkanDevice* device);
  ~BufferCache();  // the GPU must be done with everything (caller awaits)

  BufferCache(const BufferCache&) = delete;
  BufferCache& operator=(const BufferCache&) = delete;

  // Start of recording a frame: frees buffers and upload space the GPU no
  // longer uses (`completed` = last finished submission) and starts a new
  // "checked this frame" round.
  void BeginFrame(uint64_t completed);

  // The Vulkan buffer holding `buffer`'s data (host byte order, starting at
  // offset 0), or VK_NULL_HANDLE if it can't be read (logged once). If it's
  // new or its data changed, records the upload into `cmd`, which must not be
  // inside a render pass. `submission` = the tracker index `cmd` will be
  // submitted as.
  VkBuffer Get(const GuestBuffer& buffer, VkCommandBuffer cmd, uint64_t submission);

  // After the frame's last Get, before drawing: makes this frame's uploads
  // visible to the vertex input stage (one barrier for all of them).
  void FinishUploads(VkCommandBuffer cmd);
  // Before submitting `cmd`: makes the upload data visible to the GPU.
  void FlushUploads();

  // Stats for the renderer's periodic log line.
  size_t buffer_count() const { return entries_.size(); }
  uint64_t bytes_uploaded() const { return bytes_uploaded_; }

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint64_t last_submission = 0;  // last frame that used it
  };
  struct Entry {
    Buffer buffer;
    uint64_t content_hash = 0;
    uint64_t checked_round = 0;  // BeginFrame round when the hash was last compared
  };

  void DestroyBuffer(Buffer& buffer);
  bool Upload(Entry& entry, const GuestBuffer& guest, const uint8_t* source, VkCommandBuffer cmd,
              uint64_t submission);

  const rex::ui::vulkan::VulkanDevice* device_;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  std::unique_ptr<rex::ui::vulkan::VulkanUploadBufferPool> upload_pool_;
  std::unordered_map<uint64_t, Entry> entries_;  // key: address, size, endian
  std::vector<Buffer> graveyard_;                // replaced buffers, freed when unused
  uint64_t round_ = 0;
  bool uploaded_this_frame_ = false;
  uint64_t bytes_uploaded_ = 0;
};

}  // namespace native
