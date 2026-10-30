// =============================================================================
// ui/marquee.cpp -- see marquee.h
// =============================================================================
#include "marquee.h"
#include "hud_scale.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../native/guest.h"

namespace marquee {
namespace {

using Clock = std::chrono::steady_clock;

// THE TIMELINE of one round (seconds; speed in the caller's units per
// second: the menus' 640 x 480 canvas units, ~1.5 screen pixels each at 720p).
constexpr double kHoldStart = 1.6;  // the start is read first
constexpr double kSpeed = 45.0;     // ~a word a second: readable, not sluggish
constexpr double kHoldEnd = 1.2;    // the end is read too
constexpr double kFade = 0.35;      // out at the end, in at the start
// A text not drawn for this long (its screen closed, a page turned) starts
// again from its beginning when it shows again.
constexpr double kForgetAfter = 0.3;
// Fits within this much (0.5 %) don't scroll (measuring noise).
constexpr float kSlack = 1.005f;

struct Text {
  float width = 0, room = 0;  // as Fit got them
  Clock::time_point start{};   // the round's clock (restarted on changes)
  Clock::time_point last_drawn{};
  Clock::time_point last_fit{};  // the caller's last Fit
  bool drawn_once = false;
  // Measured from the game's own vertices (unshifted), any units:
  float full_min = 0, full_max = 0;   // the whole text's left / right, last draw
  float first_offset = 0;             // first batch's left - the whole text's left, last draw
  bool measured = false;
  // This draw (wrapper -> batches):
  float cur_min = 0, cur_max = 0, cur_first_min = 0;
  int batches = 0;
  bool logged = false;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Text> g_texts;
// The text drawing right now (its wrapper is running), or null. Read by the
// renderer hooks on every batch: kept separate from the map's lock.
std::atomic<Text*> g_active{nullptr};
std::atomic<uint32_t> g_active_text{0};
uint8_t g_justify = 0;  // the active text's justification byte

// The open batch (between BeginPrims and EndPrims of the active text).
struct Batch {
  bool open = false;
  uint32_t vertices = 0, stride = 0, count = 0, prim_type = 0;
  int uv = -1, colour = -1;
} g_batch;

float LoadF(const uint8_t* base, uint32_t a) {
  uint32_t v = native::guest::Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// Where the round is: scroll position 0..1 and the fade (alpha factor).
void Timeline(double t, double scroll_seconds, float* position, float* alpha) {
  const double round = kFade + kHoldStart + scroll_seconds + kHoldEnd + kFade;
  // The very first round starts already faded in.
  double local = std::fmod(t + kFade, round);
  *position = 0, *alpha = 1;
  if (local < kFade) {
    *alpha = float(local / kFade);
    return;
  }
  local -= kFade;
  if (local < kHoldStart) return;
  local -= kHoldStart;
  if (local < scroll_seconds) {
    *position = float(local / scroll_seconds);
    return;
  }
  local -= scroll_seconds;
  *position = 1;
  if (local < kHoldEnd) return;
  local -= kHoldEnd;
  *alpha = float(std::clamp(1.0 - local / kFade, 0.0, 1.0));
}

}  // namespace

void Fit(uint32_t text, float width, float room) {
  if (!text) return;
  std::lock_guard lock(g_mutex);
  if (!(width > room * kSlack) || room <= 0) {
    if (g_texts.erase(text)) REXLOG_DEBUG("Marquee: text {:08X} fits again", text);
    return;
  }
  Text& t = g_texts[text];
  t.last_fit = Clock::now();
  if (std::fabs(t.width - width) > 0.5f || std::fabs(t.room - room) > 0.5f) {
    // New words or a new box: from the start.
    t.width = width, t.room = room;
    t.start = Clock::now();
    t.measured = false;
    REXLOG_INFO("Marquee: text {:08X} scrolls ({:.0f} wide, room {:.0f})", text, width, room);
  }
}

double OnePassSeconds(float width, float room) {
  if (!(width > room * kSlack) || room <= 0) return 0;
  return kHoldStart + (width - room) / kSpeed + kHoldEnd;
}

void Stop(uint32_t text) {
  std::lock_guard lock(g_mutex);
  g_texts.erase(text);
}

void OnBeginPrims(uint8_t* base, uint32_t xn_context, uint32_t prim_type) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  g_batch = {};
  const uint32_t stream = native::guest::Load32(base, xn_context + native::guest::context::kPrimStream);
  if (!stream) return;
  const uint32_t vertices = native::guest::Load32(base, stream + native::guest::stream::kPosition);
  const uint32_t stride = native::guest::Load32(base, stream + native::guest::stream::kStride);
  const uint32_t count = native::guest::Load32(base, stream + native::guest::stream::kCount);
  if (!vertices || stride < 12 || stride > 252 || !count || count > 4096) return;
  auto offset_of = [&](uint32_t field) {
    const uint32_t p = native::guest::Load32(base, stream + field);
    return p > vertices && p - vertices < stride ? int(p - vertices) : -1;
  };
  g_batch.open = true;
  g_batch.vertices = vertices, g_batch.stride = stride, g_batch.count = count;
  g_batch.prim_type = prim_type;
  g_batch.uv = offset_of(native::guest::stream::kUv);
  g_batch.colour = offset_of(native::guest::stream::kColour);
}

void OnEndPrims(uint8_t* base) {
  Text* t = g_active.load(std::memory_order_relaxed);
  if (!t || !g_batch.open) return;
  g_batch.open = false;
  const Batch& b = g_batch;
  auto x_at = [&](uint32_t v) { return b.vertices + v * b.stride; };  // x = first float
  // 1. This batch's extent, as the game laid it out.
  float lo = 1e30f, hi = -1e30f;
  for (uint32_t v = 0; v < b.count; ++v) {
    const float x = LoadF(base, x_at(v));
    if (!std::isfinite(x)) return;
    lo = std::min(lo, x), hi = std::max(hi, x);
  }
  if (t->batches == 0) {
    t->cur_min = lo, t->cur_max = hi, t->cur_first_min = lo;
  } else {
    t->cur_min = std::min(t->cur_min, lo), t->cur_max = std::max(t->cur_max, hi);
  }
  ++t->batches;
  if (!t->logged) {
    t->logged = true;
    REXLOG_INFO("Marquee: text {:08X} batch: type {} vertices {} stride {} uv +{} colour +{} x {:.1f}..{:.1f}",
                g_active_text.load(), b.prim_type, b.count, b.stride, b.uv, b.colour, lo, hi);
  }
  // Glyph quads only: triangle lists (3 per triangle) or 4-vertex strips.
  const bool list = b.prim_type == 0 && b.count % 3 == 0;
  const bool quads = b.prim_type == 1 && b.count % 4 == 0;
  if (!list && !quads) return;
  // 2. The whole text's extent: last draw's width, anchored at this draw's
  //    first batch (the page may slide between frames); the first draw
  //    uses what it has.
  float full_min = t->cur_first_min, full_max = t->cur_max;
  if (t->measured) {
    full_min = t->cur_first_min - t->first_offset;
    full_max = full_min + (t->full_max - t->full_min);
  }
  const float extent = full_max - full_min;
  if (!(extent > 0)) return;
  const float visible = extent * std::clamp(t->room / t->width, 0.05f, 1.0f);
  float left;
  switch (g_justify) {
    case 0: left = full_min; break;                                // left
    case 1: left = full_max - visible; break;                      // right
    default: left = (full_min + full_max) / 2 - visible / 2; break;  // centred
  }
  const float right = left + visible;
  // 3. Where the round is.
  const double now = std::chrono::duration<double>(Clock::now() - t->start).count();
  float position, alpha;
  Timeline(now, (t->width - t->room) / kSpeed, &position, &alpha);
  const float shift = (left - full_min) - position * (extent - visible);
  // 4. Each glyph: moved, clamped to the window with its u moved alike, faded.
  const uint32_t group = list ? 3 : 4;
  for (uint32_t g0 = 0; g0 < b.count; g0 += group) {
    // The glyph's du/dx from its own corners (the two x extremes).
    float gx0 = 1e30f, gx1 = -1e30f, gu0 = 0, gu1 = 0;
    for (uint32_t v = g0; v < g0 + group; ++v) {
      const float x = LoadF(base, x_at(v));
      const float u = b.uv >= 0 ? LoadF(base, x_at(v) + b.uv) : 0;
      if (x < gx0) gx0 = x, gu0 = u;
      if (x > gx1) gx1 = x, gu1 = u;
    }
    const float dudx = gx1 > gx0 ? (gu1 - gu0) / (gx1 - gx0) : 0;
    for (uint32_t v = g0; v < g0 + group; ++v) {
      const float x = LoadF(base, x_at(v)) + shift;
      const float clamped = std::clamp(x, left, right);
      StoreF(base, x_at(v), clamped);
      if (b.uv >= 0 && clamped != x) {
        StoreF(base, x_at(v) + b.uv, LoadF(base, x_at(v) + b.uv) + (clamped - x) * dudx);
      }
      if (b.colour >= 0 && alpha < 1) {
        uint8_t* a = GuestPtr(base, x_at(v) + uint32_t(b.colour));  // ARGB, big-endian: A first
        a[0] = uint8_t(std::lround(a[0] * alpha));
      }
    }
  }
}

}  // namespace marquee

// =============================================================================
// Text@frontend slot 6 (sub_82371D68): one Scrooby text draws itself. Ours
// (in the map) are marked active around the original, so the renderer hooks
// change their batches; afterwards the draw's measurements are kept for the
// next frame.
// =============================================================================
extern "C" REX_FUNC(__imp__sub_82371D68);
extern "C" REX_FUNC(sub_82371D68) {
  using namespace marquee;
  const uint32_t text = ctx.r3.u32;
  // HUD size (ui/hud_scale.h): a player's text on the shared page InGame
  // (mojo count, multiplier, counter Y) shrinks toward that player's corner.
  hud_scale::Scope hud_size(ctx, base, hud_scale::WhereOfText(text));
  Text* t = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (!g_texts.empty()) {
      const auto it = g_texts.find(text);
      if (it != g_texts.end()) t = &it->second;
    }
  }
  if (!t) {
    __imp__sub_82371D68(ctx, base);
    return;
  }
  const auto now = Clock::now();
  if (t->drawn_once &&
      std::chrono::duration<double>(now - t->last_drawn).count() > kForgetAfter) {
    if (t->last_fit < t->last_drawn) {
      // Gone from the screen and nobody asked again since: forgotten (its
      // page may have been unloaded and this address be another text now).
      {
        std::lock_guard lock(g_mutex);
        g_texts.erase(text);
      }
      __imp__sub_82371D68(ctx, base);
      return;
    }
    t->start = now;  // back on screen: from the beginning
  }
  t->drawn_once = true;
  t->last_drawn = now;
  t->batches = 0;
  g_justify = *GuestPtr(base, text + 108);  // 0 left, 1 right, else centred (findings/39)
  g_active_text = text;
  g_active = t;
  __imp__sub_82371D68(ctx, base);
  g_active = nullptr;
  g_batch.open = false;
  if (t->batches > 0) {
    t->full_min = t->cur_min, t->full_max = t->cur_max;
    t->first_offset = t->cur_first_min - t->cur_min;
    t->measured = true;
  }
}
