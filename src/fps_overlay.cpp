// =============================================================================
// fps_overlay.cpp -- see fps_overlay.h
// =============================================================================
#include "fps_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/ppc/func.h>

#include "frame_rate.h"
#include "pddi/intercept.h"
#include "ui/scrooby.h"

REXCVAR_DEFINE_INT32(fps_overlay_position, 0, "CrashMoM",
                     "Where the frame-rate counter is: 0 top left, 1 top centre, 2 top right, 3 "
                     "bottom left, 4 bottom right")
    .range(0, 4);
REXCVAR_DEFINE_INT32(fps_overlay, 0, "CrashMoM",
                     "Frame-rate counter at the top of the window: 0 off, 1 FPS, 2 + average and "
                     "1% low, 3 + frame times (game / swap)")
    .range(0, 3);

namespace fps_overlay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kLiveSeconds = 0.25;
constexpr double kAverageSeconds = 5.0;
constexpr double kLowSeconds = 10.0;

// One game frame.
struct Frame {
  Clock::time_point end;
  float ms, game_ms, swap_ms;
};

// Written by the frame listener (game's main thread), read by the overlay
// (UI thread).
std::mutex g_mutex;
std::deque<Frame> g_frames;  // the last kLowSeconds
Clock::time_point g_previous;
bool g_have_previous = false;

void OnFrameEnd(void*) {
  const Clock::time_point now = Clock::now();
  if (REXCVAR_GET(fps_overlay) <= 0) {
    g_have_previous = false;  // nothing to show: keep no history
    return;
  }
  if (!g_have_previous) {
    g_have_previous = true;
    g_previous = now;
    return;
  }
  const pddi::FrameTiming& timing = pddi::LastFrameTiming();
  // The game's own work: without the pacer's wait for the frame's start time.
  const double game_ms = timing.game_ms - std::min(frame_rate::TakePaceWaitMs(), timing.game_ms);
  const float ms = float(std::chrono::duration<double, std::milli>(now - g_previous).count());
  g_previous = now;
  std::lock_guard lock(g_mutex);
  g_frames.push_back({now, ms, float(game_ms), float(timing.swap_ms)});
  while (!g_frames.empty() &&
         std::chrono::duration<double>(now - g_frames.front().end).count() > kLowSeconds) {
    g_frames.pop_front();
  }
}

// The numbers of one draw.
struct Numbers {
  double fps = 0, average = 0, low = 0, ms = 0, game_ms = 0, swap_ms = 0;
  bool valid = false;
};

Numbers Compute() {
  Numbers n;
  std::lock_guard lock(g_mutex);
  if (g_frames.empty()) return n;
  const Clock::time_point last = g_frames.back().end;
  double live_ms = 0, live_game = 0, live_swap = 0, average_ms = 0;
  int live = 0, average = 0;
  std::vector<float> all;
  all.reserve(g_frames.size());
  for (auto it = g_frames.rbegin(); it != g_frames.rend(); ++it) {
    const double age = std::chrono::duration<double>(last - it->end).count();
    if (age < kLiveSeconds) {
      live_ms += it->ms;
      live_game += it->game_ms;
      live_swap += it->swap_ms;
      ++live;
    }
    if (age < kAverageSeconds) {
      average_ms += it->ms;
      ++average;
    }
    all.push_back(it->ms);
  }
  if (!live || live_ms <= 0) return n;
  n.fps = 1000.0 * live / live_ms;
  n.ms = live_ms / live;
  n.game_ms = live_game / live;
  n.swap_ms = live_swap / live;
  n.average = average_ms > 0 ? 1000.0 * average / average_ms : n.fps;
  // The 99th percentile frame time = the slowest 1 %.
  const size_t k = std::min(all.size() - 1, size_t(double(all.size()) * 0.99));
  std::nth_element(all.begin(), all.begin() + std::ptrdiff_t(k), all.end());
  n.low = all[k] > 0 ? 1000.0 / all[k] : 0;
  n.valid = true;
  return n;
}

