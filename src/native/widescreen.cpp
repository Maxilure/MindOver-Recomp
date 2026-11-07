// native/widescreen.cpp -- see widescreen.h (a picture as wide as the window)
#include "widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../pddi/intercept.h"

REXCVAR_DEFINE_INT32(aspect_ratio, 0, "CrashMoM",
                     "Picture shape (native renderer, Native Only): 0 = match the window, 1 = 16:9 "
                     "(the original), 2 = 21:9, 3 = 32:9. Wider than 16:9 shows MORE of the world at "
                     "the sides (nothing stretched); a window of another shape gets black bars")
    .range(0, 3);
REXCVAR_DEFINE_DOUBLE(widescreen_max_aspect, 3.6, "CrashMoM",
                      "Widest picture shape when matching the window (width / height; 3.6 = just "
                      "over 32:9). A wider window gets black bars at the sides")
    .range(1.78, 6.0);
REXCVAR_DEFINE_DOUBLE(debug_widescreen_aspect, 0.0, "CrashMoM",
                      "Testing widescreen without a wide window: pretend the window is this "
                      "shape (width / height, e.g. 2.37 = 21:9); 0 = the real window");

namespace widescreen {
namespace {

// The game's display object (xnDisplay) and its shape floats (widescreen.h).
constexpr uint32_t kDisplayPointer = 0x82571B78;
constexpr uint32_t kWidth = 1680, kHeight = 1684, kWidth2 = 1688, kHeight2 = 1692;
constexpr float kGameHeightF = 0.5625f;  // 9 / 16: the game's height for width 1

std::atomic<rex::ui::Window*> g_window{nullptr};
std::atomic<float> g_aspect{kGameAspect};  // what the game draws with now
float g_logged = 0.0f;                     // last aspect announced in the log
bool g_logged_camera = false;

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float ReadFloat(uint32_t a) {
  const uint32_t b = Read32(a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void WriteFloat(uint32_t a, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  uint8_t* p = Guest(a);
  p[0] = uint8_t(b >> 24); p[1] = uint8_t(b >> 16); p[2] = uint8_t(b >> 8); p[3] = uint8_t(b);
}

// The shape the window asks for: its own, between 16:9 and the cap, rounded
// to 1/200 so a window being dragged doesn't re-make the surfaces for every
// pixel of width.
// The fixed shapes of the Aspect Ratio setting (index = the flag): "21:9"
// monitors are 64:27 (2560 x 1080; 3440 x 1440 is a little wider: Match
// Window fits those exactly), 32:9 = two 16:9 screens side by side.
constexpr float kFixedAspects[4] = {0.0f, 16.0f / 9.0f, 64.0f / 27.0f, 32.0f / 9.0f};

float WantedAspect(bool allowed) {
  rex::ui::Window* window = g_window.load(std::memory_order_relaxed);
  if (!allowed) return kGameAspect;
  const int choice = std::clamp(int(REXCVAR_GET(aspect_ratio)), 0, 3);
  if (choice > 0 && REXCVAR_GET(debug_widescreen_aspect) <= 0.0) return kFixedAspects[choice];
  if (!window) return kGameAspect;
  float w = float(window->GetActualPhysicalWidth());
  float h = float(window->GetActualPhysicalHeight());
  if (REXCVAR_GET(debug_widescreen_aspect) > 0.0) {
    w = float(REXCVAR_GET(debug_widescreen_aspect));
    h = 1.0f;
  }
  if (w <= 0.0f || h <= 0.0f) return kGameAspect;
  const float cap = float(REXCVAR_GET(widescreen_max_aspect));
  float a = std::clamp(w / h, kGameAspect, std::max(kGameAspect, cap));
  a = std::round(a * 200.0f) / 200.0f;
  return a < kGameAspect + 0.01f ? kGameAspect : a;
}

}  // namespace

void AttachWindow(rex::ui::Window* window) { g_window.store(window, std::memory_order_relaxed); }

float CurrentAspect() { return g_aspect.load(std::memory_order_relaxed); }

Rect Picture(float window_w, float window_h) {
  const float a = CurrentAspect();
  Rect r;
  r.w = window_w;
  r.h = window_w / a;
  if (r.h > window_h) {
    r.h = window_h;
    r.w = window_h * a;
  }
  r.x = (window_w - r.w) * 0.5f;
  r.y = (window_h - r.h) * 0.5f;
  return r;
}

int WindowPictureHeight() {
  rex::ui::Window* window = g_window.load(std::memory_order_relaxed);
  if (!window) return 0;
  const float w = float(window->GetActualPhysicalWidth()), h = float(window->GetActualPhysicalHeight());
  if (w <= 0.0f || h <= 0.0f) return 0;
  return int(Picture(w, h).h + 0.5f);
}

Rect Game(float window_w, float window_h) {
  Rect r = Picture(window_w, window_h);
  const float w = r.h * kGameAspect;
  r.x += (r.w - w) * 0.5f;
  r.w = w;
  return r;
}

float Update(bool allowed) {
  const float drawn = g_aspect.load(std::memory_order_relaxed);
  const float next = WantedAspect(allowed);
  // The display object exists once the renderer is up (it's made before the
  // first frame); 0 = not yet.
  const uint32_t display = Read32(kDisplayPointer);
  if (display < 0x40000000u) return drawn;
  // Heights = 1 / aspect (16:9: 0.5625), +1688 = 1: the menus' projection
  // (aspect = slot 0x50 = +1688 / +1692) and their canvas scale
  // (min(+1680 / 640, +1684 / 480) / +1688, sub_8237BBE8) then cancel out:
  // pages keep their size and stay centred (measured 2026-11-07).
  // +1680 = (16 / 9) / aspect: slot 0x54 (+1680 / +1684, read by the views,
  // sub_82379B20 / sub_82379D40) still answers 16:9, like the camera's own
  // aspect (sub_82381E78 below): the game's logic keeps its 16:9 screen.
  // The canvas scale's min() picks +1684 either way.
  const float height = 1.0f / next, width = kGameAspect / next;
  if (ReadFloat(display + kHeight) != height || ReadFloat(display + kHeight2) != height ||
      ReadFloat(display + kWidth) != width) {
    WriteFloat(display + kWidth, width);
    WriteFloat(display + kHeight, height);
    WriteFloat(display + kWidth2, 1.0f);
    WriteFloat(display + kHeight2, height);
  }
  g_aspect.store(next, std::memory_order_relaxed);
  if (next != g_logged) {
    g_logged = next;
    REXLOG_INFO("Widescreen: picture {:.3f}:1 ({})", next,
                next > kGameAspect ? "wider than the game's 16:9" : "the game's 16:9");
  }
  return drawn;
}

namespace {

// pddiBaseContext::v29 = SET PROJECTION (sub_8243E4B8): f1 near, f2 far, f3
// HORIZONTAL field of view (radians), f4 aspect, r3 the context. It stores
// them in the context's state block (+0x88: +0x24 .. +0x30) and rebuilds the
// projection (xnContext slot 122, sub_824319A8, perspective case: x extent =
// near x tan(fov / 2), y extent = x extent / aspect).
// Callers seen (2026-11-07, Crash's house): the CAMERA's view set-up
// (VectorCamera slot 22 sub_82381B38, return address 0x82381BAC) with its
// field of view (+16) and aspect (+28: the game's, see CameraAspect); the
// menus (sub_8237BBE8 / sub_82262FA8: fov 1, the display's aspect); the
// context's state restore (sub_8243D4E8) and sub_822F2498 (reads the current
// projection back, sets it again with another near plane): both re-send what
// is stored = already converted. So ONLY the camera's set-up is converted:
// our aspect, and a field of view widened so the VERTICAL one stays the
// game's 16:9 one.
constexpr uint32_t kCameraSetProjection = 0x82381BAC;

void OnSetProjection(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const float aspect = g_aspect.load(std::memory_order_relaxed);
  const float fov = float(ctx.f3.f64), asked = float(ctx.f4.f64);
  if (uint32_t(ctx.lr) == kCameraSetProjection && std::fabs(aspect - kGameAspect) > 0.001f &&
      fov > 0.05f && fov < 3.0f) {
    // Wider by the picture's factor (the camera's own aspect is the game's:
    // 16:9, or another shape for a viewport of its own; CameraAspect below).
    const float wide = aspect / kGameAspect;
    ctx.f3.f64 = 2.0 * std::atan(double(std::tan(0.5f * fov)) * wide);
    ctx.f4.f64 = double(asked) * wide;
    if (!g_logged_camera) {
      g_logged_camera = true;
      REXLOG_INFO("Widescreen: world projection widened: field of view {:.3f} -> {:.3f} rad, "
                  "aspect {:.3f} -> {:.3f} (from {:08X})",
                  fov, float(ctx.f3.f64), asked, aspect, uint32_t(ctx.lr));
    }
  }
  original(ctx, base);
}

}  // namespace

// THE CAMERA'S VIEW SET-UP: Camera slot 0x14 sub_82381E78 (r3 camera, f1 the
// field of view, f2 the viewport's shape), called by the view set-up before
// it hands +16 / +28 to the projection. Read 2026-11-07 (xexdis):
//   +16 field of view (f1; when byte +209 is set, made from a vertical one
//       with the display's aspect, sub_82381D88), +20 = f1, +24 = f2;
//   +28 aspect = +212 x +24 x xnDisplay slot 0x50 (the menus' WIDE aspect;
//       call 0x82381F08, store 0x82381F28, found with `who`);
//   +40 = 1 / tan(fov / 2), +44 = +28 x +40: the projection's scale factors;
//   +48..+76 = the VIEW FRUSTUM's side planes (sin / cos of fov / 2 and of
//       the vertical half angle atan(tan(fov / 2) / aspect)): what the game
//       CULLS with. With the 16:9 field of view they cut the wide picture's
//       sides: scenery there popped in only once the 16:9 view reached it
//       (playtest 2026-11-07).
// So the original runs with OUR field of view (Hor+, the wide picture's):
// its frustum is the drawn one. Then the game's 16:9 view goes back into
// +16 / +28 / +40 / +44, so everything the GAME computes with its camera
// (aiming, "where on screen", markers, the co-op camera's fit) is the middle
// 16:9 of our picture, exactly what the original shows. OnSetProjection
// widens the drawn projection from those.
constexpr uint32_t kCamFov = 16, kCamFovIn = 20, kCamAspect = 28, kCamScaleX = 40, kCamScaleY = 44;
constexpr uint32_t kCamFovFromVertical = 209;  // byte: +16 made from a vertical field of view
bool g_logged_vertical = false;

}  // namespace widescreen

extern "C" REX_FUNC(__imp__sub_82381E78);
extern "C" REX_FUNC(sub_82381E78) {
  using namespace widescreen;
  const uint32_t camera = ctx.r3.u32;
  const float aspect = CurrentAspect();
  const float wide = aspect / kGameAspect;
  const bool from_vertical = camera && *Guest(camera + kCamFovFromVertical) != 0;
  const float game_fov = float(ctx.f1.f64);
  const bool widen = camera && wide > 1.001f && !from_vertical && game_fov > 0.05f && game_fov < 3.0f;
  if (widen) ctx.f1.f64 = 2.0 * std::atan(std::tan(0.5 * game_fov) * wide);
  __imp__sub_82381E78(ctx, base);
  if (!camera || wide <= 1.001f) return;
  if (from_vertical && !g_logged_vertical) {
    g_logged_vertical = true;  // (not seen yet: its +16 is already the display's wide one)
    REXLOG_INFO("Widescreen: camera {:08X} makes its view from a vertical field of view", camera);
  }
  // The game's own 16:9 view back for its logic (the frustum stays wide).
  const float now = ReadFloat(camera + kCamAspect);
  if (!(now > 0.1f && now < 20.0f)) return;
  const float game_aspect = now / wide;
  WriteFloat(camera + kCamAspect, game_aspect);
  if (widen) {
    WriteFloat(camera + kCamFov, game_fov);
    WriteFloat(camera + kCamFovIn, game_fov);
    const float scale_x = 1.0f / std::tan(0.5f * game_fov);
    WriteFloat(camera + kCamScaleX, scale_x);
    WriteFloat(camera + kCamScaleY, game_aspect * scale_x);
  }
}

namespace widescreen {

namespace {

// THE CANVAS <-> SCREEN CONVERSIONS (2026-11-07, playtest: the mojo cursor
// aimed off, players' markers sat at the bottom "off screen" though on screen,
// mojo flew past the portraits): the HUD's canvas point -> a 0..1 screen
// point (sub_82265D40: x = (+0xC / +0x10) / 480 x (cx - 320) + 0.5) and back
// (sub_82265DE8; its callers sub_82265E98 + the markers' sub_8226B2C8) read
// the display's shape (xnDisplay slot 0x48 = the struct at +1672: +8 / +0xC
// / +0x10 / +0x14 = +1680..+1692). Those say WIDE (the menus need it), but
// the 0..1 point goes through the GAME's camera, which stays 16:9 (see the
// camera's view set-up below): the two disagreed by the wide factor. Asked
// from those two helpers, the getter answers a copy with the game's own
// 16:9 shape, so canvas, 0..1 point and camera are all the game's 16:9 again
// (the middle 16:9 of our picture). Points beyond it (the wide sides) just
// fall outside 0..1, where the camera's maths carry on in a straight line.
constexpr uint32_t kShapeStruct = 1672;  // slot 0x48's answer: display + 1672
uint32_t g_game_shape = 0;               // guest copy (40 bytes), made once

void OnDisplayShape(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t display = ctx.r3.u32;
  original(ctx, base);
  const bool conversion = lr >= 0x82265D40 && lr < 0x82265E98;  // sub_82265D40 / sub_82265DE8
  if (!conversion || std::fabs(CurrentAspect() - kGameAspect) < 0.001f) return;
  if (!g_game_shape) g_game_shape = rex::system::kernel_memory()->SystemHeapAlloc(40);
  if (!g_game_shape) return;
  std::memcpy(Guest(g_game_shape), Guest(display + kShapeStruct), 40);
  WriteFloat(g_game_shape + 8, 1.0f);              // +1680 width
  WriteFloat(g_game_shape + 0xC, kGameHeightF);    // +1684 height (16:9)
  WriteFloat(g_game_shape + 0x10, 1.0f);           // +1688
  WriteFloat(g_game_shape + 0x14, kGameHeightF);   // +1692
  ctx.r3.u64 = g_game_shape;
}

}  // namespace

void Install() {
  pddi::SetHandler(pddi::kFn_8243E4B8, &OnSetProjection);
  pddi::SetHandler(pddi::kFn_82433960, &OnDisplayShape);
}

}  // namespace widescreen
