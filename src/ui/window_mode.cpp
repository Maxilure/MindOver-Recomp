// =============================================================================
// ui/window_mode.cpp -- see window_mode.h
// =============================================================================
#include "window_mode.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace window_mode {
namespace {

std::function<void(std::function<void()>)> g_post;  // runs a job on the UI thread
std::function<void()> g_on_moved;                   // frame_rate::UpdateDisplayRefresh

std::mutex g_mutex;
std::vector<Display> g_displays;
std::atomic<bool> g_refresh_queued{false};

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

}  // namespace

void Install(std::function<void(std::function<void()>)> post_to_ui, std::function<void()> on_moved) {
  g_post = std::move(post_to_ui);
  g_on_moved = std::move(on_moved);
  RefreshDisplays();
}

Mode Current() {
  return rex::cvar::Query<bool>("fullscreen") ? Mode::kFullscreen : Mode::kWindowed;
}

void Set(Mode mode, const std::function<void(const char*, const std::string&)>& set_flag) {
  // Each flag's change callback queues its own window update on the UI
  // thread, in this order: the kind of fullscreen first (always borderless;
  // only written when it was on, from an older file), then on / off.
  if (rex::cvar::Query<bool>("fullscreen_exclusive")) set_flag("fullscreen_exclusive", "false");
  set_flag("fullscreen", mode == Mode::kWindowed ? "false" : "true");
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
