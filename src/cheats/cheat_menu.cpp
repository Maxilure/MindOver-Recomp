// =============================================================================
// cheats/cheat_menu.cpp -- see cheat_menu.h
// =============================================================================
#include "cheat_menu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/virtual_key.h>

#include "../input/keyboard_mouse.h"
#include "../players/dummy_players.h"
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
    bool opened;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = !open_;
      opened = open_;
      is_open_ = open_;
    }
    // Opening = cheats enabled (cheats.h): achievements off, the bar says so.
    if (opened) cheats::NoteCheatsUsed();
  }
  bool IsOpen() const { return is_open_.load(); }

  // No continuous repaint, even while open: the window is meant to stay up
  // while playing, and asking for it repainted ~1,200 times a second (measured
  // 2026-10-05). The game's own picture updates repaint it every game frame
  // (~60 a second), which keeps the numbers live; mouse and keys repaint too.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      // (a dummy's held stick pad direction must not outlive the window)
      for (int d = 0; d < dummy_players::kMaxDummies; ++d) dummy_players::SetHeldStick(d, 0.0f, 0.0f);
      return;
    }
    // Top right, out of Crash's way (the game's camera keeps him centred).
    const ImVec2 size(std::min(430.0f, io.DisplaySize.x - 20.0f),
                      std::min(700.0f, io.DisplaySize.y - 20.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Appearing,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.90f);
    bool still_open = true;
    if (ImGui::Begin("Cheats###crashmom_cheats", &still_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      DrawContents();
    }
    ImGui::End();
    if (!still_open) open_ = false;
    is_open_ = open_;
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
    DrawDummies();

    Section("Everyone");
    bool god = cheats::GodMode();
    if (ImGui::Checkbox("God mode", &god)) cheats::SetGodMode(god);
    ImGui::SetItemTooltip(
        "Every player's Crash and titan loses no health (and refills).\n"
        "Deaths that skip health (pits?) may still happen.");
    bool no_ai = cheats::NoAi();
    if (ImGui::Checkbox("No AI", &no_ai)) cheats::SetNoAi(no_ai);
    ImGui::SetItemTooltip("Enemies and enemy titans stop thinking: no attacks, no moving.\n"
                          "They can still be hit and beaten.");

    DrawUnlocks(snap);
    DrawTime();
    DrawCamera();

    Section("Screen");
    bool hide_hud = cheats::HudHidden();
    if (ImGui::Checkbox("Hide the HUD", &hide_hud)) cheats::SetHudHidden(hide_hud);
    ImGui::SetItemTooltip("Portraits, health and special bars. The mojo count text stays.");
    ImGui::Spacing();
    ImGui::TextDisabled("All mojo collected: %s", Thousands(snap.total_mojo).c_str());
    // (Opening this window enabled cheats: cheats.h AchievementsBlocked.)
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Achievements are off: %s.",
                       cheats::BlockReason());
    ImGui::TextDisabled("Loading a save turns them back on (with this window closed and the");
    ImGui::TextDisabled("switches off). A save made now is marked as made with cheats.");
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
    if (info.mojo_upgrades_done) {
      ImGui::Text("Every mojo upgrade bought: the rest come with the story");
    } else if (info.next_price > 0) {
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

    ImGui::BeginDisabled(!snap.in_level || info.fully_upgraded || info.mojo_upgrades_done);
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
                                       "close each \"Level Up!\" screen (B) for the next one.\n"
                                       "Mojo buys 18 of them (the last at 220,000); the others\n"
                                       "of the 32 are unlocked by the story.");
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

  // mutex_ held. The game's unlock lists (cheats.h, UNLOCKS).
  void DrawUnlocks(const cheats::Snapshot& snap) {
    Section("Unlocks");
    ImGui::BeginDisabled(!snap.in_level);
    constexpr int kKinds = int(cheats::UnlockKind::kCount);
    for (int k = 0; k < kKinds; ++k) {
      const auto kind = static_cast<cheats::UnlockKind>(k);
      if (k) ImGui::SameLine();
      // "Outfits 4/11"; the ## part keeps the id steady while the count changes.
      const std::string label = std::string(cheats::UnlockName(kind)) + " " +
                                std::to_string(snap.unlocked[k]) + "/" +
                                std::to_string(snap.unlock_total[k]) + "##unlock" +
                                std::to_string(k);
      if (ImGui::Button(label.c_str())) cheats::RequestUnlockAll(kind);
      ImGui::SetItemTooltip("Unlocked / all. Click to unlock all of them.");
    }
    if (ImGui::Button("Unlock everything")) {
      for (int k = 0; k < kKinds; ++k) cheats::RequestUnlockAll(static_cast<cheats::UnlockKind>(k));
    }
    ImGui::SetItemTooltip(
        "Every outfit (Coco and the titan suits included: pick them at the chest),\n"
        "concept art, enemy info and tutorial page, in the game being played.\n"
        "Kept when the game saves. Nobody's outfit changes by itself.");
    ImGui::EndDisabled();
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

  // DUMMY PLAYERS (players/dummy_players.h): the fake controllers, half-steered.
  // mutex_ held.
  void DrawDummies() {
    Section("Dummy players");
    const int count = dummy_players::Count();
    if (count <= 0) {
      ImGui::TextDisabled("No dummy controllers. Add  debug_fake_pads = 3  to user/settings.toml");
      ImGui::TextDisabled("(1-3 dummies) and restart the game.");
      return;
    }
    ImGui::TextDisabled("START joins, B leaves a mask. Hold a direction on the pad to walk.");
    static const char* kModes[] = {"Idle (buttons only)", "Follow player 1", "Follow player 2", "Follow player 3",
                                   "Follow player 4",     "Copy player 1",   "Copy player 2",   "Copy player 3",
                                   "Copy player 4"};
    for (int d = 0; d < count; ++d) {
      ImGui::PushID(d);
      const int player = dummy_players::PlayerOf(d);
      if (player >= 0) {
        ImGui::Text("Dummy controller %d: player %d", d + 2, player + 1);
      } else {
        ImGui::Text("Dummy controller %d: plays as nobody (Controls -> Players)", d + 2);
      }
      // Mode: 0 idle, 1-4 follow player 1-4, 5-8 copy player 1-4.
      const dummy_players::Mode mode = dummy_players::GetMode(d);
      int item = mode == dummy_players::Mode::kIdle ? 0
                 : mode == dummy_players::Mode::kFollow ? 1 + dummy_players::GetTarget(d)
                                                        : 5 + dummy_players::GetTarget(d);
      ImGui::SetNextItemWidth(170.0f);
      if (ImGui::Combo("##mode", &item, kModes, IM_ARRAYSIZE(kModes))) {
        if (item == 0) dummy_players::SetMode(d, dummy_players::Mode::kIdle, 0);
        else if (item <= 4) dummy_players::SetMode(d, dummy_players::Mode::kFollow, item - 1);
        else dummy_players::SetMode(d, dummy_players::Mode::kCopy, item - 5);
      }
      ImGui::SetItemTooltip(
          "Follow: walks after that player, stops ~3 units away, jumps when stuck\n"
          "below them. Copy: repeats that player's controller (not START / BACK).\n"
          "The buttons and the stick pad work in every mode.");
      // Which controller it pretends to be (the Automatic button pictures).
      ImGui::SameLine();
      int model = dummy_players::GetModel(d);
      ImGui::SetNextItemWidth(190.0f);
      if (ImGui::BeginCombo("##model", dummy_players::ModelName(model))) {
        for (int m = 0; m < dummy_players::ModelCount(); ++m) {
          if (ImGui::Selectable(dummy_players::ModelName(m), m == model)) dummy_players::SetModel(d, m);
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip(
          "Which controller this dummy pretends to be. With Controller Icons on\n"
          "Automatic, the game detects it from its maker and model like a real pad\n"
          "and shows that player's prompts in its button pictures.");
      // Buttons: a tap each (150 ms; START a little longer).
      static const char* kButtons[][2] = {{"START", "start"}, {"A", "a"}, {"B", "b"}, {"X", "x"},
                                          {"Y", "y"},         {"LB", "lb"}, {"RB", "rb"}, {"LT", "lt"},
                                          {"RT", "rt"},       {"BACK", "back"}};
      for (size_t i = 0; i < IM_ARRAYSIZE(kButtons); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::SmallButton(kButtons[i][0])) {
          dummy_players::Tap(d, kButtons[i][1], i == 0 ? 300 : 150);
        }
      }
      // The stick pad: 3 x 3, held = walk that way (up = away from the camera).
      float hx = 0.0f, hy = 0.0f;
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          if (col) ImGui::SameLine();
          ImGui::PushID(row * 3 + col);
          const bool centre = row == 1 && col == 1;
          static const char* kArrows[3][3] = {{"\\", "^", "/"}, {"<", " ", ">"}, {"/", "v", "\\"}};
          ImGui::BeginDisabled(centre);
          ImGui::Button(kArrows[row][col], ImVec2(26.0f, 22.0f));
          ImGui::EndDisabled();
          if (!centre && ImGui::IsItemActive()) {
            hx = float(col - 1);
            hy = float(1 - row);
          }
          ImGui::PopID();
        }
      }
      if (hx != 0.0f && hy != 0.0f) {  // diagonals: full tilt, not more
        hx *= 0.7071f;
        hy *= 0.7071f;
      }
      dummy_players::SetHeldStick(d, hx, hy);
      ImGui::PopID();
      ImGui::Spacing();
    }
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
  std::atomic<bool> is_open_{false};  // open_ for other threads
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

bool IsOpen() { return g_dialog && g_dialog->IsOpen(); }

}  // namespace cheat_menu
