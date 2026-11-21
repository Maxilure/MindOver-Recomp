// guest_pool.cpp -- see guest_pool.h.
#include "guest_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace guest_pool {
namespace {

constexpr uint32_t kMinAlign = 16;
constexpr uint32_t kPiece = 64u << 20;  // grown in 64 MB pieces (more for one big block)

struct Used {
  uint32_t size;
  uint32_t owner;
};

struct Pool {
  std::mutex lock;
  std::vector<std::pair<uint32_t, uint32_t>> pieces;  // [start, end) of each piece of guest memory
  std::map<uint32_t, uint32_t> free_by_address;       // start -> size
  std::set<std::pair<uint32_t, uint32_t>> free_by_size;  // (size, start): best fit = lower_bound
  std::unordered_map<uint32_t, Used> used;             // start -> block
  std::unordered_map<uint32_t, uint64_t> used_by_owner;
  uint64_t used_total = 0, reserved = 0;
};
Pool g;

// The pieces' ranges again, readable WITHOUT the lock: every game `delete`
// asks "is this a pool block?", and nearly all answers are no. Pieces are
// only ever added (start << 32 | end), the count published last.
constexpr int kMaxPieces = 64;
std::array<std::atomic<uint64_t>, kMaxPieces> g_ranges{};
std::atomic<int> g_range_count{0};

bool InPiecesFast(uint32_t address) {
  const int n = g_range_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const uint64_t r = g_ranges[i].load(std::memory_order_relaxed);
    if (address >= uint32_t(r >> 32) && address < uint32_t(r)) return true;
  }
  return false;
}

uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void AddFree(uint32_t start, uint32_t size) {
  g.free_by_address[start] = size;
  g.free_by_size.insert({size, start});
}
void RemoveFree(std::map<uint32_t, uint32_t>::iterator it) {
  g.free_by_size.erase({it->second, it->first});
  g.free_by_address.erase(it);
}

// A free block back in, merged with its free neighbours.
void ReleaseRange(uint32_t start, uint32_t size) {
  auto next = g.free_by_address.lower_bound(start);
  if (next != g.free_by_address.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == start) {  // touches the block before
      start = prev->first;
      size += prev->second;
      RemoveFree(prev);
    }
  }
  next = g.free_by_address.lower_bound(start + size);
  if (next != g.free_by_address.end() && next->first == start + size) {  // and the one after
    size += next->second;
    RemoveFree(next);
  }
  AddFree(start, size);
}

// A new piece of guest virtual memory (64 KB pages, committed = zero-filled
// on first touch, read + write), at least `bytes`.
bool Grow(uint32_t bytes) {
  const uint32_t size = std::max(kPiece, RoundUp(bytes, 64 * 1024));
  auto* heap = rex::system::kernel_memory()->LookupHeap(0x40000000);
  uint32_t address = 0;
  if (!heap || !heap->Alloc(size, 64 * 1024,
                            rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
    REXLOG_ERROR("Memory: the port's pool could not grow by {} MB (guest virtual memory full)", size >> 20);
    return false;
  }
  const int n = g_range_count.load(std::memory_order_relaxed);
  if (n >= kMaxPieces) {
    REXLOG_ERROR("Memory: the port's pool has {} pieces already: not growing", n);
    heap->Release(address);
    return false;
  }
  g_ranges[n].store(uint64_t(address) << 32 | (address + size), std::memory_order_relaxed);
  g_range_count.store(n + 1, std::memory_order_release);
  g.pieces.push_back({address, address + size});
  std::sort(g.pieces.begin(), g.pieces.end());
  g.reserved += size;
  REXLOG_INFO("Memory: the port's pool grew by {} MB at {:08X} ({} MB in all)", size >> 20, address,
              g.reserved >> 20);
  ReleaseRange(address, size);
  return true;
}

bool InPieces(uint32_t address) {
  for (const auto& p : g.pieces) {
    if (address >= p.first && address < p.second) return true;
  }
  return false;
}

}  // namespace

