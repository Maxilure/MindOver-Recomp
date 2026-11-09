// =============================================================================
// ui/slide_fade.cpp -- see slide_fade.h
// =============================================================================
#include "slide_fade.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>

#include "../guest_memory.h"
#include "../native/widescreen.h"
#include "scrooby.h"

extern "C" REX_FUNC(__imp__sub_823711A8);  // Parent@frontend slot 7 (inherited colour)

REXCVAR_DEFINE_BOOL(menu_slide_fade, true, "CrashMoM",
                    "Widescreen: menus that slide in and out fade with the distance, gone by the 16:9 "
                    "screen's edge (else they stay visible in the side room until the slide ends)");

namespace slide_fade {
namespace {

// A big-endian float in guest memory.
float ReadFloat(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// The canvas is 640 units wide (4:3) and drawn in the middle of the 16:9
// screen, whose half width is 320 x (16:9) / (4:3) = 426.7 units. A panel
// whose centre has moved that far is half off the original screen: there
// it's fully see-through. Small moves stay solid: the pause panel's resting
// place (~72 units off centre), save list centring, main menu Extras'
// 60-unit swap (which fades on its own).
constexpr float kCanvasWidth = 640.0f;
constexpr float kStart = 120.0f;  // units: still solid (the pause panel rests ~72 off centre)
// Parent@frontend slot 7 (pages, groups, layers): store the inherited colour
// at +84, reset every child's to the sentinel.
constexpr uint32_t kSetInheritedColour = 0x823711A8;
constexpr uint32_t kSentinelAddress = 0x824EA190;

uint32_t Read32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

constexpr float kEnd = 320.0f * (16.0f / 9.0f) / (4.0f / 3.0f);   // units: gone

}  // namespace

Scope::Scope(PPCContext& ctx, uint8_t* base, uint32_t element) : ctx_(ctx), base_(base) {
  if (!element || !REXCVAR_GET(menu_slide_fade)) return;
  if (widescreen::CurrentAspect() <= widescreen::kGameAspect + 0.01f) return;  // 16:9: the original
  if (!(*GuestPtr(base, element + 110) & 0x40)) return;  // no transform in use
  // Only the Parent kind (pages, groups, layers): their slot 7 is the
  // "inherited colour" setter used below.
  if (Read32(base, Read32(base, element) + 28) != kSetInheritedColour) return;
  // Where the canvas centre (320, 240 = 0.5, 0.375 in canvas widths) lands:
  // x' = 0.5 m00 + 0.375 m10 + m30 (row vectors, matrix at +16). A scale
  // around the centre (the 0.85 x Options panel) moves it nowhere; a slide
  // moves it by its translation.
  const float x = 0.5f * ReadFloat(base, element + 16) + 0.375f * ReadFloat(base, element + 32) +
                  ReadFloat(base, element + 64);
  const float moved = std::fabs(x - 0.5f) * kCanvasWidth;
  if (!(moved > kStart)) return;  // also NaN
  const float t = std::clamp((moved - kStart) / (kEnd - kStart), 0.0f, 1.0f);
  const float keep = 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep: eases in and out
  // The colour the element draws with = +84 (its own +80 times its parent's,
  // worked out by the parent's draw); "not worked out yet" = the sentinel
  // [0x824EA190], then the own colour is the best guess.
  const uint32_t sentinel = Read32(base, kSentinelAddress);
  old_colour_ = Read32(base, element + 84);
  const uint32_t colour = old_colour_ == sentinel ? Read32(base, element + 80) : old_colour_;
  const uint32_t alpha = uint32_t(std::lround(float(colour >> 24) * keep));
  element_ = element;
  // Slot 7 stores +84 and sends every child back to "not worked out yet", so
  // this draw's children take their colour from the faded one.
  scrooby::Call(__imp__sub_823711A8, ctx, base, element, (colour & 0x00FFFFFFu) | alpha << 24);
}

Scope::~Scope() {
  // The cached colour as it was (children work theirs out again next frame).
  if (element_) scrooby::Call(__imp__sub_823711A8, ctx_, base_, element_, old_colour_);
}

}  // namespace slide_fade
