// =============================================================================
// main.cpp -- the launcher's window and its Play page
// =============================================================================
//
// WHAT THE PLAYER SEES (one page for now; setup, settings and updates come
// later, notes in docs/01-building.md):
//
//   Crash: Mind over Mutant                    PC port (alpha)
//   [ Play ]                 from the start: intro movies, title, menus
//   [ Continue "<save>" ]    straight into the most recently played save
//   Your saves               every save, most recently played first, each
//                            with its own Play button (or double-click it)
//   Open: saves / photos / logs / settings file
//
// While the game runs the page shows how long it has been running and a
// Stop button; when it ends, a short report: closed normally, or CRASHED
// (with the signal), the log's [error]/[warning] counts and buttons to open
// the log. The game itself is started by game_process.h; saves are read by
// saves.h; the folders found by folders.h.
//
// HOW THE GAME IS STARTED: <exe> --game_data_root=<game folder>/game, plus
// --load_save=<N> for a save (src/saves/quick_load.h: no movies, title or
// menus, gameplay in ~20 s). Nothing else: every setting comes from
// user/settings.toml as usual (command-line flags would win over it, and the
// game's F4 menu would write them into it).
//
// DRAWING: SDL3's 2D renderer + ImGui (the same UI library as the game's F5/F6
// menus). The window only redraws when something happens (input, or twice a
// 4 times a second while the game runs: its clock and the live log), so it costs ~nothing while
// waiting next to the game.
//
// FOUR TABS: Play (above), Settings (settings_page.h: the main settings with
// warnings on risky values, plus all ~200 of the game's flags), Setup
// (setup.h: build tools listed, disc extraction, SDK + game builds; the
// launcher opens on it while the game can't be played yet) and Game log: the session's log LIVE while the game
// runs (and the last session's after), merged with what the game printed to
// the terminal; errors red, warnings yellow; a filter box, "errors and
// warnings only", and it follows new lines while scrolled to the bottom.
// (game_process.h names the log file: user/logs/play-<date>_<time>.log.)
//
// Mouse and keyboard only: no controller navigation (a launcher is used for
// a few clicks; pad presses meant for the game must never reach it anyway).
//
// Command line: --game_folder=<path> (else found next to / above the launcher);
// --play=new|last|<save number>: start the game right away, as if Play /
// Continue / a save's Play was pressed (for a desktop shortcut, and tests);
// --tab=settings / --tab=log / --tab=setup: open on that tab;
// --setup_run=check|source|disc|sdk|game|all|update [--iso=<file>]: a Setup step, no window;
// --update_feed=<url>: where updates are looked for (update.h; tests: a file:// URL);
// --after_update: started by an update's restart (opens Setup, rebuilds);
// --desktop_entry=add|remove: the applications-menu entry (desktop_entry.h), no window;
// --screenshot=<file.png> [--screenshot_after=<seconds>]: draw the page, save
// it as a picture and quit (for checking the look and pictures for the docs
// without anyone at the screen; the game, if started, keeps running).
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // Windows: SDL provides WinMain and calls main below
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "desktop_entry.h"
#include "folders.h"
#include "platform.h"
#include "game_process.h"
#include "saves.h"
#include "settings_page.h"
#include "setup.h"
#include "update.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// -----------------------------------------------------------------------------
// Look
// -----------------------------------------------------------------------------

// Colours: dark, with Crash's orange as the accent.
const ImVec4 kOrange{0.96f, 0.55f, 0.13f, 1.00f};
const ImVec4 kOrangeHover{1.00f, 0.65f, 0.25f, 1.00f};
const ImVec4 kOrangeActive{0.85f, 0.45f, 0.08f, 1.00f};
const ImVec4 kMuted{0.62f, 0.64f, 0.70f, 1.00f};
const ImVec4 kGood{0.45f, 0.82f, 0.45f, 1.00f};
const ImVec4 kBad{1.00f, 0.42f, 0.38f, 1.00f};
const ImVec4 kWarn{1.00f, 0.80f, 0.35f, 1.00f};

struct Fonts {
  ImFont* regular = nullptr;
  ImFont* bold = nullptr;
  ImFont* mono = nullptr;  // the Game log's lines
};

Fonts LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  Fonts fonts;
  // The desktop's fonts (Linux: fontconfig's sans / monospace, e.g. Noto
  // Sans; Windows: Segoe UI / Consolas); ImGui's own small pixel font if none.
  const platform::FontFiles files = platform::SystemFonts();
  const std::string regular = files.regular.string();
  const std::string bold = files.bold.string();
  if (!regular.empty()) {
    fonts.regular = io.Fonts->AddFontFromFileTTF(regular.c_str(), 17.0f);
  }
  if (!fonts.regular) {
    fonts.regular = io.Fonts->AddFontDefault();
  }
  if (!bold.empty()) {
    fonts.bold = io.Fonts->AddFontFromFileTTF(bold.c_str(), 17.0f);
  }
  if (!fonts.bold) {
    fonts.bold = fonts.regular;
  }
  const std::string mono = files.mono.string();
  if (!mono.empty()) {
    fonts.mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), 15.0f);
  }
  if (!fonts.mono) {
    fonts.mono = fonts.regular;
  }
  return fonts;
}

