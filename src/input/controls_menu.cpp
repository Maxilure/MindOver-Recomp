// =============================================================================
// input/controls_menu.cpp -- see controls_menu.h
// =============================================================================
#include "controls_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "../overlay_banner.h"
#include "../players/more_players.h"
#include "bindings.h"
#include "keyboard_mouse.h"
#include "players.h"

namespace controls_menu {
namespace {

using kbm::Action;
using kbm::Input;

// The 360 face buttons in their own colours (green A, red B, blue X,
// yellow Y), like the game's prompt icons; everything else light grey.
ImVec4 PadColour(const char* pad) {
  const std::string p = pad;
  if (p == "A") return ImVec4(0.45f, 0.85f, 0.25f, 1.0f);
  if (p == "B") return ImVec4(0.95f, 0.30f, 0.25f, 1.0f);
  if (p == "X") return ImVec4(0.30f, 0.55f, 1.00f, 1.0f);
  if (p == "Y") return ImVec4(1.00f, 0.80f, 0.15f, 1.0f);
  return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
}

class ControlsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ControlsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Toggle() {
    kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
    if (!driver) {
      overlay_banner::Show("NO KEYBOARD CONTROLS", "--keyboard_mouse=false");
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    SetOpen(driver, !open_);
  }

  bool WantsContinuousRepaint() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !driver) {
      return;
    }

