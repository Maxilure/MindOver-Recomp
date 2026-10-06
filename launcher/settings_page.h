// =============================================================================
// settings_page.h -- the launcher's Settings tab
// =============================================================================
//
// THE MAIN SETTINGS, with plain names and the right control for each:
//   Frame rate   fps cap: 30 (original) / 60 / 120 / 144 / 165 / 180 / 240 /
//                unlimited / any other number
//   Picture      which renderer draws: emulated / native / native only /
//                both (two windows) / emulated only (4 flags behind one choice)
//   Window       fullscreen
//   Co-op        players (2-4), co-op camera + its two knobs, lost controller
//   Controls     keyboard + mouse (the save list has no switch: it stays on)
//   Gameplay     the ground grace above 30 fps
//   Sound, Logging
// and at the bottom ALL SETTINGS (advanced): every one of the game's ~200
// flags, by category, searchable, each with a control picked from its type
// (tick box, number with its range, list of allowed values, text).
//
// WARNINGS: a yellow triangle beside a value that's risky (hover it for why):
// above 60 fps, the native-only picture, 3-4 players... Risky = untested or
// known to misbehave; the player can still pick it. The rules live in one
// place, WarningFor() in settings_page.cpp.
//
// Every change is written to user/settings.toml AT ONCE (settings.h) and
// applies the next time the game starts. A changed value has a "Default"
// button that puts it back (= removes it from the file).
// =============================================================================
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include "settings.h"

namespace settings_page {

struct Look {
  ImFont* bold = nullptr;
  ImVec4 muted, warn, bad, accent;
};

class Page {
 public:
  Page(std::filesystem::path exe, std::filesystem::path game_folder,
       std::filesystem::path user_folder);
  ~Page();

  // Draws the tab. `game_running` = changes wait for the next start.
  void Draw(const Look& look, bool game_running);

  // Before the game starts: drop the session-only keys (settings.h).
  void BeforeLaunch();

 private:
  void DrawMain();
  void DrawAll();
  void Row(const char* label, const char* help);  // starts a row (table)
  bool Status(const std::string& name);            // warning + Default (ends a row)
  void SetValue(const std::string& name, const std::string& value);
  std::string Value(const std::string& name) const;
  void SettingControl(const settings::Setting& setting);  // the All settings widgets

  std::filesystem::path exe_, game_folder_;
  settings::Values values_;
  settings::Catalogue catalogue_;
  std::thread loader_;
  std::atomic<bool> loaded_{false};
  std::string load_error_;          // written by the loader before loaded_
  std::string write_error_;
  const Look* look_ = nullptr;
  char search_[96] = {};
  bool show_debug_ = false;
};

}  // namespace settings_page
