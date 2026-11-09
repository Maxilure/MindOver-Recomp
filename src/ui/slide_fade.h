// =============================================================================
// ui/slide_fade.h -- sliding menus fade out on a picture wider than 16:9
// =============================================================================
//
// WHAT: the game's menus (pause menu, its Options, the save list...) slide in
// from one side and out to the other. On the original 16:9 screen the panel
// leaves the screen before the slide ends. With widescreen (native/
// widescreen.h) the picture has extra room at the sides, so the panel was
// still in view there when the slide ended and the page vanished at once.
// Now a panel moved sideways fades with the distance: fully see-through by
// the time its centre reaches the 16:9 screen's edge. The slide itself (path,
// timing, sounds) stays the game's; a 16:9 picture is untouched.
//
// HOW (findings/56 "Sliding menus"): a slide = the screen's code moving a
// whole page or layer through its TRANSFORM every frame (pause menu:
// CPauseScreenAction step sub_820DA120 -> sub_82370E98 (element, x, 0) on the
// glass layer +32 and the menu page +20, x from the interpolator at +52).
// Element +16 = its 4 x 4 matrix (row vectors: x translation at +64, in
// canvas widths: 1.0 = 640 units; sub_824239E0 builds it), +110 bit 0x40 = the matrix is in use. The
// Parent draw (sub_82371270, pages / groups / layers; wrapped in
// ui/hud_hide.cpp) asks Scope: if the element is moved sideways, the colour
// it draws with is faded for the draw and put back right after. That colour
// is CACHED at +84 (own +80 times the parent's, worked out by the parent's
// draw while +84 holds the sentinel [0x824EA190]); a child keeps its cached
// one until reset. Parent slot 7 (sub_823711A8) stores +84 and resets every
// child: called with the faded colour before the draw, the old one after.
// (Changing +80 at draw time came too late: the element's +84 was already
// worked out, so a slid-out panel stayed faintly visible.)
//
// Flag menu_slide_fade (default on).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace slide_fade {

// Around one Parent draw: fades `element` by its sideways offset (wide
// picture only); its own alpha is back when the scope ends.
class Scope {
 public:
  Scope(PPCContext& ctx, uint8_t* base, uint32_t element);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t element_ = 0;  // 0 = nothing changed
  uint32_t old_colour_ = 0;  // its +84 before
};

}  // namespace slide_fade