// The counter's texts on the button prompts' page (FE_Buttons.pag
// "PortFps0".."PortFps4", one per place: options_page.cpp).
extern "C" REX_FUNC(__imp__sub_821241E8);  // FindPage(layer, page name)

constexpr int kPlaces = 5;
constexpr float kScale = 0.7f;  // the texts' size (options_page.cpp places them for it)

// The counter's two copies: on the button prompts' page (menus) and on the
// HUD page (play). Each is drawn only while its page is; both get the same
// text, so when both show they overlap exactly.
struct Copy {
  const char* page_name;
  uint32_t page = 0;
  std::array<uint32_t, kPlaces> text{};
};
struct Texts {
  std::array<Copy, 2> copies{{{"FE_Buttons"}, {"InGame"}}};
  Clock::time_point next_update;
  int shown_style = -1, shown_place = -1;
};
Texts g_texts;

uint32_t ColourFor(double fps) {
  // Against the cap: green at it, yellow below 90 %, red below 75 %.
  const int cap = frame_rate::EffectiveCap();
  const double share = cap > 0 ? fps / cap : 1.0;
  return share >= 0.9 ? 0xFF73FF73u : share >= 0.75 ? 0xFFFFD940u : 0xFFFF6659u;
}

std::u16string Line(const Numbers& n, int style) {
  std::string s = fmt::format("{:.0f} FPS", n.fps);
  if (style >= 2) s += fmt::format("   AVG {:.1f}   1% LOW {:.1f}", n.average, n.low);
  if (style >= 3) s += fmt::format("   {:.1f} MS   GAME {:.1f}   SWAP {:.1f}", n.ms, n.game_ms, n.swap_ms);
  return std::u16string(s.begin(), s.end());
}

}  // namespace

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

void Update(PPCContext& ctx, uint8_t* base) {
  const int style = REXCVAR_GET(fps_overlay);
  const int place = std::clamp(int(REXCVAR_GET(fps_overlay_position)), 0, kPlaces - 1);
  const Clock::time_point now = Clock::now();
  if (now < g_texts.next_update && style == g_texts.shown_style && place == g_texts.shown_place) return;
  g_texts.next_update = now + std::chrono::milliseconds(250);
  const Numbers n = Compute();
  const bool show = style > 0 && n.valid;
  const std::u16string line = show ? Line(n, style) : u"";
  const uint32_t colour = show ? ColourFor(n.fps) : 0;
  for (Copy& c : g_texts.copies) {
    // The page, as the game's GetMenuIndex finds one (layer 4, then 0): the
    // HUD page comes and goes with levels, looked up every time.
    uint32_t page = scrooby::Call(__imp__sub_821241E8, ctx, base, 4, scrooby::GuestAscii(c.page_name));
    if (!page) page = scrooby::Call(__imp__sub_821241E8, ctx, base, 0, scrooby::GuestAscii(c.page_name));
    // Its texts found again every time too: a new level's HUD page can land
    // where the old one was, and old element pointers would be stale.
    c.page = page;
    if (!page) continue;
    for (int i = 0; i < kPlaces; ++i) {
      c.text[size_t(i)] = scrooby::FindText(ctx, base, page, fmt::format("PortFps{}", i));
    }
    if (show) scrooby::SetScale(ctx, base, c.text[size_t(place)], kScale);
    // Only the chosen place shows (a place changed: the old one hides).
    for (int i = 0; i < kPlaces; ++i) {
      scrooby::SetVisible(base, c.text[size_t(i)], show && i == place);
    }
    if (!show) continue;
    scrooby::SetText(ctx, base, c.text[size_t(place)], line);
    scrooby::SetColour(ctx, base, c.text[size_t(place)], colour);
  }
  g_texts.shown_style = style;
  g_texts.shown_place = place;
}

}  // namespace fps_overlay
