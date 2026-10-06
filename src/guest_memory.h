// =============================================================================
// guest_memory.h -- the host address of a guest address (EVERY platform)
// =============================================================================
//
// The game's memory is a 4 GB block mapped into our process; the recompiled
// code reaches guest address A through `base` (the pointer every recompiled
// function receives, = memory()->virtual_membase()). On Linux that's simply
// base + A. NOT everywhere: on Windows (and Apple-silicon Macs) the top
// range, 0xE0000000-0xFFFFFFFF (a window onto the 360's physical memory, used
// for the game's stacks and many heap objects), sits 0x1000 bytes further on
// in host memory: the host can't map that window at the 4 KB offset the 360
// has (Windows maps in 64 KB steps), so the runtime shifts it instead. The
// generated code knows (REX_PHYS_HOST_OFFSET in generated/default/
// crash_mom_pch.h); our own C++ must use the same rule, through GuestPtr().
//
// HOW FOUND (2026-10-06, the first Windows run): the game crashed at boot
// writing to address 0 inside the fight-tree loader (sub_820C23C0). Our
// branch-count lookup had read the tree's path with `base + r3`: on Linux
// "crashmom/Frontend.bfig", on Windows the bytes 4 KB before it (an old stack
// value), so it found no count and the loader wrote through a null array.
//
// RULE: never write `base + address` or `base[address]` for guest memory in
// our code; write GuestPtr(base, address) (and *GuestPtr(...) for one byte).
// The SDK's memory()->TranslateVirtual() does the same and stays fine.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace guest_memory {

// The host shift of guest address `address` (see the header).
constexpr uint32_t HostOffset(uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}

}  // namespace guest_memory

// The host pointer of guest address `address` (`base` = the guest memory base).
inline uint8_t* GuestPtr(uint8_t* base, uint32_t address) {
  return base + address + guest_memory::HostOffset(address);
}
inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t address) {
  return base + address + guest_memory::HostOffset(address);
}
