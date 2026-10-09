// =============================================================================
// debug/guest_stack.h -- the GAME's call stack, read safely from anywhere
// =============================================================================
//
// The PowerPC calling convention keeps a "back chain": every function that
// needs a stack frame stores the caller's stack pointer at the bottom of its
// own frame (`stwu r1,-N(r1)`), and its return address (the LR register) 8
// bytes below the CALLER's stack pointer (`mflr r12; stw r12,-8(r1)`). So from
// the current r1:
//     chain = word at r1          (the caller's r1)
//     return address = word at chain - 8
//     ... and again from chain.
// The recompiled code keeps these frames in guest memory exactly like the
// Xbox did, so we can walk them. Caveats: a leaf function without a frame of
// its own doesn't appear (its caller's return address is in LR instead); and
// the r1 stored in the PPCContext can lag one frame behind inside a function
// (the compiler may keep it in a host register until the next call).
//
// Every read goes through process_vm_readv on our own process: an unmapped
// address returns an error instead of faulting, which makes all of this safe
// inside a signal handler (crash report, write watch).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace guest_stack {

// Where guest memory starts in our address space (Memory::virtual_membase()).
// Set once the runtime's memory exists; reads fail until then.
void SetMembase(uint8_t* base);

// Copies n bytes of guest memory (signal-safe). False = not readable.
bool SafeRead(uint32_t guest_address, void* out, size_t n);
// One big-endian word (signal-safe).
bool SafeRead32(uint32_t guest_address, uint32_t* value);

// Walks the back chain from `r1`; writes up to `max` return addresses that
// point into the game's code (0x820B0000-0x824D0000), innermost first.
// Returns how many it wrote. Signal-safe.
int Callers(uint32_t r1, uint32_t* out, int max);

// The recompiled game function a guest code address lies in (the nearest
// function start at or below it in the generated function table), 0 if none.
// "8217D0A0 = sub_8217CF00+0x1A0" makes a stack readable.
uint32_t FunctionContaining(uint32_t guest_address);

}  // namespace guest_stack
