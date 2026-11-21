// =============================================================================
// guest_pool.h -- the port's own memory pool, inside the game's 32-bit memory
// =============================================================================
//
// WHY (docs/findings/71-memory.md s.4, step 2): the game's heaps have the Xbox
// 360's fixed sizes (the front end's 102 KB, the level's 91 MB...). When one
// is full, the port gives the game blocks from THIS pool instead: one shared
// pool in the guest's virtual memory (the 0x40000000 range, ~1 GB), grown in
// 64 MB pieces as needed. Its addresses are ordinary 32-bit guest addresses,
// so the recompiled game code uses them like any other memory.
//
// HOW: a best-fit allocator with neighbouring free blocks merged. All of its
// book-keeping lives in host memory (C++ maps), none of it in guest memory:
// a game bug writing past a block can't damage the pool's records. Blocks are
// 16-byte aligned (more when asked). Each block remembers its OWNER (the game
// heap object it stands in for), so a heap that goes away (the level's heap at
// a level change) takes its pool blocks with it, as its own memory would.
// Thread-safe (one lock).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace guest_pool {

// A block of `bytes` (aligned to `align`, a power of two; at least 16) for
// `owner`. 0 = no guest memory left to grow into.
uint32_t Alloc(uint32_t bytes, uint32_t align, uint32_t owner);

// Is `address` the start of one of the pool's blocks? (Cheap range test
// first: an address outside the pool's pieces costs a compare or two.)
bool Owns(uint32_t address);

// The owner of the block at `address`, 0 = not a pool block.
uint32_t OwnerOf(uint32_t address);

// Frees a block (false = not a pool block: nothing done).
bool Free(uint32_t address);

// Frees every block of `owner` (its heap is being destroyed); how many.
uint32_t FreeAllOf(uint32_t owner);

// Bytes in use for `owner` (0 = everyone), and the pool's size.
uint64_t UsedBy(uint32_t owner = 0);
uint64_t Reserved();

// One line for the debug console's `memory`.
std::string Summary();

}  // namespace guest_pool
