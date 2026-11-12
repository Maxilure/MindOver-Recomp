// =============================================================================
// native/d3d_draw_skip.h -- Native Only: the game's Xbox Direct3D draws skipped
// =============================================================================
//
// WHAT THIS IS (docs/findings/59 s.4)
//   The native renderer records every draw one level ABOVE Direct3D, in
//   Radical's PDDI back end (recorder.cpp: BeginPrims / EndPrims /
//   DrawPrimBuffer / xnPrimBuffer::Draw), reading the state D3D keeps in its
//   device's copy (shader constants, fetch constants, render states). D3D's
//   DRAW functions (the 0x82476000-0x82478000 block) do nothing our picture
//   needs: they FLUSH that copy into the GPU's command buffer (sub_8230E398 /
//   E730 / F4C0 / E5D8 / E458, driven by the device's dirty masks +0x00..+0x20)
//   and write the draw packet. Checked with the console's `who`: every write
//   into the copy comes from a Set... function or Radical's code, none from a
//   draw. So while the emulated GPU draws nothing (Native Only, our picture on
//   screen) those draws are pure cost: command-buffer building on the game's
//   main thread + parsing on the emulated GPU's thread.
//
//   While Active():
//     DrawIndexedVertices 0x82477B58 / DrawVertices 0x82477768 (meshes, from
//       xnPrimBuffer::Draw; results unused: `li r3,0` after) -> nothing.
//     BeginVertices 0x82476DA0 (r3 device, r4 primitive, r5 vertex count, r6
//       stride) -> only the vertex memory, from D3D's own allocator
//       sub_82308B60(device, dwords, 16) (the same memory the original hands
//       out; the game writes the vertices there and the recorder reads them).
//     BeginIndexedVertices 0x82477248 (r5 base vertex, r6 vertex count, r7
//       index count, r8 flags (bit 0x4 = 32-bit indices), r9 stride, r10 =
//       where the index pointer goes, [entry r1 + 0x54] = where the vertex
//       pointer goes; returns 0 / E_OUTOFMEMORY) -> the two allocations only.
//     EndVertices 0x82477238 (device +0x30 = the packet's end: commits the
//       packet the Begin wrote) -> skipped after a skipped Begin (paired by a
//       flag, so a switch between Begin and End can't commit a stale pointer).
//   The dirty masks stay set while skipping: the first real draw afterwards
//   (F9 to the emulated picture, dual mode) flushes everything, from a copy
//   that is up to date. Clears and resolves still go to D3D (few per frame).
//
// SWITCH: --d3d_draw_skip=false = every draw goes to D3D (comparisons).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(bool, d3d_draw_skip);

namespace d3d_draw_skip {

// From NativeRenderer::UpdateEmulatedDrawing: the emulated GPU draws nothing
// (Native Only, our picture on the main window, no dual window).
void SetEmulatedIdle(bool idle);
// Skipping now: emulated GPU idle and the switch on.
bool Active();

// Called by the recorder's D3D draw handlers instead of the original while
// Active(); true = handled (the original must not run).
bool BeginVertices(PPCContext& ctx, uint8_t* base);
bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base);
bool Draw(PPCContext& ctx, uint8_t* base);

// The handler on EndVertices (with the recorder's hooks).
void Install();
void Uninstall();

}  // namespace d3d_draw_skip
