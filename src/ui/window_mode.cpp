// =============================================================================
// ui/window_mode.cpp -- see window_mode.h
// =============================================================================
#include "window_mode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// Our third Window Mode (window_mode.h "BORDERLESS WINDOW"): only read while
// the SDK's fullscreen flag is off.
REXCVAR_DEFINE_BOOL(borderless_window, false, "CrashMoM",
                    "Borderless Window: with fullscreen off, the window has no border and covers the whole "
                    "monitor as an ordinary window (try it when screen sharing shows a frozen picture in "
                    "Fullscreen)");
REXCVAR_DEFINE_INT32(debug_borderless_window_gap, 0, "CrashMoM",
                     "Borderless Window test: pixels left uncovered at the bottom of the monitor (0 = the "
                     "exact monitor size; 1 = a window Windows never treats as fullscreen)")
    .range(0, 64);

namespace window_mode {
namespace {

std::function<void(std::function<void()>)> g_post;  // runs a job on the UI thread
std::function<void()> g_on_moved;                   // frame_rate::UpdateDisplayRefresh

std::mutex g_mutex;
std::vector<Display> g_displays;
std::atomic<bool> g_refresh_queued{false};

// UI thread only: the Borderless Window mode's state.
std::string g_main_title;          // finds the main SDL window
SDL_WindowID g_main_window_id = 0;  // found once
bool g_borderless_applied = false;  // our border / place / size are on the window now
int g_windowed_w = 0, g_windowed_h = 0;  // the window's size before (put back after)
SDL_Rect g_borderless_rect{};            // last place / size given (log only on a change)

// SDL's names are long on Linux: "Shenzhen Acme Technology Group X27Q
// 000000000000 (DP-2)" = maker, model, serial number, connector. The short
// name = the first word with a digit in it (the model) and the word before
// it (the brand, company words skipped); serial numbers (5+ digits) are left
// out everywhere.
void NameParts(Display& d) {
  std::string name = d.name;
  if (!name.empty() && name.back() == ')') {
    const size_t open = name.rfind('(');
    if (open != std::string::npos) {
      d.port = name.substr(open + 1, name.size() - open - 2);
      name.resize(open);
    }
  }
  std::vector<std::string> words;
  size_t i = 0;
  while (i < name.size()) {
    while (i < name.size() && name[i] == ' ') ++i;
    size_t end = i;
    while (end < name.size() && name[end] != ' ') ++end;
    if (end > i) {
      std::string word = name.substr(i, end - i);
      int digits = 0;
      for (const char c : word) digits += c >= '0' && c <= '9';
      if (digits < 5) words.push_back(std::move(word));
    }
    i = end;
  }
  // Company words are never the brand ("Acme Corporation X27Q").
  static const char* const kFiller[] = {"corporation", "corp", "corp.", "inc", "inc.", "co", "co.",
                                        "ltd", "ltd.", "limited", "technology", "technologies",
                                        "group", "electronics", "company", "international",
                                        "display", "displays"};
  std::vector<std::string> brandable;
  for (const auto& w : words) {
    std::string lower;
    for (const char c : w) lower += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    bool filler = false;
    for (const char* f : kFiller) filler |= lower == f;
    if (!filler) brandable.push_back(w);
  }
  size_t model = brandable.size();
  for (size_t w = 0; w < brandable.size() && model == brandable.size(); ++w) {
    for (const char c : brandable[w]) {
      if (c >= '0' && c <= '9') { model = w; break; }
    }
  }
  if (model < brandable.size()) {
    d.short_name = model > 0 ? brandable[model - 1] + " " + brandable[model] : brandable[model];
  } else if (!brandable.empty()) {
    d.short_name = brandable.size() > 1 ? brandable[0] + " " + brandable[1] : brandable[0];
  } else {
    d.short_name = d.port.empty() ? "Monitor" : d.port;
  }
  // The full name without the serial and connector, for the card.
  std::string full;
  for (const auto& w : words) full += (full.empty() ? "" : " ") + w;
  if (!full.empty()) d.name = full;
}

// UI thread: SDL's display list -> g_displays (logged when it changes).
void ReadDisplays() {
  g_refresh_queued = false;
  std::vector<Display> list;
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  for (int i = 0; ids && i < count; ++i) {
    Display d;
    const char* name = SDL_GetDisplayName(ids[i]);
    d.name = name && *name ? name : "Monitor";
    NameParts(d);
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i])) {
      d.width = mode->w;
      d.height = mode->h;
      d.refresh = mode->refresh_rate;
    }
    d.primary = ids[i] == primary;
    list.push_back(std::move(d));
  }
  SDL_free(ids);
  std::lock_guard lock(g_mutex);
  bool changed = list.size() != g_displays.size();
  for (size_t i = 0; !changed && i < list.size(); ++i) {
    changed = list[i].name != g_displays[i].name || list[i].width != g_displays[i].width ||
              list[i].height != g_displays[i].height;
  }
  if (changed) {
    for (size_t i = 0; i < list.size(); ++i) {
      REXLOG_INFO("Window: monitor {} = \"{}\" ({}, {}) {}x{} {:.0f} Hz{}", i + 1, list[i].name,
                  list[i].short_name, list[i].port, list[i].width, list[i].height, list[i].refresh,
                  list[i].primary ? " (primary)" : "");
    }
  }
  g_displays = std::move(list);
}

