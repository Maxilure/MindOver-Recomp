// =============================================================================
// fps_overlay.cpp -- see fps_overlay.h
// =============================================================================
#include "fps_overlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "frame_rate.h"
#include "pddi/intercept.h"
#include "ui/game_font.h"

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


// The line's colour, against the frame-rate cap: green at it, yellow below
// 90 %, red below 75 %.
ImU32 ColourFor(double fps) {
  const int cap = frame_rate::EffectiveCap();
  const double share = cap > 0 ? fps / cap : 1.0;
  return share >= 0.9 ? IM_COL32(0x73, 0xFF, 0x73, 0xFF)
         : share >= 0.75 ? IM_COL32(0xFF, 0xD9, 0x40, 0xFF)
                         : IM_COL32(0xFF, 0x66, 0x59, 0xFF);
}

// The line in one of the three styles (fps_overlay.h).
std::u16string Line(const Numbers& n, int style) {
  std::string s = fmt::format("{:.0f} FPS", n.fps);
  if (style >= 2) s += fmt::format("   AVG {:.1f}   1% LOW {:.1f}", n.average, n.low);
  if (style >= 3) s += fmt::format("   {:.1f} MS   GAME {:.1f}   SWAP {:.1f}", n.ms, n.game_ms, n.swap_ms);
  return std::u16string(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// The drawing: our own text on the layer over the finished picture
// ---------------------------------------------------------------------------
// An ImGui "dialog" with no window: it only puts textured quads (the game
// font's letters, game_font.h) on ImGui's foreground draw list, which the
// SDK paints over the guest picture in the main window, whichever renderer
// made it. Nothing in the game decides whether it shows.

constexpr float kCapHeight = 15.0f;  // a capital's height at 720p (the HUD's small text x 0.7)
constexpr float kMargin = 10.0f;     // from the picture's edges, at 720p
constexpr ImU32 kOutlineColour = IM_COL32(0, 0, 0, 210);

class CounterDialog : public rex::ui::ImGuiDialog {
 public:
  CounterDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
      : ImGuiDialog(drawer), immediate_(immediate) {}

  // The window repaints with every game frame anyway; no extra repaints.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const int style = REXCVAR_GET(fps_overlay);
    if (style <= 0 || !FontReady()) return;
    // New numbers 4 times a second (every frame would be unreadable).
    const Clock::time_point now = Clock::now();
    if (now >= next_update_ || style != shown_style_) {
      next_update_ = now + std::chrono::milliseconds(250);
      shown_style_ = style;
      const Numbers n = Compute();
      line_ = n.valid ? Line(n, style) : u"";
      colour_ = n.valid ? ColourFor(n.fps) : 0;
    }
    if (line_.empty()) return;

    // The picture's rectangle in the window: the game's 16:9, fitted
    // (the presenter letterboxes it the same way).
    const float window_w = io.DisplaySize.x, window_h = io.DisplaySize.y;
    if (window_w <= 0 || window_h <= 0) return;
    float pic_w = window_w, pic_h = window_w * 9.0f / 16.0f;
    if (pic_h > window_h) {
      pic_h = window_h;
      pic_w = window_h * 16.0f / 9.0f;
    }
    const float pic_x = (window_w - pic_w) * 0.5f, pic_y = (window_h - pic_h) * 0.5f;
    const float unit = pic_h / 720.0f;  // one pixel of the game's 720p picture
    const float scale = kCapHeight * unit / font_.cap_height();
    const float width = font_.Measure(line_) * scale, height = font_.cell_height() * scale;
    const float margin = kMargin * unit;

    // The place (--fps_overlay_position): 0 top left, 1 top centre, 2 top
    // right, 3 bottom left, 4 bottom right.
    const int place = std::clamp(int(REXCVAR_GET(fps_overlay_position)), 0, 4);
    float x = pic_x + margin;
    if (place == 1) x = pic_x + (pic_w - width) * 0.5f;
    if (place == 2 || place == 4) x = pic_x + pic_w - margin - width;
    const float y = place >= 3 ? pic_y + pic_h - margin - height : pic_y + margin;

    // Outline layer first (dark), then the letters (coloured), each letter
    // at whole pixels so it stays sharp.
    ImDrawList* list = ImGui::GetForegroundDrawList();
    for (int layer = 0; layer < 2; ++layer) {
      float pen = std::round(x);
      for (char16_t c : line_) {
        const game_font::Glyph* g = font_.Find(c);
        if (!g) continue;
        if (g->width > 0) {
          rex::ui::ImmediateTexture* texture = Texture(g->page, layer == 1);
          if (texture) {
            const ImVec2 a(std::round(pen + g->left * scale), std::round(y + g->top * scale));
            const ImVec2 b(a.x + g->width * scale, a.y + g->height * scale);
            list->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(texture)), a, b,
                           ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1),
                           layer == 0 ? kOutlineColour : colour_);
          }
        }
        pen += g->advance * scale;
      }
    }
  }

 private:
  // The font, read once the first time the counter is on (from the game's
  // archive: the runtime must exist). A failure is final (logged once).
  bool FontReady() {
    if (font_.loaded()) return true;
    if (font_failed_ || !rex::Runtime::instance()) return false;
    font_failed_ = !font_.Load("Titans_Small");
    if (!font_failed_) {
      textures_.resize(font_.page_count());
    }
    return !font_failed_;
  }

  // A page's layer as a texture, made on first use (UI thread).
  rex::ui::ImmediateTexture* Texture(uint32_t page, bool fill) {
    if (!immediate_ || page >= textures_.size()) return nullptr;
    std::unique_ptr<rex::ui::ImmediateTexture>& slot = fill ? textures_[page].fill : textures_[page].outline;
    if (!slot) {
      const game_font::PageLayers& l = font_.Layers(page);
      slot = immediate_->CreateTexture(l.width, l.height, rex::ui::ImmediateTextureFilter::kLinear,
                                       false, fill ? l.fill.data() : l.outline.data());
    }
    return slot.get();
  }

  struct PageTextures {
    std::unique_ptr<rex::ui::ImmediateTexture> fill, outline;
  };
  rex::ui::ImmediateDrawer* immediate_;
  game_font::Font font_;
  bool font_failed_ = false;
  std::vector<PageTextures> textures_;
  Clock::time_point next_update_;
  int shown_style_ = -1;
  std::u16string line_;
  ImU32 colour_ = 0;
};

// The one dialog, kept for the whole session (like overlay_banner's: the
// drawer doesn't own its dialogs and the process ends right after it goes).
CounterDialog* g_dialog = nullptr;

}  // namespace

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  if (drawer && !g_dialog) {
    g_dialog = new CounterDialog(drawer, immediate);  // registers itself with the drawer
  }
}

}  // namespace fps_overlay
