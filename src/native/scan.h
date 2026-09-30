// =============================================================================
// native/scan.h -- automatic "area scan" for playtests
// =============================================================================
//
// WHY (2026-09-27): playtesting walks through the game's areas one by one to
// find what the native renderer doesn't draw yet in each. The log names
// every gap once ("not drawn natively yet: ...", "buffer / texture not
// supported yet: ..."), but to rebuild a missing effect we need the frame
// itself: every renderer call (pddi/trace.cpp) and a picture of where it is.
// Pressing F10 at the right moment in every area is tedious and easy to miss.
//
// WHAT: while a trace folder is set (tools/play.sh --trace), the FIRST time
// each gap shows up in a session, the next frame is recorded (a trace, like
// F10's) and a photo is taken (both renderers' pictures of one frame when
// ours is drawn). The log says so: "Scan: first time: <gap> -> recording".
// Without --trace nothing changes.
//
// The four places that log gaps (recorder, texture cache, buffer cache,
// render targets) call NewGap from their report-once functions.
// =============================================================================

#pragma once

#include <string_view>

namespace native::scan {

// A gap met for the first time this session (`what` = its log text). Any
// thread. Does nothing without --debug_pddi_trace_dir.
void NewGap(std::string_view what);

}  // namespace native::scan