// UI thread: the main window (the one whose title is the main window's;
// alone at Install, before the dual-mode 2nd window can exist).
SDL_Window* MainWindow() {
  if (g_main_window_id != 0) {
    if (SDL_Window* w = SDL_GetWindowFromID(g_main_window_id)) return w;
    g_main_window_id = 0;  // re-opened: look again
  }
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* found = nullptr;
  for (int i = 0; windows && i < count && !found; ++i) {
    const char* title = SDL_GetWindowTitle(windows[i]);
    if (count == 1 || (title && g_main_title == title)) found = windows[i];
  }
  SDL_free(windows);
  if (found) g_main_window_id = SDL_GetWindowID(found);
  return found;
}

// UI thread: the monitor the Borderless Window covers = the Monitor row's pick
// when it's connected, else the one the window is on.
SDL_DisplayID TargetDisplay(SDL_Window* window) {
  const int pick = int(rex::cvar::Query<int32_t>("monitor"));
  if (pick > 0) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    const SDL_DisplayID id = ids && pick <= count ? ids[pick - 1] : 0;
    SDL_free(ids);
    if (id != 0) return id;
  }
  return SDL_GetDisplayForWindow(window);
}

// UI thread: puts the main window in or out of the Borderless Window shape,
// whatever the flags say now. Runs after every mode / monitor change (queued
// after the SDK's own jobs for those flags) and once at start.
void ApplyBorderless() {
  SDL_Window* window = MainWindow();
  if (!window) return;
  const bool want = !rex::cvar::Query<bool>("fullscreen") &&
                    rex::cvar::Query<bool>("borderless_window");
  if (want) {
    // WINDOWS (2026-11-13 playtest: the mode stayed an ordinary window there;
    // fine on Linux). Made sturdier for what Windows does differently, not
    // seen on Windows yet:
    //  * a MAXIMIZED window ignores a new size / place until it's restored
    //    (and a window maximized before is restored as maximized): restored
    //    first;
    //  * leaving Fullscreen puts the old window back in steps, some after our
    //    first pass: the border and the resizable frame are taken off on
    //    every pass (not just the first), and SDL_SyncWindow waits for the
    //    system to apply what was asked;
    //  * at start the window can come after our first pass: Install checks
    //    again for a few seconds.
    // What the window really got is logged when it isn't what was asked.
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) SDL_RestoreWindow(window);
    if (!g_borderless_applied) {
      SDL_GetWindowSize(window, &g_windowed_w, &g_windowed_h);  // put back when leaving
      g_borderless_applied = true;
    }
    if (flags & SDL_WINDOW_RESIZABLE) SDL_SetWindowResizable(window, false);
    if (!(flags & SDL_WINDOW_BORDERLESS)) SDL_SetWindowBordered(window, false);
    // Placed again every time: the SDK centres the window on a Monitor change.
    SDL_Rect bounds{};
    if (!SDL_GetDisplayBounds(TargetDisplay(window), &bounds)) return;
    const int gap = int(rex::cvar::Query<int32_t>("debug_borderless_window_gap"));
    const SDL_Rect want_rect{bounds.x, bounds.y, bounds.w, std::max(1, bounds.h - gap)};
    SDL_SetWindowPosition(window, want_rect.x, want_rect.y);
    SDL_SetWindowSize(window, want_rect.w, want_rect.h);
    SDL_SyncWindow(window);  // (waits up to SDL's own limit; Wayland may still move it later)
    if (!SDL_RectsEqual(&want_rect, &g_borderless_rect)) {  // logged once per change
      g_borderless_rect = want_rect;
      REXLOG_INFO("Window: borderless window {}x{} at {},{}", want_rect.w, want_rect.h, want_rect.x,
                  want_rect.y);
    }
    SDL_Rect got{};
    SDL_GetWindowPosition(window, &got.x, &got.y);
    SDL_GetWindowSize(window, &got.w, &got.h);
    const bool bordered = !(SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS);
    static SDL_Rect s_last_got{};
    static bool s_last_bordered = false;
    if ((!SDL_RectsEqual(&got, &want_rect) || bordered) &&
        (!SDL_RectsEqual(&got, &s_last_got) || bordered != s_last_bordered)) {
      s_last_got = got;
      s_last_bordered = bordered;
      REXLOG_WARN("Window: borderless window asked {}x{} at {},{}, the window is {}x{} at {},{}{}", want_rect.w,
                  want_rect.h, want_rect.x, want_rect.y, got.w, got.h, got.x, got.y,
                  bordered ? " and still has its border" : "");
    }
  } else if (g_borderless_applied) {
    g_borderless_applied = false;
    g_borderless_rect = SDL_Rect{};
    SDL_SetWindowBordered(window, true);
    SDL_SetWindowResizable(window, true);
    // Back to the old size. Going to Fullscreen: SDL keeps it as the size the
    // window returns to when fullscreen ends (else Windowed later = monitor-sized).
    if (g_windowed_w > 0 && g_windowed_h > 0) {
      SDL_SetWindowSize(window, g_windowed_w, g_windowed_h);
      if (!rex::cvar::Query<bool>("fullscreen")) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(TargetDisplay(window)),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(TargetDisplay(window)));
      }
    }
    REXLOG_INFO("Window: borderless window off");
  }
}

}  // namespace

