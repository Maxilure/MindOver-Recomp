// =============================================================================
// cheats/cheat_menu.cpp -- see cheat_menu.h
// =============================================================================
#include "cheat_menu.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/virtual_key.h>

#include "../input/keyboard_mouse.h"
#include "cheats.h"
#include "free_camera.h"
#include "spawn.h"

namespace cheat_menu {
namespace {

const ImVec4 kTitleColour(1.0f, 0.85f, 0.25f, 1.0f);   // the Controls menu's yellow
const ImVec4 kSectionColour(0.55f, 0.80f, 1.0f, 1.0f);  // light blue section names

// "12,500"
std::string Thousands(int value) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
  return value < 0 ? "-" + digits : digits;
}

void Section(const char* name) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(kSectionColour, "%s", name);
}

class CheatDialog : public rex::ui::ImGuiDialog {
 public:
  explicit CheatDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Toggle() {
    // Right Shift + F5 = MangoHud's logging key, not ours.
    if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
        driver && driver->InputHeld(static_cast<kbm::Input>(rex::ui::VirtualKey::kRShift))) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = !open_;
  }

  // No continuous repaint, even while open: the window is meant to stay up
  // while playing, and asking for it repainted ~1,200 times a second (measured
  // 2026-10-05). The game's own picture updates repaint it every game frame
  // (~60 a second), which keeps the numbers live; mouse and keys repaint too.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    // Top right, out of Crash's way (the game's camera keeps him centred).
    const ImVec2 size(std::min(430.0f, io.DisplaySize.x - 20.0f),
                      std::min(700.0f, io.DisplaySize.y - 20.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Appearing,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.90f);
    bool still_open = true;
    if (ImGui::Begin("Cheats (F5)###crashmom_cheats", &still_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      DrawContents();
    }
    ImGui::End();
    if (!still_open) open_ = false;
  }

 private:
  // mutex_ held.
  void DrawContents() {
    const cheats::Snapshot snap = cheats::GetSnapshot();
    ImGui::TextColored(kTitleColour, "Cheats");
    ImGui::SameLine();
    ImGui::TextDisabled("for testing (not saved by us; the game saves what they change)");
    if (!snap.in_level) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                         "Not in a level: character cheats wait until you're playing one.");
    }

    DrawCharacter(snap);
    DrawSpawn(snap);

    Section("Everyone");
    bool god = cheats::GodMode();
    if (ImGui::Checkbox("God mode", &god)) cheats::SetGodMode(god);
    ImGui::SetItemTooltip(
        "Every player's Crash and titan loses no health (and refills).\n"
        "Deaths that skip health (pits?) may still happen.");

    DrawTime();
    DrawCamera();

    Section("Screen");
    bool hide_hud = cheats::HudHidden();
    if (ImGui::Checkbox("Hide the HUD", &hide_hud)) cheats::SetHudHidden(hide_hud);
    ImGui::SetItemTooltip("Portraits, health and special bars. The mojo count text stays.");
    ImGui::Spacing();
    ImGui::TextDisabled("All mojo collected: %s", Thousands(snap.total_mojo).c_str());
    if (cheats::AchievementsBlocked()) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                         "Achievements are off until the game restarts (cheats used).");
    } else {
      ImGui::TextDisabled("Level ups, mojo, health and god mode turn achievements off");
      ImGui::TextDisabled("until the game restarts.");
    }
  }

  // mutex_ held.
  void DrawCharacter(const cheats::Snapshot& snap) {
    Section("Character");
    for (int p = 0; p < cheats::kPlayers; ++p) {
      if (p) ImGui::SameLine();
      const std::string label = "Player " + std::to_string(p + 1);
      ImGui::BeginDisabled(!snap.players[p].present);
      ImGui::RadioButton(label.c_str(), &player_, p);
      ImGui::EndDisabled();
    }
    const cheats::PlayerInfo& info = snap.players[player_];
    if (!info.present) {
      ImGui::TextDisabled("Player %d isn't in the level.", player_ + 1);
      return;
    }
    // No character name on foot: each player's look differs (Crash, Coco,
    // Carbon Crash...) and we don't read which one it is. A titan's name comes
    // from its upgrade data, so that one is right.
    if (info.titan) {
      ImGui::Text("In a titan: %s", info.name.c_str());
    } else if (info.mask) {
      ImGui::Text("Riding another player as a mask (cheats act on the shared upgrades)");
    } else {
      ImGui::Text("On foot");
    }
    if (info.max_level > 0) {
      ImGui::Text("Level: %d of %d%s", info.level, info.max_level,
                  info.fully_upgraded ? "  (fully upgraded)" : "");
    } else {
      ImGui::Text("Level: %d", info.level);
    }
    if (info.next_price > 0) {
      ImGui::Text("Mojo toward the next upgrade: %s / %s", Thousands(info.mojo).c_str(),
                  Thousands(info.next_price).c_str());
    } else {
      ImGui::Text("Mojo toward upgrades: %s", Thousands(info.mojo).c_str());
    }
    if (info.max_hitpoints > 0.0f) {
      char text[64];
      std::snprintf(text, sizeof(text), "Health %.0f / %.0f", info.hitpoints, info.max_hitpoints);
      ImGui::ProgressBar(std::clamp(info.hitpoints / info.max_hitpoints, 0.0f, 1.0f),
                         ImVec2(-1.0f, 0.0f), text);
    }

    ImGui::BeginDisabled(!snap.in_level || info.fully_upgraded);
    if (ImGui::Button("Level up")) cheats::RequestLevelUp(player_);
    ImGui::SetItemTooltip(
        info.titan ? "Buys this titan's next upgrade (with the game's upgrade screen)."
                   : "Gives Crash the mojo his level's script waits for: the game then\n"
                     "upgrades him itself (screen and all). If nothing happens, the\n"
                     "script isn't waiting for an upgrade right now.");
    ImGui::SameLine();
    if (ImGui::Button("Max level")) cheats::RequestMaxLevel(player_);
    ImGui::SetItemTooltip(info.titan ? "Buys every upgrade left at once (no screens)."
                                     : "Feeds Crash every upgrade his scripts ask for, one at a time:\n"
                                       "close each \"Level Up!\" screen (B) for the next one.");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!snap.in_level);
    for (int amount : {100, 1000, 10000}) {
      ImGui::SameLine();
      const std::string label = "+" + Thousands(amount);
      if (ImGui::Button(label.c_str())) cheats::RequestAddMojo(player_, amount);
      ImGui::SetItemTooltip("%s", ("+" + Thousands(amount) + " mojo, as if picked up").c_str());
    }
    ImGui::BeginDisabled(info.mask);
    if (ImGui::Button("Refill health")) cheats::RequestRefillHealth(player_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(info.mask);
    if (ImGui::Button("Kill")) cheats::RequestKill(player_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "In a titan: the titan dies and the player is thrown out.\n"
        "On foot: the player dies (back at the checkpoint). Works with god mode.");
    ImGui::SameLine();
    ImGui::BeginDisabled(info.mask || info.titan);
    if (ImGui::Button("Free jack")) cheats::RequestFreeJack(player_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "The game's Free Jack power-up: jack any titan (B) without beating\n"
        "it first. Lasts until the next jack. On foot only.");
    ImGui::EndDisabled();
  }

  // mutex_ held. In front of the player picked in the Character section.
  void DrawSpawn(const cheats::Snapshot& snap) {
    Section("Spawn");
    const auto& catalogue = spawn::Catalogue();
    category_ = std::clamp(category_, 0, int(catalogue.size()) - 1);
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##spawn_category", catalogue[category_].label.c_str())) {
      for (int i = 0; i < int(catalogue.size()); ++i) {
        if (ImGui::Selectable(catalogue[i].label.c_str(), i == category_)) {
          category_ = i;
          entry_ = 0;
        }
      }
      ImGui::EndCombo();
    }
    const auto& entries = catalogue[category_].entries;
    entry_ = std::clamp(entry_, 0, int(entries.size()) - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##spawn_entry", entries[entry_].label.c_str())) {
      for (int i = 0; i < int(entries.size()); ++i) {
        if (ImGui::Selectable(entries[i].label.c_str(), i == entry_)) entry_ = i;
      }
      ImGui::EndCombo();
    }
    const spawn::Entry& entry = entries[entry_];
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderInt("How many", &spawn_count_, 1, 5);
    ImGui::SameLine();
    ImGui::BeginDisabled(!entry.titan);
    ImGui::Checkbox("Ready to jack", &knocked_out_);
    ImGui::SetItemTooltip(
        "Arrives already down, ready to jack: walk up and press B.\n"
        "Off: a normal titan, ready to fight. Or use Free jack (Character).");
    ImGui::EndDisabled();
    const bool can = snap.in_level && snap.players[player_].present && !snap.players[player_].mask;
    ImGui::BeginDisabled(!can);
    const std::string button = "Spawn in front of player " + std::to_string(player_ + 1);
    if (ImGui::Button(button.c_str())) {
      spawn::Request(entry.template_name, player_, spawn_count_, entry.titan && knocked_out_);
    }
    ImGui::EndDisabled();
    // Any template by name ("Group:Name", as the game's data calls them).
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##spawn_name", "or a template, e.g. Characters:Znu", custom_,
                             sizeof(custom_));
    ImGui::SetItemTooltip(
        "Any template the game's data names (\"Group:Name\"). Experimental:\n"
        "only titans are tested; other things can crash the game.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!can || !custom_[0]);
    if (ImGui::Button("Spawn by name")) {
      spawn::Request(custom_, player_, spawn_count_, knocked_out_);
    }
    ImGui::EndDisabled();
    const std::string result = spawn::LastResult();
    if (!result.empty()) ImGui::TextDisabled("Last: %s", result.c_str());
    ImGui::TextDisabled("Not in this level? It's loaded first (takes a moment).");
  }

  // mutex_ held.
  void DrawTime() {
    Section("Time");
    float speed = cheats::GameSpeed();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Game speed", &speed, 0.1f, 4.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic)) {
      cheats::SetGameSpeed(speed);
    }
    for (float preset : {0.25f, 0.5f, 1.0f, 2.0f}) {
      ImGui::SameLine();
      char label[16];
      std::snprintf(label, sizeof(label), "%gx", preset);
      if (ImGui::SmallButton(label)) cheats::SetGameSpeed(preset);
    }
    bool frozen = cheats::Frozen();
    if (ImGui::Checkbox("Freeze", &frozen)) cheats::SetFrozen(frozen);
    ImGui::SetItemTooltip(
        "Everything stops; the picture keeps drawing (with the free camera:\n"
        "a photo mode). The game's own \"pause for screenshots\".");
    ImGui::SameLine();
    ImGui::BeginDisabled(!frozen);
    if (ImGui::Button("Step one frame")) cheats::RequestStepFrame();
    ImGui::EndDisabled();
  }

  // mutex_ held.
  void DrawCamera() {
    Section("Camera");
    bool on = free_camera::Enabled();
    if (ImGui::Checkbox("Free camera", &on)) free_camera::SetEnabled(on);
    ImGui::SameLine();
    bool keys = free_camera::KeysMoveCamera();
    ImGui::BeginDisabled(!on);
    if (ImGui::Checkbox("Keys move the camera", &keys)) free_camera::SetKeysMoveCamera(keys);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "On: the keyboard and mouse fly the camera (Crash stands still;\n"
        "a controller still plays). Off: the camera stays put and the keys\n"
        "play the game again.");
    if (on) {
      ImGui::TextDisabled("W A S D move, E / Q up / down, Shift fast, Ctrl slow,");
      ImGui::TextDisabled("right mouse button held (or arrow keys) = look around");
      float speed = free_camera::Speed();
      ImGui::SetNextItemWidth(160.0f);
      if (ImGui::SliderFloat("Flying speed", &speed, 1.0f, 200.0f, "%.0f",
                             ImGuiSliderFlags_Logarithmic)) {
        free_camera::SetSpeed(speed);
      }
      float look = free_camera::LookSpeed();
      ImGui::SetNextItemWidth(160.0f);
      if (ImGui::SliderFloat("Look speed", &look, 0.1f, 4.0f, "%.1fx")) {
        free_camera::SetLookSpeed(look);
      }
      if (ImGui::Button("Back to the game's camera")) free_camera::ResetToGameCamera();
    }
    const free_camera::Pose pose = free_camera::CurrentPose();
    ImGui::TextDisabled("Camera at (%.1f, %.1f, %.1f)", pose.position[0], pose.position[1],
                        pose.position[2]);
  }

  mutable std::mutex mutex_;  // UI thread (Toggle comes from the key handler)
  bool open_ = false;
  int player_ = 0;  // whose character the Character section acts on (and spawns go to)
  int category_ = 0, entry_ = 0, spawn_count_ = 1;
  bool knocked_out_ = true;
  char custom_[128] = {};
};

// Kept for the whole session (the SDK's drawer doesn't own its dialogs).
CheatDialog* g_dialog = nullptr;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !g_dialog) {
    g_dialog = new CheatDialog(drawer);  // registers itself with the drawer
  }
}

void Toggle() {
  if (g_dialog) g_dialog->Toggle();
}

}  // namespace cheat_menu