void ApplyStyle(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontSizeBase = 17.0f;  // unscaled; FontScaleDpi below scales it
  style.WindowPadding = {28, 24};
  style.FramePadding = {12, 7};
  style.ItemSpacing = {10, 9};
  style.CellPadding = {10, 2};
  style.FrameRounding = 6;
  style.ChildRounding = 8;
  style.PopupRounding = 8;
  style.GrabRounding = 6;
  style.WindowBorderSize = 0;
  style.ChildBorderSize = 1;
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = {0.09f, 0.09f, 0.11f, 1.0f};
  c[ImGuiCol_ChildBg] = {0.12f, 0.12f, 0.15f, 1.0f};
  c[ImGuiCol_PopupBg] = {0.13f, 0.13f, 0.16f, 1.0f};
  c[ImGuiCol_Border] = {0.22f, 0.22f, 0.27f, 1.0f};
  c[ImGuiCol_FrameBg] = {0.17f, 0.17f, 0.21f, 1.0f};
  c[ImGuiCol_FrameBgHovered] = {0.22f, 0.22f, 0.27f, 1.0f};
  c[ImGuiCol_FrameBgActive] = {0.25f, 0.25f, 0.31f, 1.0f};
  c[ImGuiCol_Button] = {0.20f, 0.20f, 0.25f, 1.0f};
  c[ImGuiCol_ButtonHovered] = {0.27f, 0.27f, 0.33f, 1.0f};
  c[ImGuiCol_ButtonActive] = {0.32f, 0.32f, 0.39f, 1.0f};
  c[ImGuiCol_Header] = {0.96f, 0.55f, 0.13f, 0.22f};
  c[ImGuiCol_HeaderHovered] = {0.96f, 0.55f, 0.13f, 0.32f};
  c[ImGuiCol_HeaderActive] = {0.96f, 0.55f, 0.13f, 0.45f};
  c[ImGuiCol_TableHeaderBg] = {0.15f, 0.15f, 0.19f, 1.0f};
  c[ImGuiCol_TableRowBgAlt] = {1.0f, 1.0f, 1.0f, 0.025f};
  c[ImGuiCol_NavCursor] = kOrange;
  c[ImGuiCol_CheckMark] = kOrange;
  c[ImGuiCol_TextSelectedBg] = {0.96f, 0.55f, 0.13f, 0.35f};
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
}

// A big orange button (Play / Continue).
bool AccentButton(const char* label, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_Button, kOrange);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOrangeHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kOrangeActive);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.08f, 0.06f, 0.04f, 1.0f});
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void MutedText(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------------
// Text helpers
// -----------------------------------------------------------------------------

const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// "1 h 05 min" / "12 min" / "40 s".
std::string Duration(double seconds) {
  const long total = long(seconds);
  char text[48];
  if (total >= 3600) {
    std::snprintf(text, sizeof(text), "%ld h %02ld min", total / 3600, total / 60 % 60);
  } else if (total >= 60) {
    std::snprintf(text, sizeof(text), "%ld min", total / 60);
  } else {
    std::snprintf(text, sizeof(text), "%ld s", total);
  }
  return text;
}

// The save's own "saved at" stamp: "Oct 4, 11:50".
std::string SavedAt(const saves::Save& save) {
  if (save.month < 1 || save.month > 12) {
    return "?";
  }
  char text[48];
  std::snprintf(text, sizeof(text), "%s %d, %02d:%02d", kMonths[save.month - 1], save.day,
                save.hour, save.minute);
  return text;
}

// When a save was last played, in words: "today", "yesterday", "3 days ago",
// "Oct 4" (file-clock ticks, saves.h).
std::string PlayedWhen(int64_t ticks) {
  using namespace std::chrono;
  const auto file_time = fs::file_time_type(fs::file_time_type::duration(ticks));
  const auto when = clock_cast<system_clock>(file_time);
  const std::time_t then_t = system_clock::to_time_t(when);
  const std::time_t now_t = system_clock::to_time_t(system_clock::now());
  const std::tm then = platform::LocalTime(then_t), now = platform::LocalTime(now_t);
  // Calendar days between the two (midnight to midnight, local time).
  std::tm then_day = then, now_day = now;
  then_day.tm_hour = now_day.tm_hour = 12;
  then_day.tm_min = now_day.tm_min = then_day.tm_sec = now_day.tm_sec = 0;
  const long days = long(std::difftime(std::mktime(&now_day), std::mktime(&then_day)) / 86400.0 + 0.5);
  char text[48];
  if (days <= 0) {
    std::snprintf(text, sizeof(text), "today %02d:%02d", then.tm_hour, then.tm_min);
  } else if (days == 1) {
    std::snprintf(text, sizeof(text), "yesterday %02d:%02d", then.tm_hour, then.tm_min);
  } else if (days < 7) {
    std::snprintf(text, sizeof(text), "%ld days ago", days);
  } else {
    std::snprintf(text, sizeof(text), "%s %d", kMonths[then.tm_mon], then.tm_mday);
  }
  return text;
}

// Opens a folder or file with the desktop's default program (Linux:
// xdg-open, Windows: the shell), through SDL.
void OpenPath(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (path.has_extension()) {
      return;  // a file that isn't there: nothing to open
    }
    fs::create_directories(path, ec);  // a folder the game hasn't made yet
  }
#if defined(_WIN32)
  SDL_OpenURL(("file:///" + path.generic_string()).c_str());  // file:///D:/CrashMoM/...
#else
  SDL_OpenURL(("file://" + path.string()).c_str());
#endif
}

