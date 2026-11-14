// =============================================================================
// ui/menu_frame_wide.h -- the main menu's frame and backdrop on a picture
// wider than 16:9
// =============================================================================
//
// WHAT THE ORIGINAL DOES (findings/56 s.4b): the 360 game supports 4:3 and
// 16:9 TVs with ONE flag (xnDisplay +1656 bit 0x40, from the console's video
// mode). The main menu's frame page FE_NV_Frame.pag holds TWO complete frames,
// laid out by hand: layer NVFrameBorder_Fullscreen (4:3) and layer
// NVFrameBorder_Widescreen (16:9: the same pictures moved out to the 16:9
// edges, the top and bottom bars stretched). CNVBackgroundAction (vtable
// 0x82024374) shows one of them every frame (update sub_820D7B88). The cyan
// backdrop behind it is drawn in code by the same class: a gradient as wide
// as sub_820C4D60 says (640 or 853 canvas units) plus five turning rings.
//
// WHAT WE DO (only when the picture is wider than 16:9): the game's own
// method with a bigger number. Each side of the canvas is d = 240 x (A - 16/9)
// units further out (A = the picture's aspect; the canvas is 480 units tall):
// - frame (16:9 layer): left pieces x - d; right pieces, cogs and the light
//   x + d; the top and bottom bars: each is drawn as tiles, the END tiles
//   move -d / +d with the corners, only the plain MIDDLE tile is stretched
//   (a whole-picture stretch widened the ends: thin stripes at the joints);
// - gradient: sub_820C4D60 answers 480 x A, only to the backdrop's two calls
//   (the players' markers and the canvas <-> screen helpers keep 853: game
//   logic stays 16:9, findings/56 s.2);
// - rings: the four outer rings are drawn again every 440 units sideways
//   (their own spacing) until the picture's edge.
// - the N. V. door (GameStart_NV_Door.pag, opening from the middle when the
//   title gives way to the main menu): the plain bodies mapped wider; the
//   edge strips with the teeth and the "N" NOT stretched: cut, and extended
//   with copies of their own slices, alternately mirrored (seamless joins),
//   drawn by drawing the strip again (several passes).
// At 16:9 (or with the flag menu_frame_wide off) everything is the original.
//
// Overrides (no Install needed): sub_820D7B88, sub_820C4D60, sub_820D8408,
// sub_820D8048. Called from ui/voodoo_card.cpp (the Sprite draw's wrapper)
// and native/recorder.cpp (the immediate-geometry hooks, any renderer).
// =============================================================================
#pragma once

#include <cstdint>

namespace menu_frame_wide {

// Around a Scrooby picture's draw (Sprite@frontend slot 6, sub_82374098;
// game's main thread): notes whether it is one of the frame's bars or the
// door's pictures. Returns how many times to draw it (1 = once, as usual;
// more for a door strip being extended), the caller calls SetPass(p) before
// each draw (the same registers each time), EndSpriteDraw after the last.
int BeginSpriteDraw(uint8_t* base, uint32_t element);
void SetPass(int pass);
void EndSpriteDraw();
// Immediate geometry (xnContext BeginPrims after the original / EndPrims
// before it): such a picture's tile is measured and moved in the game's own
// data.
void OnBeginPrims(uint8_t* base, uint32_t xn_context);
void OnEndPrims(uint8_t* base);

}  // namespace menu_frame_wide
