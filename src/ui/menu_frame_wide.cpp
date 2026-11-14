// =============================================================================
// ui/menu_frame_wide.cpp -- see menu_frame_wide.h
// =============================================================================
#include "menu_frame_wide.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../guest_memory.h"
#include "../native/guest.h"
#include "../native/widescreen.h"
#include "scrooby.h"

extern "C" REX_FUNC(__imp__sub_820D7B88);  // CNVBackgroundAction slot 5: per-frame update (picks the layer)
extern "C" REX_FUNC(__imp__sub_820C4D60);  // visible canvas width: 853 (16:9 TV) or 640 (4:3)
extern "C" REX_FUNC(__imp__sub_820D8408);  // the backdrop's five rings
extern "C" REX_FUNC(__imp__sub_820D8048);  // the backdrop's quad helper (action, corners, uvs, colours, texture)
extern "C" REX_FUNC(__imp__sub_82371A90);  // Parent: find a picture by name (searches its children)

REXCVAR_DEFINE_BOOL(menu_frame_wide, true, "CrashMoM",
                    "Widescreen: the main menu's frame and cyan backdrop reach the edges of a picture "
                    "wider than 16:9 (else they stop at the 16:9 edges, black beyond)");

namespace menu_frame_wide {
namespace {

// CNVBackgroundAction: +28 = layer NVFrameBorder_Widescreen (set-up
// sub_820D7960 found it by name; +24 = the 4:3 layer, never touched here).
constexpr uint32_t kWideLayer = 28;
// Return addresses of the backdrop gradient's two width questions
// (sub_820D8208 asks sub_820C4D60 twice: left edge, right edge).
constexpr uint32_t kGradientAsk1 = 0x820D823C;
constexpr uint32_t kGradientAsk2 = 0x820D8260;
// The rings' own sideways spacing (canvas x 100 and 540) and size (a quad of
// half size 128 turning: corners up to 128 x sqrt 2 out).
constexpr float kRingSpacing = 440.0f;
constexpr float kRingReach = 182.0f;
constexpr float kCanvasCentreX = 320.0f;

float ReadFloat(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void WriteFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint8_t* p = GuestPtr(base, address);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(bits >> (24 - 8 * i));
}
uint32_t Read32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// How far each side of the canvas moves out this frame (canvas units, 0 at
// 16:9 or narrower, or with the flag off).
float SideMove() {
  if (!REXCVAR_GET(menu_frame_wide)) return 0.0f;
  const float a = widescreen::CurrentAspect();
  if (!(a > widescreen::kGameAspect + 0.001f)) return 0.0f;  // also NaN
  return 240.0f * (a - widescreen::kGameAspect);
}

// The 16:9 layer's pieces: which way each moves. Names from FE_NV_Frame.pag
// (tools/scrooby_dump.py); the 4:3 layer uses the same names, which is why
// they are looked up INSIDE the 16:9 layer.
enum class Move { kLeft, kRight, kWiden };
struct Piece {
  std::string_view name;
  Move move;
};
constexpr std::array<Piece, 11> kPieces = {{
    {"NVFrameLeft", Move::kLeft},
    {"NVFrameTopLeft", Move::kLeft},
    {"NVFrameBottomLeft", Move::kLeft},
    {"NVFrameRight", Move::kRight},
    {"NVFrameTopRight", Move::kRight},
    {"NVFrameBottomRight", Move::kRight},
    {"cog3", Move::kRight},
    {"cog4", Move::kRight},
    {"NV_light2", Move::kRight},
    {"NVFrameTop", Move::kWiden},
    {"NVFrameBottom", Move::kWiden},
}};

// The pieces found in the current layer + their ORIGINAL x (+92, raw: units
// x the constant at 0x8204701C, like the game stores it). Positions are
// written from these every frame, so a window that changes shape just works.
struct Found {
  uint32_t element = 0;
  float x = 0.0f;
};
struct State {
  uint32_t layer = 0;  // the 16:9 layer these belong to (0 = none yet)
  std::array<Found, kPieces.size()> pieces{};
};
State g;

// Inside the rings' draw (sub_820D8408): their quad helper's calls get copies.
bool g_in_rings = false;

void FindPieces(PPCContext& ctx, uint8_t* base, uint32_t layer) {
  g = State{};
  g.layer = layer;
  int found = 0;
  for (size_t i = 0; i < kPieces.size(); ++i) {
    const uint32_t e = scrooby::Call(__imp__sub_82371A90, ctx, base, layer,
                                     scrooby::GuestAscii(kPieces[i].name));
    if (!e) continue;
    g.pieces[i] = {e, ReadFloat(base, e + 92)};
    ++found;
  }
  REXLOG_INFO("Menu frame wide: 16:9 frame layer {:08X}, {} of {} pieces found", layer, found,
              kPieces.size());
}

// Every frame, BEFORE the original's update: the moved positions (the
// original then turns the cogs around their moved centres).
void PlacePieces(uint8_t* base, float d) {
  float unit;
  {
    const uint32_t bits = Read32(base, 0x8204701C);
    std::memcpy(&unit, &bits, 4);
  }
  for (size_t i = 0; i < kPieces.size(); ++i) {
    const Found& f = g.pieces[i];
    if (!f.element) continue;
    switch (kPieces[i].move) {
      case Move::kLeft: WriteFloat(base, f.element + 92, f.x - d * unit); break;
      case Move::kRight: WriteFloat(base, f.element + 92, f.x + d * unit); break;
      case Move::kWiden: break;  // stays in place: its tiles are edited as it draws (below)
    }
  }
}

// PICTURES EDITED AS THEY DRAW: the frame's two bars and the N. V. door's four
// pictures. A Scrooby picture wider than its texture is drawn as TILES, one
// immediate draw each (read from a photo's draw list: FE_NV_Frame_top =
// three, the door's edge strips five). While one of these pictures draws (its
// Sprite draw wrapped in ui/voodoo_card.cpp, the one wrapper of
// sub_82374098), each tile's vertices are moved in the game's own data right
// before EndPrims, like the scrolling texts (ui/marquee.h). Everything is
// worked out in CANVAS x: the menus' projection puts the picture's whole width
// (480 x A canvas units, centred on x 320) into clip -1..1, so a vertex's
// canvas x = 320 + 240 A x clip x, clip x = x c4.x + y c5.x + c7.x (the MVP's
// first column, the page's own scale and slide included).
//
// THE BARS (top and bottom of the frame). Each tile holds: the first the
// bar's LEFT END (a few see-through columns + the joint's bevel), the last its
// RIGHT END, the middle one only the plain band. Stretching the whole picture
// (the first version: a sideways scale on its transform) widened the ends too,
// about 2.4 times at 32:9: the see-through end columns showed the cyan
// backdrop beside the corner pieces and the bottom bar's bevel no longer met
// the corner's (thin stripes / small steps at all four joints). Now the end
// tiles move RIGIDLY with their corner pieces (-d / +d: the joints look
// exactly like the original's) and only the middle tile is stretched.
//
// THE DOOR (GameStart_NV_Door.pag: it opens from the middle when the title
// gives way to the main menu, and closes when leaving it). Two plain door
// BODIES (NV_door.tga stretched by the game itself over 860 x 270 / 860 x 240
// units, x -110..750 = just the 16:9 screen) and two edge STRIPS with
// INTERLOCKING TEETH and the "N" handle in the middle (tiles x -110, 144,
// 398, 652, 714, 744; the same on both halves).
// - Bodies: mapped wider (plain metal, already stretched 13 times by the
//   game: no visible difference): the part outside x 144..398 so the 16:9
//   screen's edges (x -106.7 / 746.7) land on the wide picture's.
// - Strips: NOT stretched (a first version mapped them like the bodies: the
//   outer teeth came out wider). They are cut and extended with COPIES of
//   their own slices, drawn by drawing the strip again (more passes of its
//   Sprite draw, ui/voodoo_card.cpp), each tile CROPPED by its texture
//   coordinates and moved or MIRRORED (no tile is ever scaled):
//     left : cut at x -94 (just inside the rounded left end), copies of the
//            slice -94..144 going left, the left end piece moved out past them;
//     right: cut at x 652, copies of the slice 398..652 going right, the end
//            tiles (652..744, the angled end) moved out past them.
//   Copies ALTERNATE mirrored / normal (like reflections between two facing
//   mirrors): every join meets its own reflection, so the outline runs on
//   without a step. Plain repeating left steps of 2-4 pixels (720p) at the
//   joins: the hand-drawn teeth are not at one exact height (measured on a
//   photo of the door, both outlines per column). An even number of copies
//   puts the end piece back against the slice's own edge. Both strips get the
//   same cuts and copies, so their teeth still interlock. The N tile and
//   everything in the 16:9 screen are drawn as the game draws them.
//
// Which bar tile is which: its x range against the bar's ends as measured in
// the previous frame (the game writes the same numbers every frame); on the
// very first frame, against the picture's own box (+92 x, +88 width: the
// left end tile starts at the box's x, the right end tile ends at or past the
// box's right edge: these pictures draw a few units wider than their box).
enum class Kind { kBar, kDoorBody, kDoorStrip };
struct Edited {
  uint32_t element = 0;
  Kind kind = Kind::kBar;
  bool known = false;
  float lo = 0, hi = 0;          // the picture's canvas x range last frame (as the game drew it)
  float cur_lo = 0, cur_hi = 0;  // this frame's, being measured
  int tiles = 0;
  float box_x = 0, box_w = 0;  // the picture's own box (canvas units), for the first frame
};
// [0] top bar, [1] bottom bar, [2..5] the door's pictures.
std::array<Edited, 6> g_edited;
constexpr std::array<std::string_view, 4> kDoorPictures = {"DoorTopQuadSprite", "DoorTop",
                                                           "DoorBottomQuadSprite", "DoorBottom"};
uint32_t g_door_page = 0;  // GameStart_NV_Door.pag as last found
int g_drawing = -1;        // index into g_edited of the picture drawing now, -1 = none
float g_draw_d = 0;        // its side move this frame (canvas units)
// The N tile of the door's strips, kept as drawn (canvas x).
constexpr float kDoorKeepLo = 144.0f, kDoorKeepHi = 398.0f;  // the strips' tile edges (measured)
// The strips' cuts and slices (canvas x; tiles measured in the draw list, the
// rounded left end = the first ~16 texels of the left tile, kit picture).
constexpr float kStripLeftCut = -94.0f, kStripLeftWidth = 144.0f - kStripLeftCut;  // slice -94..144
constexpr float kStripRightCut = 652.0f, kStripRightWidth = 652.0f - 398.0f;       // slice 398..652
// The strip being drawn: this pass and the plan (copies each side, even).
int g_pass = 0, g_left_copies = 0, g_right_copies = 0;
// The 16:9 screen's edges in canvas x (853.3 units wide, centred on 320).
constexpr float kScreenLeft = 320.0f - 426.67f, kScreenRight = 320.0f + 426.67f;
// The open batch of that picture (between BeginPrims and EndPrims).
struct Batch {
  bool open = false;
  uint32_t vertices = 0, stride = 0, count = 0;
  float c4x = 0, c5x = 0, c7x = 0;  // clip x = x c4.x + y c5.x + c7.x
  int uv = -1;                      // texture coordinates' offset in a vertex (-1 = none)
} g_batch;
int g_logged = 0;  // first tiles logged (a few lines once)

float LoadF(uint8_t* base, uint32_t a) {
  const uint32_t v = native::guest::Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The door's pictures (from the frame's update, every frame while the side
// move is on: the page is looked up by name, its pictures again only when the
// page object changed).
void FindDoor(PPCContext& ctx, uint8_t* base) {
  const uint32_t page = scrooby::FindPage(ctx, base, "GameStart_NV_Door");
  if (page == g_door_page) return;
  g_door_page = page;
  int found = 0;
  for (size_t i = 0; i < kDoorPictures.size(); ++i) {
    Edited& e = g_edited[2 + i];
    e = Edited{};
    e.kind = i % 2 == 0 ? Kind::kDoorBody : Kind::kDoorStrip;
    e.element = page ? scrooby::FindPicture(ctx, base, page, kDoorPictures[i]) : 0;
    found += e.element != 0;
  }
  if (page) REXLOG_INFO("Menu frame wide: door page {:08X}, {} of 4 pictures found", page, found);
}


// One tile of a door strip in pass g_pass (see THE DOOR above): which part of
// the strip this pass draws (canvas x a..b of the original), and where: x' =
// x + shift, or mirrored x' = mirror - x. The tile is cropped to a..b (its
// texture coordinates cut at the same place), moved, or hidden (zero wide).
void StripTile(uint8_t* base, const Batch& b, float lo, float hi, float half) {
  const int nl = g_left_copies, nr = g_right_copies;
  const float wl = kStripLeftWidth, wr = kStripRightWidth;
  const float pl = kStripLeftCut, pr = kStripRightCut;
  float a = 0, z = 0, shift = 0, mirror = 0;
  bool mirrored = false;
  const int p = g_pass;
  if (p == 0) {  // the strip as drawn, cut at both ends
    a = pl, z = pr;
  } else if (p <= nl) {  // left copy k, going left: odd = mirrored
    const int k = p;
    a = pl, z = pl + wl;
    mirrored = k & 1;
    if (mirrored) mirror = 2.0f * pl - float(k - 1) * wl;
    else shift = -float(k) * wl;
  } else if (p == nl + 1) {  // the left end piece, past the copies
    a = -1e9f, z = pl, shift = -float(nl) * wl;
  } else if (p <= nl + 1 + nr) {  // right copy k, going right: odd = mirrored
    const int k = p - (nl + 1);
    a = pr - wr, z = pr;
    mirrored = k & 1;
    if (mirrored) mirror = 2.0f * pr + float(k - 1) * wr;
    else shift = float(k) * wr;
  } else {  // the right end tiles, past the copies
    a = pr, z = 1e9f, shift = float(nr) * wr;
  }
  auto at = [&](uint32_t v) { return b.vertices + v * b.stride; };
  auto canvas_x = [&](uint32_t v) {
    return kCanvasCentreX + half * (LoadF(base, at(v)) * b.c4x + LoadF(base, at(v) + 4) * b.c5x + b.c7x);
  };
  const float from = std::max(lo, a), to = std::min(hi, z);
  const bool hidden = !(to - from > 0.01f) || !(hi - lo > 0.01f) || b.uv < 0;
  // The texture's u at the tile's two edges (vertices on the left / right edge).
  float u_lo = 0, u_hi = 0;
  if (!hidden) {
    int n_lo = 0, n_hi = 0;
    for (uint32_t v = 0; v < b.count; ++v) {
      const float u = LoadF(base, at(v) + uint32_t(b.uv));
      if (canvas_x(v) - lo < hi - canvas_x(v)) u_lo += u, ++n_lo;
      else u_hi += u, ++n_hi;
    }
    if (!n_lo || !n_hi) return;  // not a quad: leave it
    u_lo /= float(n_lo), u_hi /= float(n_hi);
  }
  for (uint32_t v = 0; v < b.count; ++v) {
    const float x = canvas_x(v);
    float nx;
    if (hidden) {
      nx = lo;  // zero wide: nothing drawn
    } else {
      const float c = x - lo < hi - x ? from : to;  // this vertex's edge after the crop
      const float u = u_lo + (c - lo) / (hi - lo) * (u_hi - u_lo);
      WriteFloat(base, at(v) + uint32_t(b.uv), u);
      nx = mirrored ? mirror - c : c + shift;
    }
    WriteFloat(base, at(v), LoadF(base, at(v)) + (nx - x) / (half * b.c4x));
  }
  if (g_logged < 40 && !hidden) {
    ++g_logged;
    REXLOG_INFO("Menu frame wide: strip pass {} tile {:.1f}..{:.1f} -> part {:.1f}..{:.1f} {} {:.1f}", p, lo, hi,
                from, to, mirrored ? "mirrored at" : "moved by", mirrored ? mirror : shift);
  }
}

}  // namespace

int BeginSpriteDraw(uint8_t* base, uint32_t element) {
  g_drawing = -1;
  g_pass = 0;
  if (!element) return 1;
  // The bars are known once the frame's layer is (FindPieces): put them in.
  int bar = 0;
  for (size_t i = 0; i < kPieces.size() && g.layer; ++i) {
    if (kPieces[i].move != Move::kWiden) continue;
    if (g_edited[size_t(bar)].element != g.pieces[i].element) {
      g_edited[size_t(bar)] = Edited{};
      g_edited[size_t(bar)].element = g.pieces[i].element;
    }
    ++bar;
  }
  for (size_t i = 0; i < g_edited.size(); ++i) {
    if (g_edited[i].element == element) {
      g_drawing = int(i);
      break;
    }
  }
  if (g_drawing < 0) return 1;
  g_draw_d = SideMove();
  Edited& e = g_edited[size_t(g_drawing)];
  e.cur_lo = 1e30f, e.cur_hi = -1e30f, e.tiles = 0;
  if (!e.known) {  // the box, for the first frame (units x the constant at 0x8204701C)
    float unit;
    {
      const uint32_t bits = Read32(base, 0x8204701C);
      std::memcpy(&unit, &bits, 4);
    }
    e.box_x = unit > 0 ? ReadFloat(base, element + 92) / unit : 0;
    const uint8_t* w = GuestPtr(base, element + 88);
    e.box_w = float(int16_t(uint16_t(w[0]) << 8 | w[1]));
  }
  if (e.kind != Kind::kDoorStrip || !(g_draw_d > 0.0f)) return 1;
  // Copies each side: enough for the ends to reach the wide picture's edges
  // (left end at -110, right end at 744; the 16:9 edges -106.7 / 746.7),
  // rounded up to an even number (the end piece then meets the slice's own
  // edge, not its mirror image).
  auto even = [](float n) { const int k = int(std::ceil(n)); return k + (k & 1); };
  g_left_copies = std::max(2, even(g_draw_d / kStripLeftWidth));
  g_right_copies = std::max(2, even((g_draw_d + 2.7f) / kStripRightWidth));
  // Passes: the strip cut at both ends, the left copies, the left end piece,
  // the right copies, the right end piece.
  return 1 + g_left_copies + 1 + g_right_copies + 1;
}

void SetPass(int pass) { g_pass = pass; }

void EndSpriteDraw() {
  if (g_drawing < 0) return;
  Edited& e = g_edited[size_t(g_drawing)];
  if (e.tiles > 0) e.known = true, e.lo = e.cur_lo, e.hi = e.cur_hi;
  g_drawing = -1;
  g_batch = {};
}

void OnBeginPrims(uint8_t* base, uint32_t xn_context) {
  if (g_drawing < 0) return;
  namespace guest = native::guest;
  g_batch = {};
  const uint32_t stream = guest::Load32(base, xn_context + guest::context::kPrimStream);
  const uint32_t device = guest::Load32(base, xn_context + guest::context::kDevice);
  if (!stream || !device) return;
  const uint32_t vertices = guest::Load32(base, stream + guest::stream::kPosition);
  const uint32_t stride = guest::Load32(base, stream + guest::stream::kStride);
  const uint32_t count = guest::Load32(base, stream + guest::stream::kCount);
  if (!vertices || stride < 12 || stride > 252 || !count || count > 64) return;
  g_batch.open = true;
  g_batch.vertices = vertices, g_batch.stride = stride, g_batch.count = count;
  g_batch.c4x = LoadF(base, device + guest::device::VsConstant(4));
  g_batch.c5x = LoadF(base, device + guest::device::VsConstant(5));
  g_batch.c7x = LoadF(base, device + guest::device::VsConstant(7));
  const uint32_t uv = guest::Load32(base, stream + guest::stream::kUv);
  g_batch.uv = uv > vertices && uv - vertices < stride ? int(uv - vertices) : -1;
}

void OnEndPrims(uint8_t* base) {
  if (g_drawing < 0 || !g_batch.open) return;
  const Batch b = g_batch;
  g_batch.open = false;
  if (!(std::fabs(b.c4x) > 1e-7f)) return;
  Edited& e = g_edited[size_t(g_drawing)];
  const float half = 240.0f * widescreen::CurrentAspect();  // canvas units per clip unit
  auto at = [&](uint32_t v) { return b.vertices + v * b.stride; };  // x, y = first floats
  auto canvas_x = [&](uint32_t v) {
    return kCanvasCentreX + half * (LoadF(base, at(v)) * b.c4x + LoadF(base, at(v) + 4) * b.c5x + b.c7x);
  };
  float lo = 1e30f, hi = -1e30f;
  for (uint32_t v = 0; v < b.count; ++v) {
    const float x = canvas_x(v);
    lo = std::min(lo, x), hi = std::max(hi, x);
  }
  if (!(hi >= lo)) return;  // NaN
  if (g_pass == 0) {
    e.cur_lo = std::min(e.cur_lo, lo), e.cur_hi = std::max(e.cur_hi, hi);
    ++e.tiles;
  }
  const float d = g_draw_d;
  if (!(d > 0.0f)) return;
  if (e.kind == Kind::kDoorStrip) {
    StripTile(base, b, lo, hi, half);
    return;
  }
  // The bar's ends: last frame's, or on the very first frame its box.
  const bool measured = e.known && e.hi > e.lo;
  if (e.kind == Kind::kBar && !measured && !(e.box_w > 1.0f)) return;
  // Where each vertex goes (canvas x -> canvas x).
  const char* what = "";
  auto moved = [&](float x) -> float {
    if (e.kind == Kind::kBar) {
      bool left, right;
      if (measured) {
        // "This is the bar's end": within 0.2% of its width.
        const float same = 0.002f * (e.hi - e.lo);
        left = std::fabs(lo - e.lo) < same, right = std::fabs(hi - e.hi) < same;
      } else {
        left = std::fabs(lo - e.box_x) < 1.0f;
        right = hi > e.box_x + e.box_w - 1.0f;
      }
      if (left && !right) {  // rigid, with the left corner
        what = "left end";
        return x - d;
      }
      if (right && !left) {  // rigid, with the right corner
        what = "right end";
        return x + d;
      }
      what = "middle";
      return x < 0.5f * (lo + hi) ? x - d : x + d;            // stretched (or a lone tile)
    }
    what = "door body";
    if (x < kDoorKeepLo) return kDoorKeepLo - (kDoorKeepLo - x) * (1.0f + d / (kDoorKeepLo - kScreenLeft));
    if (x > kDoorKeepHi) return kDoorKeepHi + (x - kDoorKeepHi) * (1.0f + d / (kScreenRight - kDoorKeepHi));
    return x;  // the N tile: as drawn
  };
  for (uint32_t v = 0; v < b.count; ++v) {
    const float x = canvas_x(v);
    const float dx = moved(x) - x;  // canvas units -> vertex units: / (240 A c4.x)
    WriteFloat(base, at(v), LoadF(base, at(v)) + dx / (half * b.c4x));
  }
  if (g_logged < 12) {
    ++g_logged;
    REXLOG_INFO("Menu frame wide: picture {} tile x {:.1f}..{:.1f} of {:.1f}..{:.1f} ({}), side move {:.1f}",
                g_drawing, lo, hi, e.lo, e.hi, what, d);
  }
}

}  // namespace menu_frame_wide

using namespace menu_frame_wide;

// CNVBackgroundAction's per-frame update: shows the 4:3 or the 16:9 layer,
// turns the cogs, pulses the light.
extern "C" REX_FUNC(sub_820D7B88) {
  const uint32_t action = ctx.r3.u32;
  const uint32_t layer = action ? Read32(base, action + kWideLayer) : 0;
  const float d = SideMove();
  if (layer && layer != g.layer) FindPieces(ctx, base, layer);
  if (layer) PlacePieces(base, d);
  if (d > 0.0f) {  // the N. V. door's pictures (edited as they draw)
    const PPCContext saved = ctx;
    FindDoor(ctx, base);
    ctx = saved;
  }
  __imp__sub_820D7B88(ctx, base);
}

// "How wide is the visible canvas?" (853 / 640). Wider only for the
// backdrop's gradient.
extern "C" REX_FUNC(sub_820C4D60) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_820C4D60(ctx, base);
  if (lr != kGradientAsk1 && lr != kGradientAsk2) return;
  const float d = SideMove();
  if (d > 0.0f) ctx.f1.f64 = double(float(ctx.f1.f64) + 2.0f * d);
}