// -----------------------------------------------------------------------------
// The page
// -----------------------------------------------------------------------------

struct Launcher {
  folders::Folders folders;
  Fonts fonts;
  // Lives as long as the process (never deleted): game_process's force-stop
  // timer thread may still hold it while the launcher closes.
  game_process::Game* game = new game_process::Game;
  std::vector<saves::Save> save_list;
  Clock::time_point saves_read{};
  std::string start_error;
  bool other_game_running = false;
  Clock::time_point other_checked{};
  char filter[64] = {};
  int selected = -1;  // index into save_list
  bool confirm_stop = false;
  int select_tab = -1;  // 0 Play, 1 Settings, 2 Game log, 3 Setup: switch to it next frame
  std::unique_ptr<setup::Setup> setup;
  std::unique_ptr<settings_page::Page> settings;  // made once the game folder is known
  fs::path launcher_path;                         // this program (for the menu entry)
  desktop_entry::Status desktop = desktop_entry::Status::kMissing;
  std::string desktop_error;

  // The Game log tab's copy of the log (game_process.h) and its filter.
  game_process::LogView log;
  char log_filter[96] = {};
  bool log_problems_only = false;
  std::vector<int> log_shown;       // indices into log.lines that pass the filter
  uint64_t log_shown_version = ~0ull;
  std::string log_shown_filter;
  bool log_shown_problems = false;
  bool log_follow = true;           // keep scrolled to the newest line

  void RefreshSaves(bool force) {
    const auto now = Clock::now();
    if (force || now - saves_read > std::chrono::seconds(5)) {
      save_list = saves::List(folders.user);
      saves_read = now;
      if (selected >= int(save_list.size())) {
        selected = -1;
      }
    }
  }

  void RefreshOtherGame() {
    const auto now = Clock::now();
    if (now - other_checked > std::chrono::seconds(2)) {
      other_game_running = game_process::AnotherGameRunning();
      desktop = desktop_entry::Check(launcher_path);
      other_checked = now;
    }
  }

  // Starts the game: from the start (save_number 0) or straight into a save.
  void Play(int save_number) {
    std::vector<std::string> args;
    args.push_back("--game_data_root=" + folders.disc.string());
    if (save_number > 0) {
      args.push_back("--load_save=" + std::to_string(save_number));
    }
    start_error.clear();
    if (settings) {
      settings->BeforeLaunch();  // no leftover load_save / log_file (settings.h)
    }
    if (!game->Start(folders.exe, args, folders.root, folders.user, &start_error)) {
      return;
    }
  }

  void Draw();
  void DrawHeader();
  bool DrawProblems();  // true = the game can't be started
  void DrawRunning();
  void DrawReport();
  void DrawPlay();
  void DrawSaves();
  void DrawFolders();
  void DrawPlayTab(game_process::State state);
  void DrawLogTab();
};

void Launcher::DrawHeader() {
  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.9f);
  ImGui::TextUnformatted("Mind over Recomp");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FontSizeBase * 0.65f);
  ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
  static const std::string version = update::InstalledVersion(folders);
  ImGui::Text("%s", version.empty() ? "(alpha)" : version.c_str());
  ImGui::PopStyleColor();
  // The port is named after the project; the game's name only says what it
  // is a port of (it's not an official product).
  ImGui::TextDisabled("An unofficial PC port of Crash: Mind over Mutant");
  ImGui::Spacing();
}