    // Centred, a fixed comfortable size (the list scrolls inside).
    const ImVec2 size(std::min(760.0f, io.DisplaySize.x - 40.0f),
                      std::min(720.0f, io.DisplaySize.y - 40.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.94f);
    bool still_open = true;
    if (ImGui::Begin("Controls (F6)###crashmom_controls", &still_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      DrawContents(driver);
    }
    ImGui::End();
    if (!still_open) {
      SetOpen(driver, false);  // the window's X
    }
  }

 private:
  // mutex_ held.
  void SetOpen(kbm::KeyboardMouseDriver* driver, bool open) {
    if (open == open_) {
      return;
    }
    open_ = open;
    capturing_ = false;
    driver->CancelCapture();
    if (open) {
      edit_ = driver->bindings();  // start from what's in use
      message_.clear();
    }
    driver->SetMenuOpen(open);
  }

  // mutex_ held.
  void DrawContents(kbm::KeyboardMouseDriver* driver) {
    const float base = ImGui::GetStyle().FontSizeBase;
    ImGui::PushFont(nullptr, base * 1.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "Controls");
    ImGui::PopFont();

    // Two tabs: the keys, and who plays as which player. The bottom line
    // (Close + the status message) stays under both.
    const float bottom = ImGui::GetFrameHeightWithSpacing() + base * 0.5f;
    if (ImGui::BeginChild("##tabs", ImVec2(0.0f, -bottom))) {
      if (ImGui::BeginTabBar("##controls_tabs")) {
        if (ImGui::BeginTabItem("Keys")) {
          DrawKeysTab(driver);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Players")) {
          DrawPlayersTab(driver);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::EndChild();

    if (ImGui::Button("Close (F6)")) {
      SetOpen(driver, false);
      return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", message_.c_str());
  }

  // mutex_ held.
  void DrawKeysTab(kbm::KeyboardMouseDriver* driver) {
    const float base = ImGui::GetStyle().FontSizeBase;
    ImGui::TextWrapped(
        "Click a key to change it, then press the new key or mouse button (Esc cancels). "
        "Right-click a key to remove it. The game's button prompts show Xbox buttons: the "
        "middle column says which one each action presses. A controller works at the same "
        "time.");
    ImGui::Spacing();

    // The list scrolls; the sliders and the reset button stay.
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 3.0f + base;
    if (ImGui::BeginChild("##list", ImVec2(0.0f, -bottom))) {
      DrawTable(driver);
    }
    ImGui::EndChild();

    // Tunables: applied while dragging, saved on release.
    ImGui::Separator();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Spin speed (stick circles per second)",
                       &edit_.settings.spin_turns_per_second, 1.5f, 6.0f, "%.1f");
    if (ImGui::IsItemEdited()) driver->SetBindings(edit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) Save("Spin speed saved.");
    ImGui::SetItemTooltip(
        "How fast the Spin key turns the stick. The game counts full circles, and forgets "
        "them if the next one takes longer than a second.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Walk (how far the stick leans)", &edit_.settings.walk_tilt, 0.2f,
                       0.85f, "%.2f");
    if (ImGui::IsItemEdited()) driver->SetBindings(edit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) Save("Walk saved.");

    if (ImGui::Button("Reset keys to defaults")) {
      capturing_ = false;
      driver->CancelCapture();
      auto players = std::move(edit_.players);  // the Players tab's choices stay
      edit_ = kbm::Bindings::Defaults();
      edit_.players = std::move(players);
      driver->SetBindings(edit_);
      Save("Back to the default keys.");
    }
  }

  // mutex_ held. Who plays as which player (input/players.h).
  void DrawPlayersTab(kbm::KeyboardMouseDriver* driver) {
    kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get();
    if (!players) {
      ImGui::TextWrapped("Player assignment isn't available in this run.");
      return;
    }
    ImGui::TextWrapped(
        "Which device plays as which player. For two players: keyboard and mouse on "
        "Player 1, a controller on Player 2. In a level, press Start on the second "
        "device to join (the pause menu also offers Join Game and Drop Out). Moving a "
        "device onto a player swaps it with the device that was there. In the title "
        "and main menus every device works. Changes apply at once and are remembered.");
    ImGui::Spacing();

    const auto devices = players->Devices();  // refreshed every frame: plug in a pad any time
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##players", 2, flags)) {
      ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("Plays as", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableHeadersRow();
      for (const auto& dev : devices) {
        ImGui::TableNextRow();
        ImGui::PushID(dev.key.c_str());
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(dev.name.c_str());
        ImGui::TableSetColumnIndex(1);
        DrawPlayerCombo(driver, players, dev);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (devices.size() <= 1) {
      ImGui::Spacing();
      ImGui::TextDisabled("No controller connected. Connect one: it shows up here.");
    }
  }

  // mutex_ held.
  void DrawPlayerCombo(kbm::KeyboardMouseDriver* driver, kbm::PlayerAssignment* players,
                       const kbm::PlayerAssignment::Device& dev) {
    // Choice -> text. Automatic says what it gives right now.
    auto text = [&](int choice) -> std::string {
      if (choice == kbm::kPlayerOff) return "Off (not used)";
      if (choice == kbm::kPlayerAuto) {
        return dev.player >= 0 ? "Automatic (Player " + std::to_string(dev.player + 1) + ")"
                               : std::string("Automatic (not used)");
      }
      return "Player " + std::to_string(choice + 1);
    };
    // The keyboard has no "automatic" (it's player 1 unless moved).
    const int shown = dev.keyboard && dev.choice == kbm::kPlayerAuto ? 0 : dev.choice;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##player", text(shown).c_str())) {
      return;
    }
    // Players 1 to --local_players (levels make room for that many), plus
    // whatever is chosen now (a choice from a run with more players stays).
    std::vector<int> options;
    if (!dev.keyboard) options.push_back(kbm::kPlayerAuto);
    for (int p = 0; p < 4; ++p) {
      if (p < more_players::LocalPlayerCount() || p == shown) options.push_back(p);
    }
    options.push_back(kbm::kPlayerOff);
    for (int option : options) {
      if (ImGui::Selectable(text(option).c_str(), option == shown) && option != shown) {
        // Moving onto a player swaps with the devices there: remember them all.
        std::string message;
        for (const auto& change : players->SetChoice(dev.key, option)) {
          if (change.choice == kbm::kPlayerAuto) {
            edit_.players.erase(change.key);
          } else {
            edit_.players[change.key] = kbm::PlayerChoice{change.choice, change.name};
          }
          message += (message.empty() ? "" : " ") + change.name + ": " +
                     (change.key == dev.key ? text(option)
                                            : "Player " + std::to_string(change.choice + 1)) +
                     ".";
        }
        driver->SetBindings(edit_);
        Save(message);
      }
    }
    ImGui::EndCombo();
  }

  // mutex_ held.
  void DrawTable(kbm::KeyboardMouseDriver* driver) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##keys", 4, flags)) {
      return;
    }
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Xbox 360", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableHeadersRow();

    kbm::Group group = kbm::Group::kOther;
    for (int a = 0; a < kbm::kActionCount; ++a) {
      const Action action = Action(a);
      const kbm::ActionInfo& info = kbm::Info(action);
      if (a == 0 || info.group != group) {
        group = info.group;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "%s", kbm::GroupLabel(group));
      }
      ImGui::TableNextRow();
      ImGui::PushID(a);

      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(info.label);
      if (info.hint[0]) {
        ImGui::SetItemTooltip("%s", info.hint);
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(PadColour(info.pad), "%s", info.pad);

      for (int slot = 0; slot < kbm::kSlots; ++slot) {
        ImGui::TableSetColumnIndex(2 + slot);
        ImGui::PushID(slot);
        DrawSlot(driver, action, slot);
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // mutex_ held.
  void DrawSlot(kbm::KeyboardMouseDriver* driver, Action action, int slot) {
    const bool this_one = capturing_ && capture_action_ == action && capture_slot_ == slot;
    const Input key = edit_.keys[action][slot];
    std::string label;
    if (this_one) {
      label = "press a key...";
      // Gentle pulse so it's obvious the menu is waiting.
      const float t = float(ImGui::GetTime());
      const float glow = 0.55f + 0.25f * std::sin(t * 6.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(glow, glow * 0.6f, 0.1f, 1.0f));
    } else {
      label = key == kbm::kNoInput ? std::string("-") : kbm::InputLabel(key);
    }
    if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
      if (this_one) {
        capturing_ = false;  // clicked again: cancel
        driver->CancelCapture();
        message_.clear();
      } else {
        capturing_ = true;
        capture_action_ = action;
        capture_slot_ = slot;
        message_ = std::string("Press the new key for ") + kbm::Info(action).label + ".";
        // Called on the UI thread from the driver's key / mouse handler,
        // outside OnDraw (so taking mutex_ there is fine).
        driver->Capture([this, action, slot](Input input) { OnCaptured(action, slot, input); });
      }
    }
    if (this_one) {
      ImGui::PopStyleColor();
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && key != kbm::kNoInput) {
      edit_.Assign(action, slot, kbm::kNoInput);
      driver->SetBindings(edit_);
      Save(kbm::InputLabel(key) + " removed from " + kbm::Info(action).label + ".");
    }
  }

  // A key arrived for the slot being changed (UI thread).
  void OnCaptured(Action action, int slot, Input input) {
    kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = false;
    if (!driver || !open_) {
      return;
    }
    if (input == kbm::kNoInput) {
      message_ = "Cancelled.";
      return;
    }
    const Action previous = edit_.Assign(action, slot, input);
    driver->SetBindings(edit_);
    std::string text = kbm::InputLabel(input) + " -> " + kbm::Info(action).label;
    if (previous != kbm::kActionCount) {
      text += std::string(" (taken from ") + kbm::Info(previous).label + ")";
    }
    Save(text + ".");
  }

  // mutex_ held.
  void Save(std::string message) {
    message_ = kbm::SaveBindings(edit_, kbm::ControlsFilePath())
                   ? std::move(message)
                   : std::string("Couldn't save the controls file (see the log).");
  }

  mutable std::mutex mutex_;  // UI thread mostly; the capture callback too
  bool open_ = false;
  kbm::Bindings edit_;        // what the menu shows (and the driver uses)
  bool capturing_ = false;
  Action capture_action_ = kbm::kActionCount;
  int capture_slot_ = 0;
  std::string message_;       // the status line at the bottom
};

// Kept for the whole session (the SDK's drawer doesn't own its dialogs).
ControlsDialog* g_dialog = nullptr;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !g_dialog) {
    g_dialog = new ControlsDialog(drawer);  // registers itself with the drawer
  }
}

void Toggle() {
  if (g_dialog) {
    g_dialog->Toggle();
  }
}

}  // namespace controls_menu