uint32_t Alloc(uint32_t bytes, uint32_t align, uint32_t owner) {
  align = std::max(kMinAlign, align);
  if (align & (align - 1)) align = kMinAlign;  // not a power of two: the default
  const uint32_t size = RoundUp(std::max<uint32_t>(bytes, 1), kMinAlign);
  if (size < bytes) return 0;  // wrapped (a 4 GB request)
  std::lock_guard<std::mutex> l(g.lock);
  for (int attempt = 0; attempt < 2; ++attempt) {
    // Best fit: the smallest free block that holds the size once aligned.
    for (auto it = g.free_by_size.lower_bound({size, 0}); it != g.free_by_size.end(); ++it) {
      const uint32_t block = it->second, block_size = it->first;
      const uint32_t start = RoundUp(block, align);
      if (uint64_t(start) + size > uint64_t(block) + block_size) continue;
      RemoveFree(g.free_by_address.find(block));
      if (start > block) AddFree(block, start - block);  // the alignment gap stays free
      uint32_t taken = size;
      const uint32_t tail = block + block_size - (start + size);
      if (tail >= kMinAlign) {
        AddFree(start + size, tail);
      } else {
        taken += tail;  // a sliver: part of the block
      }
      g.used[start] = {taken, owner};
      g.used_by_owner[owner] += taken;
      g.used_total += taken;
      return start;
    }
    if (attempt == 0 && !Grow(size + align)) return 0;
  }
  return 0;
}

bool Owns(uint32_t address) {
  if (!InPiecesFast(address)) return false;
  std::lock_guard<std::mutex> l(g.lock);
  return InPieces(address) && g.used.count(address);
}

uint32_t OwnerOf(uint32_t address) {
  if (!InPiecesFast(address)) return 0;
  std::lock_guard<std::mutex> l(g.lock);
  if (!InPieces(address)) return 0;
  const auto it = g.used.find(address);
  return it == g.used.end() ? 0 : it->second.owner;
}

bool Free(uint32_t address) {
  if (!InPiecesFast(address)) return false;
  std::lock_guard<std::mutex> l(g.lock);
  if (!InPieces(address)) return false;
  const auto it = g.used.find(address);
  if (it == g.used.end()) return false;
  const Used u = it->second;
  g.used.erase(it);
  g.used_by_owner[u.owner] -= u.size;
  g.used_total -= u.size;
  ReleaseRange(address, u.size);
  return true;
}

uint32_t FreeAllOf(uint32_t owner) {
  std::lock_guard<std::mutex> l(g.lock);
  std::vector<std::pair<uint32_t, uint32_t>> blocks;
  for (const auto& [start, u] : g.used) {
    if (u.owner == owner) blocks.push_back({start, u.size});
  }
  for (const auto& [start, size] : blocks) {
    g.used.erase(start);
    g.used_total -= size;
    ReleaseRange(start, size);
  }
  g.used_by_owner.erase(owner);
  return uint32_t(blocks.size());
}

uint64_t UsedBy(uint32_t owner) {
  std::lock_guard<std::mutex> l(g.lock);
  if (!owner) return g.used_total;
  const auto it = g.used_by_owner.find(owner);
  return it == g.used_by_owner.end() ? 0 : it->second;
}

uint64_t Reserved() {
  std::lock_guard<std::mutex> l(g.lock);
  return g.reserved;
}

std::string Summary() {
  std::lock_guard<std::mutex> l(g.lock);
  std::string s = fmt::format("port pool: {:.1f} MB in {} blocks, {} MB reserved in {} pieces", g.used_total / 1048576.0,
                              g.used.size(), g.reserved >> 20, g.pieces.size());
  for (const auto& [owner, bytes] : g.used_by_owner) {
    if (bytes) s += fmt::format(" | heap object {:08X}: {:.1f} MB", owner, bytes / 1048576.0);
  }
  return s + "\n";
}

}  // namespace guest_pool