bool Launcher::DrawProblems() {
  auto problem = [&](const char* title, const std::string& detail) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::PushFont(fonts.bold, 0.0f);
    ImGui::TextWrapped("%s", title);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    MutedText(detail.c_str());
    ImGui::Spacing();
  };
  if (folders.root.empty()) {
    problem("Couldn't find the game.",
            "Put the launcher in the game's folder (next to program/), or start it with "
            "--game_folder=<path to the game folder>.");
    return true;
  }
  bool blocked = false;
  if ((!folders.ExeExists() || !folders.DiscExists()) && setup) {
    ImGui::TextUnformatted("The game isn't ready to play yet.");
    if (ImGui::Button("Open Setup")) {
      select_tab = 3;
    }
    ImGui::Spacing();
  }
  if (!folders.ExeExists()) {
    problem("The game isn't built yet.",
            "Expected at " + folders::Pretty(folders.exe) +
                ". Build it first (docs/01-building.md, steps 2 to 4).");
    blocked = true;
  }
  if (!folders.DiscExists()) {
    problem("The disc's files aren't there.",
            "Expected in " + folders::Pretty(folders.disc) +
                " (default.xex and the rest). Extract your disc image first "
                "(docs/01-building.md, step 1).");
    blocked = true;
  }
  return blocked;
}

void Launcher::DrawRunning() {
  const auto elapsed = std::chrono::duration<double>(Clock::now() - game->started()).count();
  ImGui::BeginChild("running", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.35f);
  ImGui::TextColored(kGood, "Playing");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FontSizeBase * 0.2f);
  ImGui::TextColored(kMuted, "for %s", Duration(elapsed).c_str());
  MutedText("The game is running in its own window. This window can stay open: it shows how the "
            "session ended when the game closes.");
  if (ImGui::Button("Watch the game log")) {
    select_tab = 2;
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop the game...")) {
    confirm_stop = true;
    ImGui::OpenPopup("Stop the game?");
  }
  if (ImGui::BeginPopupModal("Stop the game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Progress since your last save is lost.");
    ImGui::Spacing();
    if (ImGui::Button("Stop it")) {
      game->Stop();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep playing") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::EndChild();
}

void Launcher::DrawReport() {
  const game_process::Result result = game->result();
  ImGui::BeginChild("report", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.2f);
  const std::string played = Duration(double(result.played.count()));
  if (result.crashed) {
    ImGui::TextColored(kBad, "The game crashed after %s", played.c_str());
  } else if (result.killed) {
    ImGui::TextColored(kWarn, "The game was ended from outside after %s", played.c_str());
  } else if (result.stopped_by_launcher) {
    ImGui::TextColored(kWarn, "The game was stopped after %s", played.c_str());
  } else if (result.exit_code != 0) {
    ImGui::TextColored(kWarn, "The game quit with an error (code %d) after %s", result.exit_code,
                       played.c_str());
  } else {
    ImGui::TextColored(kGood, "The game closed normally after %s", played.c_str());
  }
  ImGui::PopFont();
  if (result.crashed || result.killed) {
    // (Linux SIGKILL = 9: also what the system does when memory runs out.)
    ImGui::TextColored(kMuted, "Ended by %s.%s", result.ended_by.c_str(),
                       result.killed && result.raw == 9
                           ? " (Also what the system does when memory runs out.)"
                           : "");
  }
  if (!result.log.empty()) {
    ImGui::TextColored(kMuted, "Log: %s", folders::Pretty(result.log).c_str());
    if (log.errors || log.warnings) {
      ImGui::SameLine();
      ImGui::TextColored(log.errors ? kBad : kWarn,
                         " %d error%s (%d different), %d warning%s (%d different)", log.errors,
                         log.errors == 1 ? "" : "s", log.error_kinds, log.warnings,
                         log.warnings == 1 ? "" : "s", log.warning_kinds);
    }
  }
  if (!result.fps.empty()) {
    ImGui::TextColored(kMuted, "Frame rate: %s", result.fps.c_str());
  }
  ImGui::Spacing();
  if (!result.log.empty() && ImGui::Button("Open the log")) {
    OpenPath(result.log);
  }
  ImGui::SameLine();
  if (ImGui::Button("Logs folder")) {
    OpenPath(folders.user / "logs");
  }
  ImGui::SameLine();
  if (ImGui::Button("Show the game log")) {
    select_tab = 2;
  }
  ImGui::SameLine();
  if (ImGui::Button("OK")) {
    game->Forget();
  }
  ImGui::EndChild();
  ImGui::Spacing();
}

void Launcher::DrawPlay() {
  const float big = ImGui::GetFrameHeight() * 1.9f;
  const float width = ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleDpi * 15.0f;

  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.25f);
  const bool play = AccentButton("Play", {width, big});
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + big * 0.12f);
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted("From the start");
  ImGui::PopFont();
  MutedText("Intro movies (Esc or Start skips them), the title screen and the menus.");
  ImGui::EndGroup();
  if (play) {
    Play(0);
  }

  if (!save_list.empty()) {
    const saves::Save& last = save_list.front();
    ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.25f);
    const bool resume = AccentButton("Continue", {width, big});
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + big * 0.12f);
    ImGui::PushFont(fonts.bold, 0.0f);
    ImGui::Text("\"%s\"", last.name.c_str());
    ImGui::PopFont();
    char details[160];
    std::snprintf(details, sizeof(details),
                  "%d%%  \u00b7  %s  \u00b7  played %s. Straight in: no movies or menus.",
                  last.percent, Duration(last.play_seconds).c_str(),
                  PlayedWhen(last.played).c_str());
    MutedText(details);
    ImGui::EndGroup();
    if (resume) {
      Play(last.number);
    }
  }
  if (!start_error.empty()) {
    ImGui::TextColored(kBad, "%s", start_error.c_str());
  }
  ImGui::Spacing();
}