void Install(std::function<void(std::function<void()>)> post_to_ui, std::function<void()> on_moved,
             std::string main_title) {
  g_post = std::move(post_to_ui);
  g_on_moved = std::move(on_moved);
  // Copied on the UI thread (the only reader) through the job.
  g_post([title = std::move(main_title)] {
    g_main_title = title;
    ApplyBorderless();  // a settings file with the mode on
  });
  // ... and again while the window settles at start (it may not exist yet
  // at the first pass, or the SDK sizes it after us). Only does anything
  // with the mode on.
  std::thread([] {
    for (int ms : {500, 1000, 1500, 3000}) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      if (g_post) g_post([] { ApplyBorderless(); });
    }
  }).detach();
  // A hand-edited / console change of the flag moves the window too.
  rex::cvar::RegisterChangeCallback("borderless_window",
                                    [](std::string_view, std::string_view) { AfterChange(); });
  RefreshDisplays();
}

Mode Current() {
  if (rex::cvar::Query<bool>("fullscreen")) return Mode::kFullscreen;
  return rex::cvar::Query<bool>("borderless_window") ? Mode::kBorderlessWindow : Mode::kWindowed;
}

void Set(Mode mode, const std::function<void(const char*, const std::string&)>& set_flag) {
  // Each flag's change callback queues its own window update on the UI
  // thread, in this order: the kind of fullscreen first (always borderless;
  // only written when it was on, from an older file), then on / off.
  if (rex::cvar::Query<bool>("fullscreen_exclusive")) set_flag("fullscreen_exclusive", "false");
  // Ours first: leaving Borderless Window gives the border back before the
  // SDK's fullscreen job runs (ApplyBorderless runs again after it anyway).
  set_flag("borderless_window", mode == Mode::kBorderlessWindow ? "true" : "false");
  set_flag("fullscreen", mode == Mode::kFullscreen ? "true" : "false");
  AfterChange();
}

std::vector<Display> Displays() {
  std::lock_guard lock(g_mutex);
  return g_displays;
}

void RefreshDisplays() {
  if (!g_post || g_refresh_queued.exchange(true)) return;  // one read in flight is enough
  g_post([] { ReadDisplays(); });
}

void AfterChange() {
  if (!g_post) return;
  // Now (after the flags' own jobs, queued before this one), then twice
  // more: a Wayland compositor moves the window when it gets to it.
  auto recheck = [] {
    ApplyBorderless();  // after the SDK's own move (it centres on a Monitor change)
    ReadDisplays();
    if (g_on_moved) g_on_moved();
  };
  g_post(recheck);
  std::thread([recheck] {
    for (int ms : {500, 1000}) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      if (g_post) g_post(recheck);
    }
  }).detach();
}

}  // namespace window_mode
