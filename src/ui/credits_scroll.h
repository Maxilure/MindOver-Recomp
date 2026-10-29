// =============================================================================
// ui/credits_scroll.h -- the credits scroll back and forth
// =============================================================================
//
// WHAT THE PLAYER GETS (2026-10-29): the credits (main menu -> Credits, and
// after the last boss) roll by themselves as before, and now:
//   * the mouse WHEEL scrolls them, both ways, like a list: a notch down =
//     on toward the end (the names go up), up = back; one line a notch,
//     glided smoothly over ~0.2 s;
//   * the VOICE LINES (people with a voice clip: the game grows the one in
//     the middle and A plays it) answer to the mouse anywhere on screen:
//     under the pointer one grows, a left click (or A / Space) plays it;
//   * Up / Down (D-pad, stick, arrow keys, W / S) set the speed in 10 steps
//     each way, like moving through a list (2026-10-30 playtest; the Xbox
//     had Up = faster): Down rolls them on toward the end, Up slows them,
//     stops, then rolls them BACKWARDS (up to the same speed): a controller
//     can read back too; that stops by itself at the start (the credits
//     begin on an empty screen, as on the Xbox). Back (B / Esc / right click) leaves,
//     as before.
//
// HOW THE GAME ROLLS THEM (CCreditScreenAction, vtable 0x8202248C; Update
// sub_820C6330, read 2026-10-29 with xexdis):
//   * 30 text elements ("01".."30" on GameStart_Credits.pag) at this +84,
//     8 bytes each: the element, the credit line it shows. The lines (272
//     bytes each) run from this +28 to +32: +0 a text-bank key, +4 the text
//     (UTF-16), flags +260 heading, +261 big heading, +262 a line with a
//     voice clip (A plays it: +340, +348 bit 0x80 while it plays), +268
//     "text by key".
//   * Each frame of the running state (+16 == 3), step = speed (+324, 0..1,
//     buttons 45 up / 16 down change it by 0.1) x dt x 500 / +332. The
//     phase (+344: 1 rolling, 2 / 3 the end) moves every element up by the
//     step (sub_820C6EB8); an element past the top is RECYCLED at the bottom
//     with the next line (+336 = the last line handed out; +328 = where the
//     next line goes, which moves with the text: +328 += step).
// So the text only knows how to go FORWARD: a recycled line is gone. Going
// back = CHECKPOINTS: every 2 lines of distance we keep the whole rolling
// state (the 30 elements' places, lines and visibility, +328..+348, the two
// end elements); back by d = restore the last checkpoint before the target,
// put each element's text back by the game's own rule (the code at
// 0x820C6F8C: text +4, or by key when +260 / +268), then roll forward to
// the target with the game's own phase functions, in small steps.
// We run the game's Update with dt = 0 (its own step = 0; f2 + 30 dt keeps
// its animation clock +124 the same) and the speed set to 0.5 to SEE its
// Up / Down (0.6 / 0.4 after it), then move the text ourselves by our own
// signed speed. A frame where the game waits for a voice clip doesn't move.
// =============================================================================
#pragma once

#include <cstdint>

namespace credits_scroll {
// (Hooks only: the Update wrapper in credits_scroll.cpp.)
}  // namespace credits_scroll