void Launcher::DrawSaves() {
  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.1f);
  ImGui::Text("Your saves");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "(%zu)", save_list.size());
  if (save_list.empty()) {
    MutedText("No saves yet. Start a New Game from the main menu: it makes its own save.");
    return;
  }
  ImGui::SameLine();
  const float filter_width = ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleDpi * 14;
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - filter_width);
  ImGui::SetNextItemWidth(filter_width);
  ImGui::InputTextWithHint("##filter", "Find a save...", filter, sizeof(filter));

  // Leave room under the table for the folder buttons.
  const float below = ImGui::GetFrameHeightWithSpacing() * 1.6f;
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("saves", 6, flags, {0, -below})) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 1.1f);
  ImGui::TableSetupColumn("Play time", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("Last played", ImGuiTableColumnFlags_WidthStretch, 1.8f);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableHeadersRow();

  // Case-insensitive "contains" for the filter box.
  auto matches = [&](const std::string& name) {
    if (!filter[0]) {
      return true;
    }
    auto lower = [](std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
      return s;
    };
    return lower(name).find(lower(filter)) != std::string::npos;
  };

  int play_number = 0;
  for (int i = 0; i < int(save_list.size()); ++i) {
    const saves::Save& save = save_list[i];
    if (!matches(save.name)) {
      continue;
    }
    ImGui::PushID(save.number);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    // The whole row is one selectable: click selects, double-click plays.
    const bool picked = ImGui::Selectable(save.name.empty() ? "(no name)" : save.name.c_str(),
                                          selected == i,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowOverlap |
                                              ImGuiSelectableFlags_AllowDoubleClick,
                                          {0, ImGui::GetFrameHeight() * 0.8f});
    if (picked) {
      selected = i;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        play_number = save.number;
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Save file %d, saved %s", save.number, SavedAt(save).c_str());
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d%%", save.percent);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Duration(save.play_seconds).c_str());
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(saves::DifficultyName(save.difficulty));
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "%s", PlayedWhen(save.played).c_str());
    ImGui::TableNextColumn();
    if (ImGui::SmallButton("Play")) {
      play_number = save.number;
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
  if (play_number > 0) {
    Play(play_number);
  }
}

void Launcher::DrawFolders() {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kMuted, "Open:");
  ImGui::SameLine();
  if (ImGui::Button("Saves")) {
    OpenPath(folders.user / "saves");
  }
  ImGui::SameLine();
  if (ImGui::Button("Photos (F10)")) {
    OpenPath(folders.user / "photos");
  }
  ImGui::SameLine();
  if (ImGui::Button("Logs")) {
    OpenPath(folders.user / "logs");
  }
  ImGui::SameLine();
  if (ImGui::Button("Settings file")) {
    OpenPath(folders.user / "settings.toml");
  }
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "  %s", folders::Pretty(folders.user).c_str());

  // The applications menu entry (desktop_entry.h), at the row's right end.
  const char* label = desktop == desktop_entry::Status::kMissing    ? "Add to the applications menu"
                      : desktop == desktop_entry::Status::kOutdated ? "Update the menu entry"
                                                                    : "Remove from the menu";
  const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
  const float right = ImGui::GetWindowContentRegionMax().x - width;
  ImGui::SameLine(std::max(right, ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x));
  if (ImGui::Button(label)) {
    std::string error;
    const bool ok = desktop == desktop_entry::Status::kCurrent
                        ? desktop_entry::Remove(&error)
                        : desktop_entry::Write(launcher_path, folders.root, &error);
    desktop_error = ok ? std::string() : error;
    desktop = desktop_entry::Check(launcher_path);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("%s\n%s",
                      desktop == desktop_entry::Status::kCurrent
                          ? "The launcher is in your applications menu (right-click it there for "
                            "\"Continue last save\")."
                          : "Puts the launcher in your desktop's applications menu, with a "
                            "\"Continue last save\" shortcut on right-click.",
                      folders::Pretty(desktop_entry::EntryPath()).c_str());
  }
  if (!desktop_error.empty()) {
    ImGui::TextColored(kBad, "%s", desktop_error.c_str());
  }
}

