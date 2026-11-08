// =============================================================================
// native/scan.h -- NATIVE GAPS: everything the native renderer can't draw yet
// =============================================================================
//
// WHY (2026-09-27, grown 2026-11-08): playtesting walks through the game's
// areas one by one to find what the native renderer doesn't draw yet. Gaps
// used to be one quiet INFO line each, only from some places: a draw the
// renderer dropped at the last step (a missing buffer, a stencil copy that
// never arrived, a render pass that couldn't open) left no trace at all, and
// a draw that reached D3D by a road the recorder doesn't listen to (a
// material or helper we have never met: the "missing shader" case) was
// invisible by construction. A 2026-11-08 session showed something plainly
// missing with ZERO gap lines in its log.
//
// WHAT: one place every gap goes through. Per gap ("what" = its text, which
// names the material / reason / game caller):
//   * the FIRST time in a session: a loud WARN banner
//       *** NATIVE GAP #3 (first time): skipped draw: particle: ... ***
//     plus an automatic photo (Tools -> Take a photo: both pictures of one
//     frame + its draw list, in user/photos/), at most one every 3 s (an area
//     with five new gaps gets one photo), and with --trace (tools/play.sh
//     --trace) a recording of the next frame's renderer calls;
//   * every time: a counter;
//   * <log name>-native-gaps.txt beside the session log: every gap met so
//     far, numbered, with its first time, its count and its photo's name;
//     rewritten when a new gap shows up and every 10 s while counts change.
// The numbers (#3) are the ones to go through one by one afterwards.
//
// --debug_native_gaps=false: back to the old quiet log line, no photo.
// Gaps are only met while our picture is recorded (native shown, native
// only, or dual mode): with the emulated picture alone nothing is checked.
//
// The callers: recorder (material classes, primitive types, odd vertex
// data, D3D draws outside the recorded roads), texture / buffer caches,
// render targets, and the renderer's draw loop (each reason it drops a draw).
// =============================================================================

#pragma once

#include <string_view>

namespace native::scan {

// A gap met (`what` = its text: the same text = the same gap). Any thread;
// cheap enough to call per draw (a short lock + a hash lookup).
void NewGap(std::string_view what);

}  // namespace native::scan
