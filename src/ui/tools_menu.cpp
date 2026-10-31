// =============================================================================
// ui/tools_menu.cpp -- see tools_menu.h
// =============================================================================
#include "tools_menu.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "../achievements/achievements.h"
#include "../cheats/cheats.h"
#include "../language/texts.h"
#include "../overlay_banner.h"

namespace tools_menu {
namespace {

const ImVec4 kTitleColour(1.0f, 0.85f, 0.25f, 1.0f);   // the Controls / Cheats menus' yellow
const ImVec4 kSectionColour(0.55f, 0.80f, 1.0f, 1.0f);  // light blue section names
const ImVec4 kWarnColour(1.0f, 0.55f, 0.35f, 1.0f);

// One button. `bind` = the key bind it runs (the names the SDK / the port
// register; settings.toml can give any of them a key again). `was` = the key
// it used to have, shown dimmed so long-time players find it.
struct Tool {
  const char* section;
  const char* label;
  const char* bind;  // null: `run` instead (test buttons without a key bind)
  const char* was;
  const char* hint;
  bool close_menu;  // the tool opens its own window / changes the picture: get out of its way
  void (*run)() = nullptr;
};

// In the order shown. Sections: what a player may use, then the developers'.
constexpr Tool kTools[] = {
    {trsn("Game"), trsn("Cheats..."), "bind_cheat_menu", "F5",
     trsn("Level ups, mojo, god mode, spawns, game speed, free camera.\n"
     "OPENING IT TURNS ACHIEVEMENTS OFF until a save is loaded, and a save made\n"
     "meanwhile is marked as made with cheats (achievements stay off for it)."),
     true},
    {trsn("Game"), trsn("Controls (keys)..."), "bind_controls_menu", "F6",
     trsn("Keyboard and mouse keys, and which device plays as which player."), true},
    {trsn("Game"), trsn("Hide / show the HUD"), "bind_hide_hud", "F2",
     trsn("Hides the whole HUD (portraits, bars, mojo count, combo meters, markers, hints).\n"
     "Not a cheat: achievements are untouched. The message bar keeps showing."),
     false},
    {trsn("Game"), trsn("Fullscreen on / off"), "bind_fullscreen", trsn("F11 (still F11)"),
     trsn("Borderless fullscreen on the monitor picked in Options -> Display."), false},
    {trsn("Game"), trsn("Take a photo"), "bind_photo", "F10",
     trsn("A PNG of the next frame into the photos folder (both renderers' pictures\n"
     "while the native one is drawn). The menus over the picture aren't in it."),
     true},

    {trsn("Picture"), trsn("Switch renderer (emulated / native)"), "bind_renderer", "F9",
     trsn("Which picture the main window shows: the imitated Xbox 360 GPU or the\n"
     "port's own renderer. Off with Emulated Only or while the second window is open."),
     true},
    {trsn("Picture"), trsn("Second window (dual mode)"), "bind_native_window", "F8",
     trsn("A second window with the port's picture; the main one shows the emulated one."), true},

    {trsn("Saves"), trsn("Rename the save under the cursor"), "bind_save_rename", trsn("F2 (F2 hides the HUD now)"),
     trsn("On the Load / Save Game screen only (X there opens the game's own name screen)."), true},
    {trsn("Saves"), trsn("Delete the save under the cursor"), "bind_save_delete", trsn("Delete (still Delete)"),
     trsn("On the Load / Save Game screen only (Y there does the same). The file is moved\n"
     "to user/saves/Deleted saves, never really deleted."),
     true},

    {trsn("Developer"), trsn("Debug overlay"), "bind_debug_overlay", "F3",
     trsn("The SDK's frame times and threads."), false},
    {trsn("Developer"), trsn("Log console"), "bind_console", "`",
     trsn("The SDK's live log."), false},
    {trsn("Developer"), trsn("All settings (flags)"), "bind_settings", "F4",
     trsn("Every setting by name, as in settings.toml. Prefer the in-game Options screen."), false},
    {trsn("Developer"), trsn("SDK achievements list"), "bind_achievements", "F7",
     trsn("The SDK's own list (the port keeps achievements itself: pause -> Achievements)."), false},
    {trsn("Developer"), trsn("Highlight a material (cycles)"), "bind_highlight", "F11",
     trsn("Paints one material magenta in the native picture: bump, particles, motion\n"
     "trails, off."),
     false},
    {trsn("Developer"), trsn("Particle experiment (cycles)"), "bind_particle_experiment", "F12",
     trsn("Changes how particles are drawn in the native picture, then erases groups of\n"
     "them in both pictures, then normal again."),
     false},
    {trsn("Developer"), trsn("Test: fake achievement card"), nullptr, "-",
     trsn("The achievement card on the game's message bar with a made-up name (in a level).\n"
     "Nothing is earned or recorded."),
     false, [] { achievements::ShowTestCard(false); }},
    {trsn("Developer"), trsn("Test: long fake achievement card"), nullptr, "-",
     trsn("The same with a name too long for the card: it scrolls (ui/marquee.h)."), false,
     [] { achievements::ShowTestCard(true); }},
    {trsn("Developer"), trsn("Test: game-style message (voodoo doll look)"), nullptr, "-",
     trsn("A message in the message bar's own wide look (title, message, counter), as the\n"
     "game shows a voodoo doll or an objective. Nothing else happens."),
     false, [] { achievements::ShowTestGameMessage(); }},
};

void Section(const char* name) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(kSectionColour, "%s", trs(name));
}

class ToolsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ToolsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = !open_;
  }

  // Repainted with the game's frames, mouse and keys (not ~1,200 times a
  // second: ImGuiDialog's default while open, measured for the Cheats menu).
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Tool* run = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!open_) return;
      // Top left: the Cheats menu opens top right, so both can stay up.
      ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(std::min(380.0f, io.DisplaySize.x - 20.0f), 0.0f),
                               ImGuiCond_Appearing);
      ImGui::SetNextWindowBgAlpha(0.92f);
      bool still_open = true;
      if (ImGui::Begin((std::string(trs("Tools (F1)")) + "###crashmom_tools").c_str(), &still_open,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        run = DrawContents();
      }
      ImGui::End();
      if (!still_open) open_ = false;
      if (run && run->close_menu) open_ = false;
    }
    // Outside our lock: the tool may open another window or toggle ours.
    if (run && run->run) {
      run->run();
      REXLOG_INFO("Tools: {}", run->label);
    } else if (run && !rex::ui::InvokeBind(run->bind)) {
      overlay_banner::Show(trs("NOT AVAILABLE"), trs(run->label));
      REXLOG_INFO("Tools: \"{}\" ({}) isn't available in this run", run->label, run->bind);
    } else if (run && run->bind) {
      REXLOG_INFO("Tools: {}", run->label);
    }
  }

 private:
  // mutex_ held. The tool picked this frame, or null.
  const Tool* DrawContents() {
    ImGui::TextColored(kTitleColour, "%s", trs("Tools"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", trs("F1 opens / closes   F2 = HUD   F11 = fullscreen"));
    const Tool* picked = nullptr;
    const char* section = nullptr;
    const float width = 300.0f;
    for (const Tool& tool : kTools) {
      if (!section || std::string(section) != tool.section) {
        section = tool.section;
        Section(section);
      }
      if (ImGui::Button(trs(tool.label), ImVec2(width, 0.0f))) picked = &tool;
      if (tool.bind) {
        // TR: Tooltip under each Tools button: %s = what it does, then the key, then the setting name.
        ImGui::SetItemTooltip(trs("%s\n\n(Key, or the key it used to have: %s. Setting name: %s)"), trs(tool.hint),
                              trs(tool.was), tool.bind);
      } else {
        ImGui::SetItemTooltip("%s", trs(tool.hint));
      }
      if (tool.bind && std::string(tool.bind) == "bind_cheat_menu") {
        if (cheats::AchievementsBlocked()) {
          ImGui::TextColored(kWarnColour, trs("Achievements are off: %s"), trs(cheats::BlockReason()));
        } else {
          ImGui::TextDisabled("%s", trs("Opening it turns achievements off until a save is loaded."));
        }
      }
    }
    return picked;
  }

  mutable std::mutex mutex_;  // UI thread (Toggle comes from the key handler)
  bool open_ = false;
};

// Kept for the whole session (the SDK's drawer doesn't own its dialogs).
ToolsDialog* g_dialog = nullptr;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !g_dialog) g_dialog = new ToolsDialog(drawer);  // registers itself with the drawer
}

void Toggle() {
  if (g_dialog) g_dialog->Toggle();
}

}  // namespace tools_menu