void Launcher::DrawPlayTab(game_process::State state) {
  std::string newer;
  if (setup && setup->UpdateAvailable(&newer)) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kOrange, "Version %s is out.", newer.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("See the update")) {
      select_tab = 3;
    }
    ImGui::Spacing();
  }
  if (DrawProblems()) {
    return;  // not built / no disc yet: the message points to Setup
  }
  if (state == game_process::State::kRunning) {
    DrawRunning();
  } else {
    if (state == game_process::State::kEnded) {
      RefreshSaves(saves_read < game->started());  // the session may have saved
      DrawReport();
    }
    if (other_game_running) {
      ImGui::TextColored(kWarn, "The game is already running (started some other way).");
      MutedText("Close it first: two copies at once would fight over the saves.");
    } else {
      DrawPlay();
    }
  }
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::BeginDisabled(state == game_process::State::kRunning || other_game_running);
  DrawSaves();
  ImGui::EndDisabled();
  ImGui::Spacing();
  DrawFolders();
}

void Launcher::DrawLogTab() {
  if (log.file.empty()) {
    ImGui::Spacing();
    MutedText("Nothing yet: once the game starts from here, its log shows up in this tab as it is "
              "written. Earlier sessions' logs are in the logs folder.");
    if (ImGui::Button("Logs folder")) {
      OpenPath(folders.user / "logs");
    }
    return;
  }

  // The file, its counts, buttons.
  const bool running = game->state() == game_process::State::kRunning;
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(running ? kGood : kMuted, running ? "Live" : "Last session");
  ImGui::SameLine();
  ImGui::TextColored(log.errors ? kBad : kMuted, " %d error%s", log.errors,
                     log.errors == 1 ? "" : "s");
  ImGui::SameLine();
  ImGui::TextColored(log.warnings ? kWarn : kMuted, "%d warning%s (%d different)", log.warnings,
                     log.warnings == 1 ? "" : "s", log.warning_kinds);
  ImGui::TextColored(kMuted, "%s", folders::Pretty(log.file).c_str());

  const float filter_width = ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleDpi * 18;
  ImGui::SetNextItemWidth(filter_width);
  ImGui::InputTextWithHint("##logfilter", "Show only lines containing...", log_filter,
                           sizeof(log_filter));
  ImGui::SameLine();
  ImGui::Checkbox("Errors and warnings only", &log_problems_only);
  ImGui::SameLine();
  ImGui::Checkbox("Follow new lines", &log_follow);
  ImGui::SameLine();
  if (ImGui::Button("Open in an editor")) {
    OpenPath(log.file);
  }
  ImGui::SameLine();
  if (ImGui::Button("Logs folder")) {
    OpenPath(folders.user / "logs");
  }

  // Which lines pass the filter: rebuilt only when the log or the filter changed.
  const std::string filter_text = log_filter;
  if (log_shown_version != log.version || log_shown_filter != filter_text ||
      log_shown_problems != log_problems_only) {
    auto lower = [](std::string text) {
      std::transform(text.begin(), text.end(), text.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      return text;
    };
    const std::string needle = lower(filter_text);
    log_shown.clear();
    for (int i = 0; i < int(log.lines.size()); ++i) {
      const std::string& line = log.lines[i];
      if (log_problems_only && line.find("] [error] ") == std::string::npos &&
          line.find("] [warning] ") == std::string::npos && !line.starts_with("terminal: ")) {
        continue;
      }
      if (!needle.empty() && lower(line).find(needle) == std::string::npos) {
        continue;
      }
      log_shown.push_back(i);
    }
    log_shown_version = log.version;
    log_shown_filter = filter_text;
    log_shown_problems = log_problems_only;
  }
  if (log.dropped) {
    ImGui::TextColored(kMuted, "(the first %zu lines are only in the file)", log.dropped);
  }

  // The lines: only the visible ones are drawn (ImGuiListClipper), so a long
  // log costs nothing extra.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.07f, 0.07f, 0.09f, 1.0f});
  ImGui::BeginChild("logtext", {0, 0}, ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(fonts.mono, ImGui::GetStyle().FontSizeBase * 0.85f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.0f});
  ImGuiListClipper clipper;
  clipper.Begin(int(log_shown.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const std::string& line = log.lines[log_shown[row]];
      ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
      if (line.find("] [error] ") != std::string::npos) {
        colour = kBad;
      } else if (line.find("] [warning] ") != std::string::npos) {
        colour = kWarn;
      } else if (line.starts_with("terminal: ")) {
        colour = ImVec4{0.55f, 0.78f, 0.95f, 1.0f};
      }
      ImGui::PushStyleColor(ImGuiCol_Text, colour);
      ImGui::TextUnformatted(line.data(), line.data() + line.size());
      ImGui::PopStyleColor();
    }
  }
  // Follow: stay at the bottom while it's on; scrolling up turns it off,
  // scrolling back to the bottom turns it on again.
  if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
    log_follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1 && ImGui::GetIO().MouseWheel < 0;
  }
  if (log_follow) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void Launcher::Draw() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  DrawHeader();
  const game_process::State state = game->state();
  if (state == game_process::State::kIdle) {
    RefreshSaves(false);
    RefreshOtherGame();
  }
  game->CopyLog(log);
  if (folders.root.empty()) {
    DrawProblems();  // no game folder at all: nothing else can work
    ImGui::End();
    return;
  }
  if (ImGui::BeginTabBar("pages")) {
    const auto flags = [&](int tab) {
      return select_tab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem("Play", nullptr, flags(0))) {
      ImGui::Spacing();
      DrawPlayTab(state);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Settings", nullptr, flags(1))) {
      ImGui::Spacing();
      if (settings) {
        const settings_page::Look look{fonts.bold, kMuted, kWarn, kBad, kOrange};
        settings->Draw(look, state == game_process::State::kRunning);
      }
      ImGui::EndTabItem();
    }
    if (setup && ImGui::BeginTabItem("Setup", nullptr, flags(3))) {
      ImGui::Spacing();
      const setup::Look look{fonts.bold, fonts.mono, kMuted, kGood, kWarn, kBad, kOrange};
      setup->Draw(look, state == game_process::State::kRunning || other_game_running);
      ImGui::EndTabItem();
    }
    char log_label[64];
    std::snprintf(log_label, sizeof(log_label), "Game log%s###log",
                  state == game_process::State::kRunning ? " (live)" : "");
    if (ImGui::BeginTabItem(log_label, nullptr, flags(2))) {
      ImGui::Spacing();
      DrawLogTab();
      ImGui::EndTabItem();
    }
    select_tab = -1;
    ImGui::EndTabBar();
  }
  ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
  fs::path override_root;
  std::string screenshot, play;
  int start_tab = -1;
  std::string desktop_action;
  std::string setup_run;
  std::string update_feed;
  bool after_update = false;
  fs::path setup_iso;
  double screenshot_after = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.starts_with("--game_folder=")) {
      override_root = arg.substr(std::strlen("--game_folder="));
    } else if (arg.starts_with("--desktop_entry=")) {
      desktop_action = arg.substr(std::strlen("--desktop_entry="));
    } else if (arg.starts_with("--setup_run=")) {
      setup_run = arg.substr(std::strlen("--setup_run="));
    } else if (arg.starts_with("--update_feed=")) {
      update_feed = arg.substr(std::strlen("--update_feed="));
    } else if (arg == "--after_update") {
      after_update = true;
    } else if (arg.starts_with("--iso=")) {
      setup_iso = arg.substr(std::strlen("--iso="));
    } else if (arg == "--tab=setup") {
      start_tab = 3;
    } else if (arg == "--tab=settings") {
      start_tab = 1;
    } else if (arg == "--tab=log") {
      start_tab = 2;
    } else if (arg.starts_with("--play=")) {
      play = arg.substr(std::strlen("--play="));
    } else if (arg.starts_with("--screenshot_after=")) {
      screenshot_after = std::atof(arg.c_str() + std::strlen("--screenshot_after="));
    } else if (arg.starts_with("--screenshot=")) {
      screenshot = arg.substr(std::strlen("--screenshot="));
    } else if (arg == "-h" || arg == "--help") {
      std::printf("crash_mom_launcher [--game_folder=<path>]\n"
                  "Starts Crash: Mind over Mutant (PC port). The game folder is found next to\n"
                  "or above the launcher unless given.\n");
      return 0;
    }
  }

  // --desktop_entry=add|remove: the applications-menu entry, without a window.
  if (!desktop_action.empty()) {
    std::error_code ec;
    const fs::path self = platform::SelfPath();
    const folders::Folders found = folders::Find(override_root);
    std::string error;
    const bool ok = desktop_action == "remove" ? desktop_entry::Remove(&error)
                                               : desktop_entry::Write(self, found.root, &error);
    std::printf("%s: %s\n", ok ? "done" : "failed",
                ok ? desktop_entry::EntryPath().string().c_str() : error.c_str());
    return ok ? 0 : 1;
  }

  // --setup_run=check|source|disc|sdk|game [--iso=<file>]: a Setup step
  // without a window (tests; setup.h), output on stdout.
  if (!setup_run.empty()) {
    const folders::Folders found = folders::Find(override_root);
    if (found.root.empty() || !found.from_source) {
      std::printf("setup: needs the source tree\n");
      return 1;
    }
    std::error_code self_ec;
    setup::Setup headless(found, update_feed, platform::SelfPath());
    const int code = headless.RunHeadless(setup_run, setup_iso);
    std::fflush(stdout);
    std::_Exit(code);
  }

  SDL_SetAppMetadata("Crash: Mind over Mutant launcher", "0.1.0-alpha", "crash_mom_launcher");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window = SDL_CreateWindow("Mind over Recomp", int(980 * scale),
                                        int(680 * scale),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "launcher: no window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetWindowMinimumSize(window, int(720 * scale), int(480 * scale));
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    std::fprintf(stderr, "launcher: no renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // nothing to remember between runs (yet)
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ApplyStyle(scale);

  Launcher launcher;
  launcher.folders = folders::Find(override_root);
  launcher.fonts = LoadFonts();
  launcher.RefreshSaves(true);
  launcher.select_tab = start_tab;
  {
    std::error_code ec;
    launcher.launcher_path = platform::SelfPath();
    // Windows: an update renamed the previous launcher aside (update.cpp).
    fs::path aside = launcher.launcher_path;
    aside += ".old";
    fs::remove(aside, ec);
    // First start: into the applications menu (once; desktop_entry.h). Not in
    // test runs (--screenshot / --play), which use copies of the game folder.
    if (!launcher.folders.root.empty() && screenshot.empty() && play.empty()) {
      desktop_entry::RenameIfOld(launcher.launcher_path, launcher.folders.root);
      desktop_entry::AddOnFirstStart(launcher.launcher_path, launcher.folders.root,
                                     launcher.folders.user);
    }
    launcher.desktop = desktop_entry::Check(launcher.launcher_path);
  }
  if (!launcher.folders.root.empty() && launcher.folders.from_source) {
    launcher.setup = std::make_unique<setup::Setup>(launcher.folders, update_feed,
                                                    launcher.launcher_path, after_update);
    if (after_update) {
      launcher.select_tab = 3;  // the update's rebuild runs there
    } else if (start_tab < 0 && launcher.setup->NeedsAttention()) {
      launcher.select_tab = 3;  // not playable yet: open on Setup
    }
  }
  if (!launcher.folders.root.empty()) {
    launcher.settings = std::make_unique<settings_page::Page>(
        launcher.folders.exe, launcher.folders.root, launcher.folders.user);
  }

  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // --play: started before the first frame (the page then shows "Playing").
  if (!play.empty() && launcher.folders.ExeExists() && launcher.folders.DiscExists()) {
    if (play == "new") {
      launcher.Play(0);
    } else if (play == "last") {
      launcher.Play(launcher.save_list.empty() ? 0 : launcher.save_list.front().number);
    } else {
      launcher.Play(std::atoi(play.c_str()));
    }
  }

  bool running = true;
  int redraws = 2;
  int frames = 0;
  const Clock::time_point launched = Clock::now();  // ImGui settles a change over a couple of frames
  while (running) {
    // Sleep until something happens; 4 times a second anyway while the game
    // runs (its clock, the live log, the report when it ends).
    SDL_Event event;
    const bool idle = launcher.game->state() == game_process::State::kIdle;
    if (redraws <= 0 && (screenshot.empty() || frames >= 4) && SDL_WaitEventTimeout(&event, idle ? 2000 : 250)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      }
      redraws = 2;
    }
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      }
      redraws = 2;
    }
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      redraws = 0;
      continue;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    launcher.Draw();
    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 23, 23, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    // --screenshot: the 4th frame (fonts and layout settled), then quit.
    if (!screenshot.empty() && ++frames >= 4 &&
        Clock::now() - launched >= std::chrono::duration<double>(screenshot_after)) {
      if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
        if (!SDL_SavePNG(shot, screenshot.c_str())) {
          std::fprintf(stderr, "launcher: screenshot failed: %s\n", SDL_GetError());
        }
        SDL_DestroySurface(shot);
      }
      running = false;
    }
    SDL_RenderPresent(renderer);
    --redraws;
  }

  // The game keeps running if it's up (its own session): closing the
  // launcher never closes the game.
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::_Exit(0);  // don't wait on the game watcher thread (it's detached by ~Game anyway)
}
