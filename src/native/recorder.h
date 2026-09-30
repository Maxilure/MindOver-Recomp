// =============================================================================
// native/recorder.h -- writes down what the game draws, for the native renderer
// =============================================================================
//
// "Shadow mode" (docs/findings/08, section 6): the game keeps calling its own
// Xbox renderer backend (the xn* classes -> D3D -> emulated GPU), untouched.
// The recorder installs handlers (pddi/intercept.h) on a few of those calls;
// each handler lets the original run, then reads what it did from the game's
// objects and appends it to a native::Frame (frame.h):
//
//   xnContext::BeginPrims        immediate-mode geometry starts: remember the
//                                material, matrix, textures, render states
//   xnContext::BeginIndexedPrims the same with an index list (characters,
//                                skinned by the game's CPU every frame)
//   xnContext::EndPrims          the game has written the vertices (and
//                                indices): copy them
//   xnContext::DrawPrimBuffer    a static mesh is drawn with a material...
//   xnPrimBuffer::Draw           ...one call per material pass: record it
//   D3D SetRenderTarget /        the surfaces (render targets) drawn into;
//     SetDepthStencilSurface     hooked at D3D level because the game's
//   D3D Clear / Resolve          post-processing calls these directly
//   D3D BeginTiling              surfaces rendered in strips: their full size
//   xnContext::EndFrame          its resolve is the finished picture
//   xnContext::v95 / v96 / v97   two-sided stencil (only D3D keeps these)
//
// Calls the native renderer can't draw yet are counted and reported once
// each in the log ("not drawn natively yet: ..."), so the gaps are visible.
// Findings: docs/findings/09 (2D screens), docs/findings/10 (3D world),
// docs/findings/11 (characters).
//
// THREADS: the game draws from its main thread only (every call in the
// traces of findings/08), and the frame-end listener that calls TakeFrame
// runs there too, so the recorder needs no locks.
// =============================================================================

#pragma once

#include "frame.h"

namespace native::recorder {

// Installs / removes the handlers. Install once (the renderer does it).
void Install();
void Uninstall();

// Main thread, right after xnDisplay::SwapBuffers: hands over the frame the
// game just finished (swapped into `out`, whose old buffers get reused) and
// starts the next one, recorded only if `record_next` (recording costs a
// little CPU per draw, so it's off while the emulated picture is shown).
void TakeFrame(Frame& out, bool record_next);

}  // namespace native::recorder