// The backdrop's rings.
extern "C" REX_FUNC(sub_820D8408) {
  g_in_rings = SideMove() > 0.0f;
  __imp__sub_820D8408(ctx, base);
  g_in_rings = false;
}

// The backdrop's quad helper: r4 = 4 corners (x, y, z floats, canvas units).
// Inside the rings' draw, an outer ring (left or right of the centre) is
// drawn again every kRingSpacing units further out while it can still be
// seen; the centre ring and the gradient are left alone.
extern "C" REX_FUNC(sub_820D8048) {
  if (!g_in_rings) {
    __imp__sub_820D8048(ctx, base);
    return;
  }
  const PPCContext saved = ctx;
  const uint32_t corners = ctx.r4.u32;
  float xs[4];
  float mid = 0.0f;
  for (int i = 0; i < 4; ++i) mid += (xs[i] = ReadFloat(base, corners + 12 * i)) * 0.25f;
  __imp__sub_820D8048(ctx, base);
  const float out = std::fabs(mid - kCanvasCentreX);
  if (out < 50.0f) return;  // the centre ring
  const float dir = mid < kCanvasCentreX ? -1.0f : 1.0f;
  const float half = 240.0f * widescreen::CurrentAspect();  // visible half width (canvas 480 tall)
  for (int k = 1; k < 8; ++k) {
    const float shift = dir * kRingSpacing * float(k);
    if (std::fabs(mid + shift - kCanvasCentreX) - kRingReach > half) break;
    for (int i = 0; i < 4; ++i) WriteFloat(base, corners + 12 * i, xs[i] + shift);
    ctx = saved;
    __imp__sub_820D8048(ctx, base);
  }
  for (int i = 0; i < 4; ++i) WriteFloat(base, corners + 12 * i, xs[i]);
  ctx = saved;
}
