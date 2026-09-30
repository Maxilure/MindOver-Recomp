// =============================================================================
// native/buffer_cache.cpp -- see buffer_cache.h for the why and how
// =============================================================================

#include "buffer_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/hash.h>
#include <rex/logging.h>

#include "guest.h"
#include "scan.h"

namespace native {

using rex::ui::vulkan::VulkanDevice;

namespace {

// Upload pages must fit the biggest single buffer. Level meshes are well
// under a megabyte each; 16 MiB leaves plenty of room.
constexpr size_t kUploadPageSize = size_t(16) << 20;

// One number per (address, size, endian): the same memory read with another
// swap is a different buffer on our side.
uint64_t Key(const GuestBuffer& b) {
  return uint64_t(b.address) | (uint64_t(b.size & 0x3FFFFFFF) << 32) |
         (uint64_t(b.endian & 3) << 62);
}

// Undo the Xbox GPU's endian mode (xenos::Endian) over `size` bytes.
void Swap(uint8_t* data, size_t size, uint8_t endian) {
  switch (endian) {
    case 1:  // 8in16: swap the bytes of each 16-bit value (indices)
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case 2:  // 8in32: reverse each 32-bit word (floats, D3DCOLOR)
      for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(data + i, &v, 4);
      }
      break;
    case 3:  // 16in32: swap the two halves of each 32-bit word
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:  // 0: as is
      break;
  }
}

void ReportOnce(const std::string& what) {
  static std::mutex mutex;
  static std::unordered_set<std::string> reported;
  std::lock_guard<std::mutex> lock(mutex);
  if (reported.insert(what).second) {
    REXLOG_WARN("NativeRenderer: buffer not supported yet: {}", what);
    native::scan::NewGap("buffer: " + what);  // with --trace: record this spot (scan.h)
  }
}

}  // namespace

BufferCache::BufferCache(const VulkanDevice* device)
    : device_(device),
      allocator_(rex::ui::vulkan::CreateVmaAllocator(device, true)),
      upload_pool_(std::make_unique<rex::ui::vulkan::VulkanUploadBufferPool>(
          device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kUploadPageSize)) {}

BufferCache::~BufferCache() {
  for (auto& [key, entry] : entries_) {
    DestroyBuffer(entry.buffer);
  }
  for (Buffer& buffer : graveyard_) {
    DestroyBuffer(buffer);
  }
  upload_pool_.reset();
  if (allocator_) {
    vmaDestroyAllocator(allocator_);
  }
}

void BufferCache::DestroyBuffer(Buffer& buffer) {
  if (buffer.buffer) {
    vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
  }
  buffer = Buffer{};
}

void BufferCache::BeginFrame(uint64_t completed) {
  ++round_;
  uploaded_this_frame_ = false;
  upload_pool_->Reclaim(completed);
  auto done = std::remove_if(graveyard_.begin(), graveyard_.end(), [&](Buffer& buffer) {
    if (buffer.last_submission > completed) {
      return false;
    }
    DestroyBuffer(buffer);
    return true;
  });
  graveyard_.erase(done, graveyard_.end());
  // Forget buffers unused for a while (a level unloaded, memory reused).
  // Checked every 64 frames.
  constexpr uint64_t kUnusedRounds = 600;  // ~10 s at 60 fps
  if ((round_ & 63) == 0) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (round_ - it->second.checked_round > kUnusedRounds) {
        if (it->second.buffer.buffer) {
          graveyard_.push_back(it->second.buffer);
        }
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void BufferCache::FinishUploads(VkCommandBuffer cmd) {
  if (!uploaded_this_frame_) {
    return;
  }
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
  device_->functions().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier,
                                            0, nullptr, 0, nullptr);
  uploaded_this_frame_ = false;
}

void BufferCache::FlushUploads() { upload_pool_->FlushWrites(); }

bool BufferCache::Upload(Entry& entry, const GuestBuffer& guest, const uint8_t* source,
                         VkCommandBuffer cmd, uint64_t submission) {
  if (guest.size > kUploadPageSize) {
    ReportOnce("bigger than the upload page (" + std::to_string(guest.size) + " bytes)");
    return false;
  }
  // A new buffer every time the data changes: the old one may still be in
  // use by frames in flight, so it goes to the graveyard (BeginFrame).
  if (entry.buffer.buffer) {
    graveyard_.push_back(entry.buffer);
    entry.buffer = Buffer{};
  }
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = guest.size;
  buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &entry.buffer.buffer,
                      &entry.buffer.allocation, nullptr) != VK_SUCCESS) {
    ReportOnce("out of GPU memory?");
    entry.buffer = Buffer{};
    return false;
  }

  // Staging copy (swapped into host order on the way), then a GPU copy.
  VkBuffer staging;
  VkDeviceSize staging_offset;
  uint8_t* mapping = upload_pool_->Request(submission, guest.size, 16, staging, staging_offset);
  if (!mapping) {
    DestroyBuffer(entry.buffer);
    return false;
  }
  std::memcpy(mapping, source, guest.size);
  Swap(mapping, guest.size, guest.endian);
  VkBufferCopy region{staging_offset, 0, guest.size};
  device_->functions().vkCmdCopyBuffer(cmd, staging, entry.buffer.buffer, 1, &region);
  uploaded_this_frame_ = true;
  bytes_uploaded_ += guest.size;
  REXLOG_DEBUG("NativeRenderer: buffer {:08X}, {} bytes, endian {}", guest.address, guest.size,
               guest.endian);
  return true;
}

VkBuffer BufferCache::Get(const GuestBuffer& guest, VkCommandBuffer cmd, uint64_t submission) {
  if (!guest.present()) {
    return VK_NULL_HANDLE;
  }
  Entry& entry = entries_[Key(guest)];
  if (entry.checked_round != round_) {
    // First use this frame: has the data changed?
    entry.checked_round = round_;
    // (Graphics memory is always readable since 2026-09-27: guest.h,
    // TranslateReadable. The area scan caught an index buffer at
    // EF2DB000 counted as unreadable, and meshes went missing because of it.)
    const uint8_t* source = guest::TranslateReadable(guest.address, guest.size);
    if (!source) {
      ReportOnce("unreadable memory");
      // Which buffers: each address once (the first 16), with its size and
      // byte order, to tell a bad pointer from an unexpected memory range.
      static std::mutex logged_mutex;
      static std::unordered_set<uint32_t> logged;
      std::lock_guard<std::mutex> lock(logged_mutex);
      if (logged.size() < 16 && logged.insert(guest.address).second) {
        REXLOG_WARN("NativeRenderer: unreadable buffer at {:08X}, {} bytes, endian {}",
                    guest.address, guest.size, guest.endian);
      }
      entry.content_hash = 0;
      return VK_NULL_HANDLE;
    }
    const uint64_t hash = XXH3_64bits(source, guest.size);
    if (hash != entry.content_hash) {
      // On failure the hash is still remembered, so it's only retried once
      // the data changes.
      entry.content_hash = hash;
      Upload(entry, guest, source, cmd, submission);
    }
  }
  if (!entry.buffer.buffer) {
    return VK_NULL_HANDLE;
  }
  entry.buffer.last_submission = submission;
  return entry.buffer.buffer;
}

}  // namespace native
