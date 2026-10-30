// =============================================================================
// ui/marquee.h -- SCROLLING TEXT for the game's own menu texts: a line too
// long for its box scrolls through it instead of running out of the box
// =============================================================================
//
// WHAT THE PLAYER SEES (2026-10-29): a text that doesn't fit shows its start,
// waits a moment, scrolls to the left at an even speed until its END reaches
// the box's right edge, waits, FADES OUT, comes back at the start FADING IN,
// and goes round again. One way only (no back-and-forth). Letters are cut
// cleanly at the box's two edges. A text that fits is never touched.
//
// WHERE: only texts the port asks for (Fit / FitLines below), and only once the usual
// shrinking (each screen's own "smaller, down to N") isn't enough:
//   * the achievement / notice card on the game's message bar (its name line)
//   * the Achievements screen's cards (names and descriptions)
//   * the Players screen's device names
//   * (FitLines, up and down) the outfit chest's descriptions
// Every other text of the game draws exactly as before.
//
// HOW (native: the game draws it, both renderers show it, no ImGui):
// A Scrooby Text draws itself in its vtable slot 6 (Text@frontend, vtable
// 0x8200BE2C, slot 6 = sub_82371D68), which writes the glyphs as immediate
// geometry: xnContext::BeginPrims -> the game writes every glyph's quad
// (position x y z, colour, uv) into D3D's command buffer -> EndPrims. That
// function is wrapped: while one of OUR texts draws, the renderer hooks
// (native/recorder.cpp OnBeginPrims / OnEndPrims, which run in front of the
// originals whatever renderer shows the picture) hand each batch here
// BEFORE EndPrims closes it, and the vertices are changed in place, in the
// game's own data:
//   1. measured: the text's left / right x as the game laid it out (any
//      units: only proportions are used, so the page's scale, slide and
//      position need no knowing);
//   2. the visible window = the text's own box: width = text width x room /
//      width (the caller's measurements with the game font), placed by the
//      text's justification (byte +108: 0 left, 1 right, else centred) like
//      the game places a text in its box;
//   3. every vertex moved left by the scroll position, then CLAMPED to the
//      window with its texture u moved the same fraction (each glyph's own
//      du/dx from its triangle), so a glyph at the edge is cut, not squeezed;
//   4. every vertex colour's alpha multiplied by the fade.
// The emulated GPU reads the command buffer after EndPrims (it sees the
// change); the native recorder copies the vertices after it too.
//
// THREADS: the game's main thread (texts draw there; Fit is called from the
// screens' per-frame hooks on the same thread). A mutex anyway.
// =============================================================================
#pragma once

#include <cstdint>

namespace marquee {

// `text` (a Scrooby Text element) is `width` wide and has `room` (same units,
// at the scale it's drawn at): scrolls while width > room (+0.5 %), else
// draws as the game draws it. Call it whenever the text or its scale is set
// (every frame is fine: the scroll only restarts when the numbers change).
void Fit(uint32_t text, float width, float room);

// UP AND DOWN (2026-10-30): a text of several lines (a wrapped description)
// that has more than `max_lines` lines shows its first `max_lines`, waits,
// scrolls UP a line at a time's pace until its last line is in view, waits,
// fades out and comes back at the start fading in (the same round as the
// sideways one). The lines are measured from the glyphs the game draws (no
// font sizes, wrapping or page scale needed: only the order of the lines
// and their spacing), so the first frame of a new text draws as the game
// draws it. `content` = anything that changes with the words (a hash): a
// new value starts the round again. Glyphs are cut at the window's top and
// bottom edges like the sideways ones at the sides.
void FitLines(uint32_t text, int max_lines, uint64_t content);

// The text draws normally again (its screen is leaving / it's reused).
void Stop(uint32_t text);

// Seconds one pass takes (start held, scrolled to the end, end held), 0 when
// `width` fits `room`: for something shown for a while (the message bar's
// card stays at least this long, so a long line is read to its end).
double OnePassSeconds(float width, float room);

// From the renderer hooks (native/recorder.cpp), every immediate batch:
// right after BeginPrims set its stream up, and right before EndPrims
// closes it. Cost when none of our texts is drawing: one relaxed load.
void OnBeginPrims(uint8_t* base, uint32_t xn_context, uint32_t prim_type);
void OnEndPrims(uint8_t* base);

}  // namespace marquee
