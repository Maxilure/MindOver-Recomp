// =============================================================================
// installer.cpp -- "Mind over Recomp.exe": installs, updates and uninstalls
//                  the game on Windows
// =============================================================================
//
// The port never ships the game: every player builds it on their own PC from
// their own disc. This small program does exactly that and nothing more
// (no settings, no save list: those live in the game; logs are in user\logs).
// It runs in one of three MODES, picked at start:
//
//   INSTALL    run from anywhere else (the download). Page 0 asks which:
//     INSTALL FRESH
//     1. Choose     the disc image (.iso) and where to put the game (a folder
//                   "Crash Mind over Mutant" is made in the picked folder),
//                   a desktop shortcut, optionally saves of an older version,
//                   "use fewer resources while building"
//     UPDATE (2026-11-08; since 2026-11-12 also what the installed copy's
//     "Update now" hands over to: --update_window, no presses needed)
//     1. Choose     the installed game's folder (its version is read from
//                   source\RELEASE.toml and compared with this installer's)
//                   (Windows' Apps list's install is filled in) and the
//                   disc image: needed only if its extracted files are gone,
//                   else optional (the game's files checked against it)
//     then, both:
//     2. Tools      the build tools (setup.cpp step 1); missing ones are
//                   installed with winget at the press of one button
//     3. Install    the port's files (next to this exe when it came in the
//                   release .zip, else downloaded from the release feed;
//                   an update swaps them in KEEPING the build: update.h
//                   LocalUpdateJob, so only what changed compiles again),
//                   this exe copied in, then setup.h's "set up everything":
//                   SDK download, disc extraction, SDK + game build; last,
//                   the Start menu, the desktop and Windows' Apps list.
//                   The PC is kept awake meanwhile; after 10 minutes a note
//                   asks to leave heavy programs closed.
//     4. Done       "Finished in N min", Play
//   The window's title carries this installer's version.
//   The steps move on in Update(), every pass of the main loop, drawn or
//   not: they used to move only while the page was DRAWN, and a minimized
//   window isn't drawn.
//
//   MANAGE     run from inside an install (its Start menu "Mind over
//              Recomp"): the version, Build again when something's out of
//              date, missing tools, Play, the game folder, Uninstall.
//              UPDATES (2026-11-12, the SELF-UPDATE, update.h): the feed is
//              read at start; "Update now" downloads the new release's .zip
//              (SHA-256 checked) and starts ITS installer with
//              --update_window=<install>, which runs the update page by
//              itself. So the code that updates is always the NEW version's:
//              the old one only downloads, checks, unpacks and starts. An
//              update keeps the version it replaces in previous\ and puts it
//              back when it fails; "Go back to <version>" puts it back by
//              hand. A release whose release.toml says a higher `updater`
//              than this program knows = the manual way (the releases page).
//
//   UNINSTALL  --uninstall (Windows' Apps list, or the button): one checkbox,
//              "Keep my saves, settings and logs" (on), then everything else
//              goes (install.h). Runs from a copy in %TEMP%: a program can't
//              delete its own file.
//
// WITHOUT A WINDOW (tests over ssh; output on stdout):
//   --check_tools                         prints the build tools' checks
//   --install_tools                       installs the missing ones (winget)
//   --install_to=<folder> --iso=<file>    a whole install into <folder>\Crash Mind over Mutant
//       [--desktop]                       (+ a desktop shortcut)
//   --update --game_folder=<install> [--iso=<file>]   (with --iso: game\ checked against the disc)
//                                         update that install with this installer's files + rebuild
//   --self_update [--update_feed=<url>]   (run from an install) the Manage page's Update now:
//                                         feed, download, then the new installer's --update
//   (MOR_TEST_BROKEN_UPDATE=1 in the environment: an update fails on purpose after the
//    game's files are swapped: the rollback test)
//   --uninstall --quiet [--remove_user]   uninstall (keeps user\ unless --remove_user)
// Other options: --update_feed=<url> (a test feed, update.h), --game_folder=
// (an install to manage), --screenshot=<png> [--screenshot_after=<s>] (a
// picture of the window, then quit).
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // Windows: SDL provides WinMain and calls main below
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "folders.h"
#include "install.h"
#include "platform.h"
#include "setup.h"
#include "task_runner.h"
#include "texts.h"
#include "ui_look.h"
#include "update.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using task_runner::Command;
using task_runner::State;
using namespace ui;

namespace {

// languages/pt_BR.po, built in (launcher/CMakeLists.txt; texts.h AddBuiltIn).
#include "pt_BR_po.inc"

// The installer's languages: English, and Brazilian Portuguese (most of the
// game's fans speak Portuguese). The first start picks Windows' own display
// language; the picker in the corner switches.
struct Language {
  const char* code;   // texts.h code ("" = English)
  const char* name;   // in its own language
};
constexpr Language kLanguages[] = {{"", "English"}, {"pt_BR", "Portugu\xC3\xAAs (Brasil)"}};

std::string WindowsLanguage() {
#if defined(_WIN32)
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE ? "pt_BR" : "";
#else
  const std::string lang = platform::GetEnv("LANG");
  return lang.starts_with("pt") ? "pt_BR" : "";
#endif
}

// This installer's version (VERSION.txt, baked in by launcher/CMakeLists.txt):
// the window's title, and what an update brings.
#ifndef MOR_VERSION
#define MOR_VERSION "unknown"
#endif
constexpr const char* kVersion = MOR_VERSION;

// "1 h 05 min", "12 min 30 s", "40 s".
std::string Duration(double seconds) {
  const long total = long(seconds + 0.5);
  char text[48];
  if (total >= 3600) {
    std::snprintf(text, sizeof(text), trs("%ld h %02ld min"), total / 3600, (total / 60) % 60);
  } else if (total >= 60) {
    std::snprintf(text, sizeof(text), trs("%ld min %02ld s"), total / 60, total % 60);
  } else {
    std::snprintf(text, sizeof(text), trs("%ld s"), total);
  }
  return text;
}

// After this long, the installing page asks to keep heavy programs closed.
constexpr double kSlowNoteSeconds = 10 * 60;

// Space a whole install takes: ~6.5 GB of disc files + ~2-4 GB of SDK and
// build (8.5 GB measured on Windows 11) + room for an update's download.
// READY-BUILT (no build): the disc's 6.1 GB + the game 0.2 GB + room for an
// update's download and the previous version (~0.4 GB): 8 GB.
constexpr double kNeededGbBuild = 15.0;
constexpr double kNeededGbReady = 8.0;

std::string Stamp() {
  const std::tm local = platform::LocalTime(std::time(nullptr));
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d_%H%M%S", &local);
  return text;
}

bool GameRunning() {
  return platform::ProcessRunning("crash_mom") ||
         platform::ProcessRunning(fs::path(folders::InstalledExeName()).stem().string());
}

// Does the folder this exe sits in hold the port's files (the release .zip
// unpacked: "Mind over Recomp.exe" next to source\)? An install or update
// needs them: the installer downloads NOTHING itself (2026-11-08: the
// download fallback, for an exe started without its files, is gone; a new
// version = the new release's .zip from GitHub). Without them: the warning
// below, and installs / updates are refused.
fs::path SourceNextToMe() {
  std::error_code ec;
  const fs::path here = platform::SelfPath().parent_path() / "source";
  return fs::is_regular_file(here / "RELEASE.toml", ec) ? here : fs::path();
}

// A READY-BUILT release (launcher/setup.cpp Kit, READY-BUILT): the game's
// program\ folder comes next to this exe. Empty = this release builds it.
fs::path ProgramNextToMe() {
  std::error_code ec;
  const fs::path here = platform::SelfPath().parent_path() / "program";
  return fs::is_regular_file(here / folders::InstalledExeName(), ec) ? here : fs::path();
}

// This release comes ready-built (nothing compiled on the player's PC).
bool ReadyBuilt() { return !ProgramNextToMe().empty(); }
double NeededGb() { return ReadyBuilt() ? kNeededGbReady : kNeededGbBuild; }

// Puts that program\ into the install (fresh, update and repair alike):
// copied into program.new, then swapped in, so a failed copy never leaves
// half a game. The icon made from the player's disc goes with the old
// folder; Setup makes it again (its game step).
std::optional<Command> ProgramCopyCommand(const fs::path& root) {
  const fs::path from = ProgramNextToMe();
  if (from.empty()) return std::nullopt;
  const fs::path to = root / "program";
  Command copy;
  copy.title = trs("Copying the game into program\\");
  copy.function = [from, to](std::string* message) {
    std::error_code ec;
    const fs::path fresh = to.string() + ".new";
    fs::remove_all(fresh, ec);
    fs::copy(from, fresh, fs::copy_options::recursive, ec);
    if (ec) {
      *message = "couldn't copy " + from.string() + ": " + ec.message();
      return false;
    }
    // The old program\: kept as the previous version when the update keeps
    // one (update::StartBackup), else removed.
    const fs::path previous = to.parent_path() / update::kPreviousFolder;
    const bool keep = fs::is_regular_file(previous / "version.txt", ec);
    if (keep) {
      fs::remove_all(previous / "program", ec);
      ec.clear();
      if (fs::exists(to, ec)) fs::rename(to, previous / "program", ec);
    } else {
      fs::remove_all(to, ec);
    }
    if (ec) {
      *message = "couldn't replace " + to.string() + " (is the game still running?): " + ec.message();
      return false;
    }
    fs::rename(fresh, to, ec);
    if (ec && keep) {
      std::error_code back;
      fs::rename(previous / "program", to, back);  // the old one stays in place
    }
    *message = ec ? "couldn't put program\\ in place: " + ec.message() : "program\\ copied.";
    if (!ec && std::getenv("MOR_TEST_BROKEN_UPDATE")) {
      // Developer test of the rollback: the update "fails" right after the
      // new game is in place (never set by players).
      *message = "MOR_TEST_BROKEN_UPDATE: this update fails on purpose";
      return false;
    }
    return !ec;
  };
  return copy;
}

// A READY-BUILT install keeps no build: an install that was built on this PC
// (0.2.x) still has its build in source\ (several GB: out\, the recompiled
// code, the SDK and its download), and an update keeps the old source\ as
// the previous version. Both are pruned. Only those folders, and only when
// the installed port's files are a ready-built release.
Command PruneOldBuildCommand(const fs::path& root) {
  Command prune;
  prune.title = trs("Removing the old build (not needed any more)");
  prune.function = [root](std::string* message) {
    std::error_code ec;
    if (!fs::is_regular_file(root / "source" / "buildtools" / "prebuilt.txt", ec)) return true;
    int removed = 0;
    for (const fs::path base : {root / "source", root / update::kPreviousFolder / "source"}) {
      for (const char* part : {"out", "generated", "thirdparty/rexglue-sdk", "thirdparty/rexglue-install"}) {
        const fs::path path = base / part;
        if (!fs::exists(path, ec)) continue;
        std::string failed;
        if (install::RemovePath(path, &failed)) {
          ++removed;
        } else {
          *message += "couldn't remove all of " + path.string() + " (" + failed + ")\n";
        }
      }
    }
    *message += std::to_string(removed) + " old build folders removed.";
    return true;  // leftovers only cost space
  };
  return prune;
}

// Puts this exe into the game folder as "Mind over Recomp.exe". The one
// there may be RUNNING (the self-update's old Mind over Recomp, still waiting
// for this one): Windows can't overwrite a running exe but can rename it, so
// it moves aside to <name>.old first (deleted at its next start, main()).
bool CopySelfInto(const fs::path& root, std::string* error) {
  const fs::path self = platform::SelfPath();
  const fs::path target = root / install::kInstallerName;
  std::error_code ec;
  if (fs::equivalent(self, target, ec)) return true;
  ec.clear();
  if (fs::exists(target, ec)) {
    fs::path aside = target;
    aside += ".old";
    fs::remove(aside, ec);
    ec.clear();
    fs::rename(target, aside, ec);
    ec.clear();
  }
  fs::copy_file(self, target, fs::copy_options::overwrite_existing, ec);
  if (ec) *error = "couldn't copy " + self.string() + ": " + ec.message();
  return !ec;
}

const char* MissingFilesText() {
  return trs("The port's files aren't next to this installer. Unpack the WHOLE .zip from the GitHub "
             "release page, then start Mind over Recomp.exe from the unpacked folder.");
}

void OpenFolder(const fs::path& path) {
  SDL_OpenURL(("file:///" + path.generic_string()).c_str());
}

std::string UrlEncode(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// When a step fails: where to report it, with the log (GitHub's bug form,
// filled in with what we know; the player attaches the log file).
void DrawReportHelp(const std::string& what_failed, const fs::path& log, const std::string& version) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.16f, 0.13f, 0.10f, 1.0f});
  ImGui::BeginChild("report", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
  ImGui::TextWrapped("%s", trs("Still not working? Report it on GitHub and attach the log file: "
                               "that's how installs get fixed. The button opens the report form "
                               "and the log's folder; drag the log file into the form."));
  if (!log.empty()) ImGui::TextDisabled("%s", log.string().c_str());
  if (ImGui::Button(trs("Report it on GitHub"))) {
    const std::string url =
        "https://github.com/Maxilure/MindOver-Recomp/issues/new?template=bug_report.yml"
        "&title=" + UrlEncode("Installer: " + what_failed.substr(0, 120)) +
        "&what=" + UrlEncode("The Windows installer stopped:\n\n" + what_failed) +
        "&version=" + UrlEncode(version.empty() ? "installer" : version) +
        "&system=" + UrlEncode("Windows");
    SDL_OpenURL(url.c_str());
    if (!log.empty()) OpenFolder(log.parent_path());
  }
  ImGui::SameLine();
  if (!log.empty() && ImGui::Button(trs("Open the log's folder"))) OpenFolder(log.parent_path());
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// --- the desktop's file / folder pickers ---------------------------------------
// They answer on another thread, maybe later: the answer waits here.
std::mutex g_pick_mutex;
std::string g_picked;      // the chosen path
int g_pick_target = 0;     // which field asked (Picker below)
enum Picker { kPickIso = 1, kPickParent, kPickOldVersion, kPickUpdateFolder };

void OnPicked(void* target, const char* const* files, int) {
  if (files && files[0]) {  // null = error, empty list = cancelled
    std::lock_guard lock(g_pick_mutex);
    g_picked = files[0];
    g_pick_target = int(reinterpret_cast<intptr_t>(target));
  }
}

// A job's progress: the step, a bar, and its output (folded open on request).
void DrawJob(const task_runner::Snapshot& job, const Fonts& fonts, bool* show_log) {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextWrapped("%s", job.step.empty() ? job.title.c_str() : job.step.c_str());
  ImGui::PopFont();
  ImGui::ProgressBar(float(job.fraction), ImVec2(-1, 0), job.detail.empty() ? nullptr : job.detail.c_str());
  if (job.state == State::kFailed) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", job.error.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Checkbox(trs("Show details"), show_log);
  if (*show_log) {
    ImGui::PushFont(fonts.mono, 0.0f);
    ImGui::BeginChild("log", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.5f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : job.lines) ImGui::TextUnformatted(line.c_str());
    if (job.state == State::kRunning) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopFont();
  }
  if (!job.log.empty()) {
    ImGui::TextDisabled(trs("Log: %s"), job.log.string().c_str());
  }
}

// The build tools' list (step 1).
void DrawTools(const setup::Status& status) {
  if (ImGui::BeginTable("tools", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("ok", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    for (const setup::Tool& tool : status.tools) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(tool.found ? kGood : kBad, "%s", tool.found ? "OK" : "--");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(tool.label.c_str());
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, tool.found ? kMuted : kWarn);
      ImGui::TextWrapped("%s", tool.detail.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
}

bool AllToolsFound(const setup::Status& status) {
  return std::all_of(status.tools.begin(), status.tools.end(),
                     [](const setup::Tool& t) { return t.found; });
}

bool EverythingDone(const setup::Status& s) {
  using setup::StepState;
  return s.done && s.source_state == StepState::kDone && s.disc_state == StepState::kDone &&
         s.sdk_state == StepState::kDone && s.game_state == StepState::kDone;
}

// =============================================================================
// The window
// =============================================================================

enum class Mode { kInstall, kManage, kUninstall };
// kStart: "install fresh" or "update"; kChoose = a fresh install's choices,
// kUpdate = an update's (then both go on to kTools).
enum class Page { kStart, kChoose, kUpdate, kTools, kInstalling, kDone };
// The install's own phases (kInstalling): the port's files, then Setup, then
// the shortcuts and the Apps list.
enum class Phase { kFiles, kBuild, kFinish, kFinished, kFailed };

struct App {
  Mode mode = Mode::kInstall;
  Fonts fonts;
  std::string feed;            // --update_feed ("" = GitHub's)
  bool show_log = false;

  // --- INSTALL: the player's choices ---------------------------------------
  Page page = Page::kStart;
  // UPDATE (2026-11-08): this run updates the game in `update_folder`
  // (picked; its root found by install::ExistingGameAt) from version
  // `update_from` to kVersion. update_same = already this version: the
  // files stay, only what's missing is built (a repair).
  bool updating = false;
  std::string update_folder;
  std::string update_checked_for;    // the folder the fields below describe
  fs::path update_root;
  std::string update_from;
  std::string update_problem;        // why it can't be updated ("" = fine)
  bool update_same = false;
  bool update_needs_disc = false;    // its game\ (extracted disc) is missing
  // "Use fewer resources while building" (setup.h SetLimitedResources).
  bool limited = false;
  // The clock: from the Install / Update press to Done.
  Clock::time_point started{};
  double took_seconds = 0;
  bool awake = false;                // platform::KeepAwake is on
  std::string iso;
  std::string parent = install::DefaultParent().string();
  bool desktop = true;
  bool import_saves = false;
  std::string old_version;     // an older install's folder (its user\ is copied)
  std::string choose_error;
  // winget (App Installer) wasn't found: the tools page offers Microsoft's
  // official download (setup::kAppInstallerUrl) under the error.
  bool winget_missing = false;
  fs::path root;               // <parent>\Crash Mind over Mutant
  // A game set up and built before, found where the player points
  // (install::ExistingGameAt): taken over as it is, nothing built again
  // (2026-11-05). Looked up again only when `parent` changes.
  bool existing = false;
  fs::path existing_root;
  std::string existing_for;
  // ALREADY INSTALLED (2026-11-05): a downloaded installer started while
  // Windows' Installed apps lists an install (install::RegisteredRoot) opened
  // on "new install" like any other, next to the installed one's own Mind
  // over Recomp (Start menu) that manages it: two programs of the same name
  // doing different things. Page 1 now says where the game is installed and
  // opens that one (play, update, uninstall); a second install stays possible
  // below. Looked up once.
  bool registered_checked = false;
  fs::path registered_root;
  std::string registered_version;
  bool quit = false;           // the window closes after this frame

  // --- step 1, the tools (before any folder exists) ---------------------------
  std::mutex tools_mutex;
  setup::Status tools;         // under tools_mutex
  std::atomic<bool> tools_checking{false};
  std::thread tools_thread;
  task_runner::Runner tools_runner;
  task_runner::Snapshot tools_job;

  // --- the install itself -------------------------------------------------------
  Phase phase = Phase::kFiles;
  task_runner::Runner files_runner;
  task_runner::Snapshot files_job;
  std::string fail_text;
  // An update that keeps the previous version (update::StartBackup) and
  // failed: it was put back (update::Rollback), once; rollback_note says so.
  bool backed_up = false;
  std::string rollback_note;
  // --update_window (the installed Mind over Recomp's "Update now"): the
  // update goes on by itself, no Next presses (unless something's missing).
  bool auto_update = false;
  std::atomic<bool> finishing{false};
  std::thread finish_thread;
  std::string finish_error;    // after the thread

  // --- MANAGE (and the build part of INSTALL) ------------------------------------
  folders::Folders folders;
  std::unique_ptr<setup::Setup> setup;
  bool was_setting_up = false; // to notice the end of a build / update
  std::string manage_note;
  // THE SELF-UPDATE (update.h SelfUpdateJob): the feed checked at start,
  // "Update now" downloads the new release and hands over to its installer.
  std::unique_ptr<update::Updater> updater;
  task_runner::Runner self_update_runner;
  task_runner::Snapshot self_update_job;
  std::shared_ptr<fs::path> new_installer = std::make_shared<fs::path>();
  bool show_notes = false;
  // "Go back to <previous version>" asks first.
  bool confirm_go_back = false;
  // Started by an old updater (--after_update): an unfinished update is
  // finished without a press.
  bool finish_on_start = false;

  // --- UNINSTALL -----------------------------------------------------------------
  bool keep_user = true;
  std::atomic<int> uninstall_state{0};  // 0 asking, 1 removing, 2 done, 3 done with errors
  std::atomic<uint64_t> removed{0};
  std::string uninstall_error;
  std::thread uninstall_thread;

  ~App() {
    for (std::thread* t : {&tools_thread, &finish_thread, &uninstall_thread}) {
      if (t->joinable()) t->join();
    }
  }

  void Draw();
  void Update();                 // the steps' state machine: every loop pass, drawn or not
  void DrawHeader();
  void TakePicks();
  // INSTALL
  void DrawStart();
  void DrawChoose();
  void DrawUpdate();
  void CheckUpdateFolder();
  void DrawSpeedOption();
  void UpdateInstalling();
  void SetAwake(bool on);
  void DrawToolsPage();
  void DrawInstalling();
  void DrawDone();
  void CheckTools();
  void BeginInstall();
  void StartFiles();
  void StartBuild();
  void StartFinish();
  // MANAGE
  void DrawManage();
  void DrawManageUpdates(bool busy);
  // UNINSTALL
  void DrawUninstall();
  void StartUninstall();
};

void App::DrawHeader() {
  ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.9f);
  ImGui::TextUnformatted("Mind over Recomp");
  ImGui::PopFont();
  // The language picker, top right (texts.h SetLanguage: takes effect now).
  {
    const float width = 190 * ImGui::GetStyle().FontScaleDpi;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
    ImGui::SetNextItemWidth(width);
    const char* current = kLanguages[0].name;
    for (const Language& language : kLanguages) {
      if (texts::Code() == language.code) current = language.name;
    }
    if (ImGui::BeginCombo("##language", current)) {
      for (const Language& language : kLanguages) {
        if (ImGui::Selectable(language.name, texts::Code() == language.code)) {
          texts::SetLanguage(language.code);
        }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::TextDisabled("%s  -  %s", trs("An unofficial PC port of Crash: Mind over Mutant, built from your own disc"),
                      trf(trs("version {0}"), {kVersion}).c_str());
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
}

void App::TakePicks() {
  std::lock_guard lock(g_pick_mutex);
  if (g_pick_target == kPickIso) iso = g_picked;
  if (g_pick_target == kPickParent) parent = g_picked;
  if (g_pick_target == kPickOldVersion) old_version = g_picked;
  if (g_pick_target == kPickUpdateFolder) update_folder = g_picked;
  if (g_pick_target) choose_error.clear();
  g_pick_target = 0;
}

// --- INSTALL, page 0: install fresh, or update --------------------------------

void App::DrawStart() {
  // An install Windows' Installed apps knows about (install::RegisteredRoot):
  // named here, and the Update page starts on it.
  if (!registered_checked) {
    registered_checked = true;
    const fs::path at = install::RegisteredRoot();
    std::error_code ec;
    if (!at.empty() && install::LooksInstalled(at) && !fs::equivalent(at, platform::SelfPath().parent_path(), ec)) {
      registered_root = at;
      registered_version = update::InstalledVersion(folders::Find(at));
      update_folder = at.string();
    }
  }
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(trs("What do you want to do?"));
  ImGui::PopFont();
  ImGui::Spacing();
  if (!registered_root.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    ImGui::TextWrapped(trs("The game is installed on this PC: %s (version %s)."), registered_root.string().c_str(),
                       registered_version.empty() ? "?" : registered_version.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  if (SourceNextToMe().empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", MissingFilesText());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  const float width = 330 * ImGui::GetStyle().FontScaleDpi;
  auto choice = [&](const char* label, const char* text, bool accent) {
    const bool pressed = accent ? AccentButton(label, ImVec2(width, 0)) : ImGui::Button(label, ImVec2(width, 0));
    ImGui::Indent();
    MutedText(text);
    ImGui::Unindent();
    ImGui::Spacing();
    return pressed;
  };
  const bool installed = !registered_root.empty();
  // The likely pick is the accented one: Update when an install is there.
  if (choice(trs("Install the game"),
             ReadyBuilt() ? trs("A new install from your disc image. It takes a minute or two and needs about "
                                "8 GB of free space (the disc's files are most of it).")
                          : trs("A new install from your disc image. The first build takes a while (often 20-60 "
                                "minutes) and needs about 15 GB of free space."),
             !installed)) {
    updating = false;
    page = Page::kChoose;
    choose_error.clear();
  }
  if (choice(trs("Update an installed game"),
             trf(trs("Brings an installed game to version {0}. Your saves and settings stay; the current "
                     "version is kept and put back if the update fails."),
                 {kVersion}).c_str(),
             installed)) {
    updating = true;
    page = Page::kUpdate;
    choose_error.clear();
    update_checked_for = "\x01";  // look at the folder again
  }
}

// "Use fewer resources while building" (both kinds of install).
void App::DrawSpeedOption() {
  if (ReadyBuilt()) return;  // nothing is built
  ImGui::Checkbox(trs("Use fewer resources while building (slower, the PC stays usable)"), &limited);
  ImGui::Indent();
  MutedText(limited ? trs("Half of the processor and low priority: other programs come first.")
                    : trs("As fast as this PC allows, with one processor core left free for Windows."));
  ImGui::Unindent();
}

// --- UPDATE, page 1 -----------------------------------------------------------

// What's in `update_folder`: an install of ours, its version, its disc files.
void App::CheckUpdateFolder() {
  if (update_checked_for == update_folder) return;
  update_checked_for = update_folder;
  update_root.clear();
  update_from.clear();
  update_problem.clear();
  update_same = false;
  update_needs_disc = false;
  if (update_folder.empty()) {
    update_problem = trs("Pick the folder the game is installed in.");
    return;
  }
  // The picked folder, or the "Crash Mind over Mutant" folder in it.
  const fs::path at = install::ExistingGameAt(fs::path(update_folder));
  const folders::Folders found = at.empty() ? folders::Folders{} : folders::Find(at);
  if (at.empty() || !found.release) {
    update_problem = trs("No installed game in that folder: pick the folder with \"Mind over Recomp.exe\", "
                         "source and user in it (or the one above it).");
    return;
  }
  update_root = at;
  update_from = update::InstalledVersion(found);
  update_needs_disc = !found.DiscExists();
  if (update_from.empty()) {
    update_problem = trs("That install has no version file (source\\RELEASE.toml): install fresh instead.");
  } else if (update::Newer(update_from, kVersion)) {
    update_problem = trf(trs("That install is version {0}, newer than this installer ({1}). Download the "
                             "newest installer from GitHub."),
                         {update_from, kVersion});
  } else {
    update_same = update_from == kVersion;
  }
}

void App::DrawUpdate() {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(trf(trs("1. Update to version {0}"), {kVersion}).c_str());
  ImGui::PopFont();
  MutedText(trs("Your saves, settings and logs stay. The current version is kept in the game folder's "
                "\"previous\" folder: if the update fails it's put back by itself."));
  ImGui::Spacing();
  SDL_Window* window = SDL_GetMouseFocus();
  const float button_w = ImGui::CalcTextSize(trs("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2;
  auto path_row = [&](const char* label, std::string* value, int target, bool folder) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-button_w - ImGui::GetStyle().ItemSpacing.x);
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", value->c_str());
    ImGui::PushID(target);
    if (ImGui::InputText("##path", buffer, sizeof(buffer))) {
      *value = buffer;
      choose_error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(trs("Browse..."))) {
      if (folder) {
        SDL_ShowOpenFolderDialog(OnPicked, reinterpret_cast<void*>(intptr_t(target)), window,
                                 value->empty() ? nullptr : value->c_str(), false);
      } else {
        static const SDL_DialogFileFilter kIso[] = {{"Xbox 360 disc image (*.iso)", "iso"}};
        SDL_ShowOpenFileDialog(OnPicked, reinterpret_cast<void*>(intptr_t(target)), window, kIso, 1,
                               nullptr, false);
      }
    }
    ImGui::PopID();
  };
  path_row(trs("The installed game's folder"), &update_folder, kPickUpdateFolder, true);
  CheckUpdateFolder();
  if (!update_problem.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", update_problem.c_str());
    ImGui::PopStyleColor();
  } else if (update_same) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    ImGui::TextWrapped(trs("Found version %s: already this version. Next checks it and builds whatever is "
                           "missing or out of date (a repair)."),
                       update_from.c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    ImGui::TextWrapped(trs("Found version %s in %s: it will be updated to %s."), update_from.c_str(),
                       update_root.string().c_str(), kVersion);
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  // The disc image: NEEDED only when game\ (the extracted disc) is gone;
  // else OPTIONAL (2026-11-12: it was always asked, and the self-update
  // can't ask): given, the game's files are checked against it and only
  // what's missing or damaged is copied again (setup::DiscRepairCommand).
  if (update_problem.empty()) {
    path_row(update_needs_disc
                 ? trs("Disc image (.iso) of Crash: Mind over Mutant, Xbox 360 (its files aren't in the game "
                       "folder any more: they're extracted again)")
                 : trs("Disc image (.iso), optional: the game's files are checked against it (only if "
                       "something seems broken)"),
             &iso, kPickIso, false);
    ImGui::Spacing();
  }
  DrawSpeedOption();
  ImGui::Spacing();
  if (!choose_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", choose_error.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::Button(trs("Back"))) {
    page = Page::kStart;
    auto_update = false;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!update_problem.empty());
  // The self-update presses Next itself, once (a problem below stops it).
  const bool auto_next = auto_update && update_problem.empty() && !update_needs_disc && choose_error.empty();
  if (AccentButton(trs("Next"), ImVec2(160 * ImGui::GetStyle().FontScaleDpi, 0)) || auto_next) {
    std::error_code ec;
    if (GameRunning()) {
      choose_error = trs("The game is running: close it, then press Next.");
    } else if (update_needs_disc && (iso.empty() || !fs::is_regular_file(fs::path(iso), ec))) {
      choose_error = trs("Pick your disc image (.iso) first.");
    } else if (!iso.empty() && !fs::is_regular_file(fs::path(iso), ec)) {
      choose_error = trs("That disc image isn't there: pick it again, or empty the field.");
    } else {
      root = update_root;
      existing = true;   // nothing new to make: the folder is there
      desktop = false;   // shortcuts are rewritten; a desktop one only if it was there
      page = Page::kTools;
      CheckTools();
    }
  }
  ImGui::EndDisabled();
}

// --- INSTALL, page 1 -----------------------------------------------------------

void App::DrawChoose() {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(trs("1. Your disc and where the game goes"));
  ImGui::PopFont();
  if (ReadyBuilt()) {
    MutedText(trs("The game's files come from your own copy of the disc: none come with this program. "
                  "The install takes a minute or two and needs about 8 GB of free space."));
  } else {
    MutedText(trs("The game is built on this PC from your own copy of the disc: no game files come "
                  "with this program. The first build takes a while (often 20-60 minutes) and "
                  "needs about 15 GB of free space."));
  }
  ImGui::Spacing();

  SDL_Window* window = SDL_GetMouseFocus();
  const float button_w = ImGui::CalcTextSize(trs("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2;
  auto path_row = [&](const char* label, std::string* value, int target, bool folder) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-button_w - ImGui::GetStyle().ItemSpacing.x);
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", value->c_str());
    ImGui::PushID(target);
    if (ImGui::InputText("##path", buffer, sizeof(buffer))) {
      *value = buffer;
      choose_error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(trs("Browse..."))) {
      if (folder) {
        SDL_ShowOpenFolderDialog(OnPicked, reinterpret_cast<void*>(intptr_t(target)), window,
                                 value->empty() ? nullptr : value->c_str(), false);
      } else {
        static const SDL_DialogFileFilter kIso[] = {{"Xbox 360 disc image (*.iso)", "iso"}};
        SDL_ShowOpenFileDialog(OnPicked, reinterpret_cast<void*>(intptr_t(target)), window, kIso, 1,
                               nullptr, false);
      }
    }
    ImGui::PopID();
  };
  path_row(trs("Disc image (.iso) of Crash: Mind over Mutant, Xbox 360"), &iso, kPickIso, false);
  ImGui::Spacing();
  path_row(trs("Install into this folder"), &parent, kPickParent, true);
  if (parent != existing_for) {
    existing_for = parent;
    existing_root = install::ExistingGameAt(fs::path(parent));
  }
  existing = !existing_root.empty();
  root = existing ? existing_root : fs::path(parent) / install::kFolderName;
  std::error_code disc_ec;
  const bool disc_there = existing && fs::is_regular_file(root / "game" / "default.xex", disc_ec);
  if (existing) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    ImGui::TextWrapped(trs("A game that was set up before is in %s: it is used as it is, nothing is "
                           "built again."),
                       root.string().c_str());
    ImGui::PopStyleColor();
    if (disc_there) MutedText(trs("Its disc files are there too: no disc image needed."));
  } else {
    ImGui::TextDisabled(trs("The game goes into %s"), root.string().c_str());
  }
  ImGui::Spacing();
  ImGui::Checkbox(trs("A shortcut on the desktop"), &desktop);
  ImGui::Checkbox(trs("Copy saves and settings from an older version"), &import_saves);
  if (import_saves) {
    ImGui::Indent();
    MutedText(trs("Pick the older version's folder (the one with \"Mind over Recomp\" and the user "
                  "folder in it). Its files are copied, never moved."));
    path_row(trs("Older version"), &old_version, kPickOldVersion, true);
    ImGui::Unindent();
  }
  ImGui::Spacing();
  DrawSpeedOption();
  ImGui::Spacing();
  if (!choose_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", choose_error.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::Button(trs("Back"))) page = Page::kStart;
  ImGui::SameLine();
  if (AccentButton(trs("Next"), ImVec2(160 * ImGui::GetStyle().FontScaleDpi, 0))) {
    std::error_code ec;
    if (!disc_there && (iso.empty() || !fs::is_regular_file(fs::path(iso), ec))) {
      choose_error = trs("Pick your disc image (.iso) first.");
    } else if (import_saves && !fs::is_directory(fs::path(old_version) / "user", ec)) {
      choose_error = trs("That older version has no user folder: pick the folder the old "
                         "\"Mind over Recomp\" is in, or untick the copy.");
    } else if (const std::string why = install::WhyNotHere(root, NeededGb(), existing); !why.empty()) {
      choose_error = why;
    } else {
      page = Page::kTools;
      CheckTools();
    }
  }
}

// --- INSTALL, page 2 -----------------------------------------------------------

void App::CheckTools() {
  if (tools_checking.exchange(true)) return;
  if (tools_thread.joinable()) tools_thread.join();
  tools_thread = std::thread([this] {
    setup::Status status = setup::Setup::CheckToolsOnly(SourceNextToMe());
    std::lock_guard lock(tools_mutex);
    tools = std::move(status);
    tools_checking = false;
  });
}

void App::DrawToolsPage() {
  ImGui::PushFont(fonts.bold, 0.0f);
  if (ReadyBuilt()) {
    ImGui::TextUnformatted(trs("2. What the game needs"));
    ImGui::PopFont();
    MutedText(trs("Two parts of Windows the game runs on: Microsoft's Visual C++ runtime and the Vulkan "
                  "graphics library (it comes with your graphics driver). Most PCs have both; missing "
                  "ones are installed with winget, Windows' own app installer."));
  } else {
    ImGui::TextUnformatted(trs("2. Build tools"));
    ImGui::PopFont();
    MutedText(trs("Free tools that turn the game's code into a Windows program. Missing ones are "
                  "installed with winget, Windows' own app installer; Windows may ask for your "
                  "permission for some of them (Visual Studio's Build Tools, the Vulkan SDK)."));
    MutedText(trs("If it seems stuck, look at the taskbar: Windows' permission question (a shield) can "
                  "open behind this window. Visual Studio's Build Tools are a big download."));
  }
  ImGui::Spacing();
  tools_runner.Copy(tools_job);
  setup::Status status;
  {
    std::lock_guard lock(tools_mutex);
    status = tools;
  }
  const bool installing = tools_job.state == State::kRunning;
  if (tools_checking || !status.done) {
    ImGui::TextDisabled("%s", trs("Checking..."));
  } else {
    DrawTools(status);
  }
  ImGui::Spacing();
  const bool all = status.done && AllToolsFound(status);
  ImGui::BeginDisabled(installing || tools_checking);
  if (ImGui::Button(trs("Back"))) page = updating ? Page::kUpdate : Page::kChoose;
  ImGui::SameLine();
  if (all) {
    if (AccentButton(updating ? trs("Next: update") : trs("Next: install")) || auto_update) {
      auto_update = false;  // a "Try again" after a failure is the player's press
      BeginInstall();
    }
  } else if (status.done) {
    if (AccentButton(trs("Install the missing tools"))) {
      std::vector<Command> commands = setup::Setup::ToolsJob(status);
      if (platform::FindProgram("winget").empty()) {
        choose_error = trs("winget, Windows' own app installer, isn't there. Get \"App "
                           "Installer\" from Microsoft with the button below (some Windows "
                           "versions hide it in the Store, and searching for it finds fake "
                           "apps), install it, then press Check again.");
        winget_missing = true;
      } else if (commands.empty()) {
        choose_error = trs("What's missing can't be installed from here (see the list).");
      } else {
        choose_error.clear();
        winget_missing = false;
        show_log = true;
        tools_runner.Forget();
        std::error_code ec;
        tools_runner.Start(trs("Installing the build tools"), std::move(commands),
                           fs::temp_directory_path(ec) / ("MindOverRecomp-tools-" + Stamp() + ".log"),
                           [this](bool) { CheckTools(); });
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(trs("Check again"))) CheckTools();
  }
  ImGui::EndDisabled();
  if (!choose_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", choose_error.c_str());
    ImGui::PopStyleColor();
    if (winget_missing) {
      // Microsoft's own link: it downloads App Installer's package from
      // Microsoft's winget releases on GitHub (opened in the browser).
      if (ImGui::Button(trs("Download App Installer (from Microsoft)"))) {
        SDL_OpenURL(setup::kAppInstallerUrl);
      }
    }
  }
  if (tools_job.state != State::kIdle) {
    ImGui::Spacing();
    DrawJob(tools_job, fonts, &show_log);
    if (tools_job.state == State::kFailed) {
      MutedText(trs("If one keeps failing, install it by hand (its name is in the list), then "
                    "press Check again."));
      DrawReportHelp(tools_job.error, tools_job.log, "");
    }
  }
}

// --- INSTALL, page 3 -------------------------------------------------------------

bool RenameOldGameExe(const fs::path& root);  // (below, by HeadlessInstall)

void App::SetAwake(bool on) {
  if (awake == on) return;
  awake = on;
  platform::KeepAwake(on);
}

void App::BeginInstall() {
  page = Page::kInstalling;
  fail_text.clear();
  show_log = false;
  started = Clock::now();
  took_seconds = 0;
  setup::SetLimitedResources(limited);
  SetAwake(true);
  std::error_code ec;
  fs::create_directories(root / "user" / "logs", ec);
  if (!ec) install::MarkInstalled(root);
  if (ec) {
    phase = Phase::kFailed;
    fail_text = trf(trs("Couldn't make {0}: {1}"), {root.string(), ec.message()});
    return;
  }
  // The port's files: an existing game keeps its own (it's taken over as it
  // is), an update or a fresh install brings the ones next to this exe (the
  // release .zip). None here = refused (MissingFilesText): no download.
  const bool update_files = updating && !update_same;
  if ((existing && !update_files) || !SourceNextToMe().empty()) {
    StartFiles();
  } else {
    phase = Phase::kFailed;
    fail_text = MissingFilesText();
  }
}

// An update's first step: the current version kept in <root>\previous
// (update.h THE PREVIOUS VERSION), to put back if the update fails.
Command BackupCommand(const fs::path& root) {
  const std::string version = update::InstalledVersion(folders::Find(root));
  Command backup;
  backup.title = trs("Keeping the current version, in case the update fails");
  backup.function = [root, version](std::string* message) {
    return update::StartBackup(root, version.empty() ? "?" : version, message);
  };
  return backup;
}

// The port's files + this exe + (optionally) an older version's user\.
void App::StartFiles() {
  phase = Phase::kFiles;
  std::vector<Command> commands;
  backed_up = false;
  rollback_note.clear();
  if (updating && !update_same) {
    commands.push_back(BackupCommand(root));
    backed_up = true;
    // UPDATE: the new source\ swapped in, the build kept (update.h), from
    // the files next to this exe.
    RenameOldGameExe(root);
    commands = update::LocalUpdateJob(SourceNextToMe(), root);
  } else if (existing) {
    // Nothing: its source\ (and build) stay as they are; only an old
    // launcher's exe name is brought up to date.
    RenameOldGameExe(root);
  } else {
    const fs::path from = SourceNextToMe();
    const fs::path to = root / "source";
    Command copy;
    copy.title = trs("Copying the port's files");
    copy.function = [from, to](std::string* message) {
      std::error_code ec;
      fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      *message = ec ? "couldn't copy " + from.string() + ": " + ec.message() : "source\\ copied.";
      return !ec;
    };
    commands.push_back(std::move(copy));
  }
  if (auto program = ProgramCopyCommand(root)) {
    commands.push_back(std::move(*program));
    commands.push_back(PruneOldBuildCommand(root));
  }
  if (updating && !update_needs_disc && !iso.empty()) {
    // The update's disc check (after the swap: the new source's extractor).
    // With game\ gone, Setup's disc step extracts it all instead.
    commands.push_back(setup::DiscRepairCommand(fs::path(iso), root / "source", folders::Find(root).disc));
  }
  Command me;
  me.title = trs("Copying Mind over Recomp into the game's folder");
  me.function = [root = root](std::string* message) { return CopySelfInto(root, message); };
  commands.push_back(std::move(me));
  if (import_saves && !old_version.empty()) {
    const fs::path from = fs::path(old_version) / "user";
    const fs::path to = root / "user";
    Command saves;
    saves.title = trs("Copying the older version's saves and settings");
    saves.function = [from, to](std::string* message) {
      std::error_code ec;
      // skip_existing: nothing in the new folder is overwritten (it's empty
      // but for logs\ now); the old folder is only read.
      fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
      *message = ec ? "couldn't copy " + from.string() + ": " + ec.message()
                    : "copied from " + from.string();
      return !ec;
    };
    commands.push_back(std::move(saves));
  }
  // An existing game built before the icon existed: the icon from its disc
  // files now (a game that still needs building gets it from the build).
  if (existing) {
    const folders::Folders here = folders::Find(root);
    std::error_code ec;
    if (here.ExeExists() && here.DiscExists() && !here.source.empty() &&
        fs::is_regular_file(here.source / "tools" / "make_icon.py", ec) &&
        !fs::is_regular_file(root / "program" / "crash_mom.ico", ec)) {
      auto icon = setup::Setup::IconJob(here);
      commands.insert(commands.end(), icon.begin(), icon.end());
    }
  }
  files_runner.Forget();
  files_runner.Start(updating ? trs("Updating the port's files") : trs("Getting the port's files"), std::move(commands),
                     root / "user" / "logs" / ("install-" + Stamp() + ".log"),
                     [](bool) {});
}

void App::StartBuild() {
  phase = Phase::kBuild;
  folders = folders::Find(root);
  setup = std::make_unique<setup::Setup>(folders, feed, root / install::kInstallerName);
  was_setting_up = false;
}

// The last step: Windows' Apps list + the shortcuts (thread: measuring the
// folder's size for the Apps list takes a few seconds).
void App::StartFinish() {
  phase = Phase::kFinish;
  finishing = true;
  if (finish_thread.joinable()) finish_thread.join();
  finish_thread = std::thread([this] {
    install::Entry entry;
    entry.root = folders.root;
    entry.installer = folders.root / install::kInstallerName;
    entry.game_exe = folders.exe;
    entry.version = update::InstalledVersion(folders);
    std::string error;
    install::Register(entry, &error);
    std::string shortcut_error;
    install::WriteShortcuts(entry, desktop, &shortcut_error);
    finish_error = error.empty() ? shortcut_error : error;
    finishing = false;
  });
}

// Moves the install's phases on. Called every pass of the main loop, also
// while the window is minimized (it isn't drawn then).
void App::UpdateInstalling() {
  if (phase == Phase::kFiles) {
    files_runner.Copy(files_job);
    if (files_job.state == State::kDone) {
      StartBuild();
    } else if (files_job.state == State::kFailed || files_job.state == State::kCancelled) {
      phase = Phase::kFailed;
      fail_text = files_job.error.empty() ? std::string(trs("Stopped.")) : files_job.error;
    }
  }
  if (phase == Phase::kBuild && setup) {
    setup->Tick(false);
    const setup::Status s = setup->CurrentStatus();
    const auto& job = setup->JobSnapshot();
    if (!was_setting_up && s.done && !setup->IsChecking() && job.state == State::kIdle) {
      std::string error;
      if (setup->StartSetUp(fs::path(iso), &error)) {
        was_setting_up = true;
      } else {
        phase = Phase::kFailed;
        fail_text = error;
      }
    } else if (was_setting_up && !setup->SettingUp() && job.state != State::kRunning &&
               !setup->IsChecking()) {
      if (EverythingDone(s)) {
        StartFinish();
      } else if (job.state == State::kFailed || job.state == State::kCancelled) {
        phase = Phase::kFailed;
        fail_text = job.error.empty() ? std::string(trs("Stopped.")) : job.error;
      } else if (s.tools_state != setup::StepState::kDone) {
        phase = Phase::kFailed;
        fail_text = trs("A build tool went missing: go Back and check the tools again.");
      } else {
        phase = Phase::kFailed;
        fail_text = !setup->message().empty() ? setup->message()
                    : trs("Setting up stopped before the game was built.");
      }
    }
  }
  if (phase == Phase::kFinish && !finishing) {
    if (finish_thread.joinable()) finish_thread.join();
    phase = Phase::kFinished;
    page = Page::kDone;
    took_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    SetAwake(false);
    std::printf("installer: finished in %s\n", Duration(took_seconds).c_str());
  }
  if (phase == Phase::kFailed) {
    if (took_seconds == 0) took_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    SetAwake(false);
    // A failed update: the version it replaced goes back (once), so the
    // game plays as before; "Try again" then starts the update over.
    if (backed_up && (!setup || (!setup->SettingUp() && setup->JobSnapshot().state != State::kRunning))) {
      backed_up = false;
      const std::string version = update::PreviousVersion(root);
      std::string message;
      if (!version.empty() && update::Rollback(root, &message)) {
        rollback_note = trf(trs("The update didn't finish, so version {0} was put back: the game plays as "
                                "before."), {version});
      } else if (!version.empty()) {
        rollback_note = trs("The update didn't finish and the earlier version couldn't be put back: ") + message;
      }
      std::printf("installer: rollback: %s\n", message.c_str());
      setup.reset();
    }
  }
}

void App::DrawInstalling() {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(updating ? trs("3. Updating") : trs("3. Installing"));
  ImGui::PopFont();
  ImGui::TextDisabled("%s", root.string().c_str());
  const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
  if (phase != Phase::kFailed) {
    ImGui::TextDisabled(trs("Running for %s"), Duration(elapsed).c_str());
  }
  ImGui::Spacing();
  // A long build: what keeps it fast (the most common cause of very slow
  // installs is the PC doing something heavy at the same time).
  if (phase != Phase::kFailed && elapsed >= kSlowNoteSeconds) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.20f, 0.16f, 0.07f, 1.0f});
    ImGui::BeginChild("slow", ImVec2(-1, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", trs("This is taking a while: that's normal, building the game is heavy work."));
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", trs("To keep it fast, don't use the PC for anything heavy until it's done: no games, "
                                 "video editing, big downloads or many browser tabs. Keep a laptop plugged in. "
                                 "Minimizing this window is fine; closing it stops the build (the next try "
                                 "carries on where it stopped)."));
    if (limited) {
      ImGui::TextWrapped("%s", trs("\"Use fewer resources\" is on: the build gives way to other programs, so "
                                   "it takes longer."));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  if (phase == Phase::kFiles) {
    DrawJob(files_job, fonts, &show_log);
  } else if (phase == Phase::kBuild && setup) {
    const auto& job = setup->JobSnapshot();
    if (job.state == State::kIdle) {
      ImGui::TextUnformatted(trs("Checking what's there..."));
    } else {
      DrawJob(job, fonts, &show_log);
    }
    ImGui::Spacing();
    if (job.state == State::kRunning && ImGui::Button(trs("Stop"))) {
      setup->CancelJob();
    }
  } else if (phase == Phase::kFinish) {
    ImGui::TextUnformatted(trs("Adding the Start menu entries and the Apps list entry..."));
  } else if (phase == Phase::kFailed) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", fail_text.c_str());
    ImGui::PopStyleColor();
    ImGui::TextDisabled(trs("Stopped after %s"), Duration(took_seconds).c_str());
    ImGui::Spacing();
    if (!rollback_note.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kGood);
      ImGui::TextWrapped("%s", rollback_note.c_str());
      ImGui::PopStyleColor();
    } else {
      MutedText(trs("Nothing is lost: the next try carries on where this one stopped. The full "
                    "output is in the log below and in the game folder's user\\logs."));
    }
    ImGui::Spacing();
    if (AccentButton(trs("Try again"))) {
      fail_text.clear();
      started = Clock::now();
      took_seconds = 0;
      SetAwake(true);
      if (setup) {
        setup->ForgetJob();
        setup->Recheck();
        phase = Phase::kBuild;
        was_setting_up = false;
      } else {
        BeginInstall();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(trs("Open the game folder"))) OpenFolder(root);
    {
      fs::path log = files_job.log;
      if (setup && !setup->JobSnapshot().log.empty()) log = setup->JobSnapshot().log;
      DrawReportHelp(fail_text, log, update::InstalledVersion(folders::Find(root)));
    }
    if (setup && setup->JobSnapshot().state != State::kIdle) {
      ImGui::Spacing();
      DrawJob(setup->JobSnapshot(), fonts, &show_log);
    } else if (files_job.state != State::kIdle) {
      ImGui::Spacing();
      DrawJob(files_job, fonts, &show_log);
    }
  }
}

void App::DrawDone() {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(updating ? trf(trs("Done: Crash Mind over Mutant is updated to {0}"), {kVersion}).c_str()
                                  : trs("Done: Crash Mind over Mutant is installed"));
  ImGui::PopFont();
  ImGui::TextColored(kGood, trs("Finished in %s"), Duration(took_seconds).c_str());
  ImGui::Spacing();
  MutedText(trs("Start it from the Start menu (\"Crash Mind over Mutant\")" ));
  MutedText(trs("Updates and uninstalling: \"Mind over Recomp\" in the Start menu (it looks for new "
                "versions), or Windows' Installed apps."));
  if (!finish_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", finish_error.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  if (AccentButton(trs("Play"), ImVec2(200 * ImGui::GetStyle().FontScaleDpi, 0))) {
    install::StartDetached(folders.exe, folders.exe.parent_path());
  }
  ImGui::SameLine();
  if (ImGui::Button(trs("Open the game folder"))) OpenFolder(folders.root);
}

// --- MANAGE ------------------------------------------------------------------------

void App::DrawManage() {
  if (!setup) return;
  const setup::Status s = setup->CurrentStatus();
  const auto& job = setup->JobSnapshot();
  const bool busy = job.state == State::kRunning || setup->SettingUp();
  const std::string version = update::InstalledVersion(folders);

  ImGui::Text(trs("Installed: version %s"), version.empty() ? "?" : version.c_str());
  ImGui::TextDisabled("%s", folders.root.string().c_str());
  ImGui::Spacing();

  // A build / update ended: refresh the Apps list and the shortcuts (the
  // version and the size changed; a first build may not have made them).
  if (was_setting_up && !busy && !setup->IsChecking()) {
    was_setting_up = false;
    if (EverythingDone(s)) StartFinish();
  }
  if (busy) was_setting_up = true;
  if (phase == Phase::kFinish && !finishing) {
    if (finish_thread.joinable()) finish_thread.join();
    phase = Phase::kFinished;
    manage_note = trs("Ready to play.");
  }

  // An update applied: this program was replaced; the new one rebuilds.
  if (setup->UpdateApplied()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGood);
    ImGui::TextWrapped("%s", trs("The new version is in place. Mind over Recomp restarts now to build it."));
    ImGui::PopStyleColor();
    platform::Restart(setup->launcher(), {"--after_update"});
    return;  // (only reached if the restart failed)
  }

  // Updates (2026-11-12): the self-update (DrawManageUpdates).
  DrawManageUpdates(busy);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // The state of the install.
  if (!s.done || setup->IsChecking()) {
    ImGui::TextDisabled("%s", trs("Checking..."));
  } else if (busy) {
    DrawJob(job, fonts, &show_log);
    if (job.state == State::kRunning && ImGui::Button(trs("Stop"))) setup->CancelJob();
  } else {
    if (s.tools_state != setup::StepState::kDone) {
      ImGui::TextColored(kWarn, "%s", trs("Some build tools are missing:"));
      DrawTools(s);
      if (ImGui::Button(trs("Install the missing tools"))) {
        std::string why;
        if (!setup->StartToolsInstall(&why)) manage_note = why;
        show_log = true;
      }
      ImGui::Spacing();
    }
    if (EverythingDone(s)) {
      ImGui::TextColored(kGood, "%s", trs("The game is built and ready."));
    } else {
      ImGui::TextColored(kWarn, "%s", trs("The game needs building (an update, or an install that stopped)."));
      ImGui::BeginDisabled(GameRunning() || s.tools_state != setup::StepState::kDone);
      if (AccentButton(trs("Build it now"))) {
        std::string why;
        // The disc is extracted already in a finished install; an install
        // that stopped before that needs the .iso again (picked on the
        // install page): ask for it here only then.
        if (!setup->StartSetUp(fs::path(iso), &why)) manage_note = why;
        was_setting_up = true;
      }
      ImGui::EndDisabled();
      if (s.disc_state != setup::StepState::kDone) {
        ImGui::SameLine();
        if (ImGui::Button(trs("Pick the disc image..."))) {
          static const SDL_DialogFileFilter kIso[] = {{"Xbox 360 disc image (*.iso)", "iso"}};
          SDL_ShowOpenFileDialog(OnPicked, reinterpret_cast<void*>(intptr_t(kPickIso)),
                                 SDL_GetMouseFocus(), kIso, 1, nullptr, false);
        }
        ImGui::TextDisabled("%s", iso.empty() ? trs("(the disc's files aren't extracted yet)") : iso.c_str());
      }
    }
    if (job.state == State::kFailed) {
      ImGui::Spacing();
      DrawJob(job, fonts, &show_log);
      DrawReportHelp(job.error, job.log, version);
    }
    if (GameRunning()) {
      ImGui::TextColored(kWarn, "%s", trs("The game is running: close it to update or build."));
    }
  }
  if (!manage_note.empty()) {
    ImGui::TextDisabled("%s", manage_note.c_str());
  }

  // The bottom row.
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() -
                                    ImGui::GetStyle().WindowPadding.y));
  ImGui::BeginDisabled(busy);
  if (AccentButton(trs("Play")) && folders.ExeExists()) {
    install::StartDetached(folders.exe, folders.exe.parent_path());
  }
  ImGui::SameLine();
  if (ImGui::Button(trs("Open the game folder"))) OpenFolder(folders.root);
  ImGui::SameLine();
  if (ImGui::Button(trs("Uninstall..."))) {
    if (install::RelaunchFromTemp({"--uninstall", "--game_folder=" + folders.root.string()})) {
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
    } else {
      manage_note = trs("Couldn't start the uninstaller.");
    }
  }
  ImGui::EndDisabled();
}

int Finish(const folders::Folders& folders, bool desktop);  // (below, without a window)

// The Manage page's updates: a new version (Update now), the manual way when
// that isn't possible, and the previous version kept by the last update.
void App::DrawManageUpdates(bool busy) {
  const char* kReleases = "https://github.com/Maxilure/MindOver-Recomp/releases/latest";
  auto manual = [&] {
    MutedText(trs("The manual way: download the .zip from GitHub, unpack it, start Mind over Recomp.exe "
                  "from it and choose \"Update an installed game\". Saves and settings are kept."));
    if (ImGui::Button(trs("Open the releases page"))) SDL_OpenURL(kReleases);
  };
  self_update_runner.Copy(self_update_job);
  update::Release latest;
  bool available = false;
  std::string error;
  const bool known = updater && updater->Latest(&latest, &available, &error);
  const std::string installed = update::InstalledVersion(folders);
  // AN UNFINISHED UPDATE (update.h): finished like an update, from the
  // newest release (the same version, or a newer one). Right away when this
  // program was just put in by an old updater (--after_update).
  const bool unfinished = update::UnfinishedUpdate(folders);
  const bool can_finish = unfinished && known && !update::Newer(installed, latest.version) &&
                          latest.updater <= update::kUpdaterVersion;
  auto start_download = [&] {
    *new_installer = fs::path();
    self_update_runner.Forget();
    self_update_runner.Start(trf(trs("Downloading version {0}"), {latest.version}),
                             update::SelfUpdateJob(latest, folders.root, new_installer),
                             folders.root / "user" / "logs" / ("self-update-" + Stamp() + ".log"));
  };
  if (can_finish && finish_on_start && self_update_job.state == State::kIdle && !busy && !GameRunning()) {
    finish_on_start = false;
    start_download();
    self_update_runner.Copy(self_update_job);
  }
  // Downloaded and unpacked: the new installer takes over (this one quits).
  if (self_update_job.state == State::kDone && !new_installer->empty()) {
    std::vector<std::string> args = {"--update_window=" + folders.root.string()};
    if (!feed.empty()) args.push_back("--update_feed=" + feed);
    platform::Restart(*new_installer, args);
    self_update_runner.Forget();  // (only reached if it couldn't start)
    manage_note = trs("Couldn't start the new version's installer.");
  }
  if (self_update_job.state == State::kRunning) {
    DrawJob(self_update_job, fonts, &show_log);
    if (ImGui::Button(trs("Stop"))) self_update_runner.Cancel();
    return;
  }
  if (self_update_job.state == State::kFailed || self_update_job.state == State::kCancelled) {
    ImGui::TextColored(kBad, "%s", trs("The update couldn't be downloaded. Nothing was changed."));
    DrawJob(self_update_job, fonts, &show_log);
    if (ImGui::Button(trs("Try again"))) {
      self_update_runner.Forget();
      self_update_job = {};
    }
    manual();
    return;
  }
  if (unfinished) {
    ImGui::PushFont(fonts.bold, 0.0f);
    ImGui::TextColored(kWarn, "%s", trs("The update isn't finished: the game itself is still the earlier version."));
    ImGui::PopFont();
    if (can_finish) {
      MutedText(trs("It gets the rest from GitHub. Saves and settings stay."));
      ImGui::BeginDisabled(busy || GameRunning());
      if (AccentButton(trs("Finish the update"))) start_download();
      ImGui::EndDisabled();
      if (GameRunning()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s", trs("(close the game first)"));
      }
      return;
    }
    if (!updater || updater->checking()) {
      ImGui::TextDisabled("%s", trs("Looking for a new version..."));
      return;
    }
    manual();
    return;
  }
  if (!updater || updater->checking()) {
    ImGui::TextDisabled("%s", trs("Looking for a new version..."));
  } else if (!known) {
    ImGui::TextColored(kWarn, trs("Couldn't look for a new version: %s"), error.c_str());
    if (ImGui::Button(trs("Look again"))) updater->Check();
    ImGui::SameLine();
    if (ImGui::Button(trs("Open the releases page"))) SDL_OpenURL(kReleases);
  } else if (!available) {
    ImGui::TextColored(kGood, "%s", trs("This is the newest version."));
    ImGui::SameLine();
    if (ImGui::SmallButton(trs("Look again"))) updater->Check();
  } else if (latest.updater > update::kUpdaterVersion) {
    // A release this program can't install by itself (update.h THE
    // UPDATER'S PROMISE): only the manual way.
    ImGui::TextColored(kGood, trs("Version %s is out."), latest.version.c_str());
    MutedText(trs("It has to be installed the manual way this once (it changes how updates work)."));
    manual();
  } else {
    ImGui::PushFont(fonts.bold, 0.0f);
    ImGui::TextColored(kGood, trs("Version %s is out."), latest.version.c_str());
    ImGui::PopFont();
    MutedText(trs("Your saves, settings and the disc's files stay. The current version is kept: if the "
                  "update fails it's put back by itself."));
    ImGui::BeginDisabled(busy || GameRunning());
    if (AccentButton(trs("Update now"))) start_download();
    ImGui::EndDisabled();
    if (GameRunning()) {
      ImGui::SameLine();
      ImGui::TextColored(kWarn, "%s", trs("(close the game first)"));
    }
    if (!latest.notes.empty()) {
      ImGui::SameLine();
      if (ImGui::Button(show_notes ? trs("Hide what's new") : trs("What's new"))) show_notes = !show_notes;
      if (show_notes) {
        ImGui::BeginChild("notes", ImVec2(0, 160 * ImGui::GetStyle().FontScaleDpi), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", latest.notes.c_str());
        ImGui::EndChild();
      }
    }
  }

  // The previous version (update.h): for an update that plays badly.
  const std::string previous = update::PreviousVersion(folders.root);
  if (!previous.empty()) {
    ImGui::Spacing();
    if (!confirm_go_back) {
      MutedText(trf(trs("The version before this one ({0}) is kept."), {previous}).c_str());
      ImGui::SameLine();
      ImGui::BeginDisabled(busy || GameRunning());
      if (ImGui::SmallButton(trf(trs("Go back to {0}"), {previous}).c_str())) confirm_go_back = true;
      ImGui::EndDisabled();
    } else {
      ImGui::TextWrapped(trs("Go back to version %s? Saves and settings stay. Updating again later is "
                             "possible."), previous.c_str());
      if (AccentButton(trf(trs("Yes, go back to {0}"), {previous}).c_str())) {
        confirm_go_back = false;
        std::string message;
        if (update::Rollback(folders.root, &message)) {
          // The Apps list's version, then the old Mind over Recomp starts.
          Finish(folders::Find(folders.root), false);
          platform::Restart(folders.root / install::kInstallerName, {});
        }
        manage_note = message;
      }
      ImGui::SameLine();
      if (ImGui::Button(trs("No"))) confirm_go_back = false;
    }
  }
}

// --- UNINSTALL -----------------------------------------------------------------------

void App::StartUninstall() {
  uninstall_state = 1;
  uninstall_thread = std::thread([this] {
    std::string error;
    const fs::path root = folders.root;
    install::RemoveShortcuts(root);
    std::error_code ec;
    if (fs::equivalent(install::RegisteredRoot(), root, ec)) install::Unregister();
    install::ForgetCMakePackages(root);
    const bool ok = install::RemoveInstall(root, keep_user, [this](uint64_t n) { removed = n; }, &error);
    uninstall_error = error;
    uninstall_state = ok ? 2 : 3;
  });
}

void App::DrawUninstall() {
  ImGui::PushFont(fonts.bold, 0.0f);
  ImGui::TextUnformatted(trs("Uninstall Crash Mind over Mutant"));
  ImGui::PopFont();
  ImGui::TextDisabled("%s", folders.root.string().c_str());
  ImGui::Spacing();
  const int state = uninstall_state;
  if (state == 0) {
    MutedText(trs("This removes the game, the disc's extracted files and the shortcuts. Your disc "
                  "image (.iso) and the programs installed for the game (the Visual C++ runtime, "
                  "or an older version's build tools) stay."));
    ImGui::Spacing();
    ImGui::Checkbox(trs("Keep my saves, settings and logs"), &keep_user);
    ImGui::TextDisabled(keep_user ? trs("They stay in %s") : trs("%s goes too"),
                        (folders.root / "user").string().c_str());
    ImGui::Spacing();
    if (GameRunning()) {
      ImGui::TextColored(kWarn, "%s", trs("The game is running: close it first."));
    }
    ImGui::BeginDisabled(GameRunning());
    if (AccentButton(trs("Uninstall"))) StartUninstall();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(trs("Cancel"))) {
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
    }
  } else if (state == 1) {
    ImGui::Text(trs("Removing... %llu files"), static_cast<unsigned long long>(removed.load()));
  } else {
    if (uninstall_thread.joinable()) uninstall_thread.join();
    ImGui::TextColored(state == 2 ? kGood : kWarn, "%s",
                       state == 2 ? trs("Uninstalled.") : trs("Uninstalled, with problems:"));
    if (!uninstall_error.empty()) ImGui::TextWrapped("%s", uninstall_error.c_str());
    if (keep_user) {
      MutedText(trf(trs("Your saves, settings and logs are still in {0}. Installing into the same "
                        "place again picks them up."),
                    {(folders.root / "user").string()}).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button(trs("Close"))) {
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
    }
  }
}

void App::Update() {
  TakePicks();
  if (mode == Mode::kInstall && page == Page::kInstalling) UpdateInstalling();
  if (mode == Mode::kManage && setup) setup->Tick(GameRunning());
}

void App::Draw() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("installer", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
  DrawHeader();
  switch (mode) {
    case Mode::kInstall:
      switch (page) {
        case Page::kStart: DrawStart(); break;
        case Page::kChoose: DrawChoose(); break;
        case Page::kUpdate: DrawUpdate(); break;
        case Page::kTools: DrawToolsPage(); break;
        case Page::kInstalling: DrawInstalling(); break;
        case Page::kDone: DrawDone(); break;
      }
      break;
    case Mode::kManage: DrawManage(); break;
    case Mode::kUninstall: DrawUninstall(); break;
  }
  ImGui::End();
}

// =============================================================================
// Without a window (tests)
// =============================================================================

// Runs `commands`, printing their output; true on success.
bool RunPrinted(const std::string& title, std::vector<Command> commands, const fs::path& log) {
  task_runner::Runner runner;
  runner.Start(title, std::move(commands), log);
  task_runner::Snapshot view;
  size_t printed = 0;
  uint64_t seen = 0;
  while (true) {
    runner.Copy(view);
    if (view.version != seen) {
      seen = view.version;
      // Lines kept are the last N: print the new tail.
      if (view.lines.size() < printed) printed = 0;
      for (size_t i = printed; i < view.lines.size(); ++i) std::printf("%s\n", view.lines[i].c_str());
      printed = view.lines.size();
      std::fflush(stdout);
    }
    if (view.state != State::kRunning && view.state != State::kIdle) break;
    SDL_Delay(100);
  }
  if (view.state != State::kDone) std::printf("FAILED: %s\n", view.error.c_str());
  return view.state == State::kDone;
}

void PrintTools(const setup::Status& status) {
  for (const setup::Tool& tool : status.tools) {
    std::printf("  %-3s %-38s %s  [%s]\n", tool.found ? "OK" : "--", tool.label.c_str(),
                tool.detail.c_str(), tool.winget.empty() ? tool.package.c_str() : tool.winget.c_str());
  }
}

int Finish(const folders::Folders& folders, bool desktop) {
  install::Entry entry;
  entry.root = folders.root;
  entry.installer = folders.root / install::kInstallerName;
  entry.game_exe = folders.exe;
  entry.version = update::InstalledVersion(folders);
  std::string error;
  const bool ok = install::Register(entry, &error) && install::WriteShortcuts(entry, desktop, &error);
  std::printf("%s\n", ok ? "== registered: Apps list + shortcuts" : ("== " + error).c_str());
  return ok ? 0 : 1;
}

// An existing game set up by 0.1.0's launcher plays program\crash_mom.exe;
// since 0.2.0 it is "Crash Mind over Mutant.exe" (folders::InstalledExeName).
// Taken over, the old exe is renamed to the new name, so the setup sees the
// game as built instead of copying it out of the build again (2026-11-05).
// True when it renamed one.
bool RenameOldGameExe(const fs::path& root) {
  std::error_code ec;
  const fs::path old_exe = root / "program" / platform::ExeName("crash_mom");
  const fs::path new_exe = root / "program" / folders::InstalledExeName();
  if (old_exe == new_exe || !fs::is_regular_file(old_exe, ec) || fs::exists(new_exe, ec)) return false;
  fs::rename(old_exe, new_exe, ec);
  return !ec;
}

int HeadlessInstall(const fs::path& parent, const fs::path& iso, const std::string& feed, bool desktop) {
  // A game set up before, there: taken over as it is (as the window does).
  const fs::path existing_root = install::ExistingGameAt(parent);
  const bool existing = !existing_root.empty();
  const fs::path root = existing ? existing_root : parent / install::kFolderName;
  if (const std::string why = install::WhyNotHere(root, NeededGb(), existing); !why.empty()) {
    std::printf("refused: %s\n", why.c_str());
    return 1;
  }
  setup::Status tools = setup::Setup::CheckToolsOnly(SourceNextToMe());
  PrintTools(tools);
  if (!AllToolsFound(tools)) {
    std::printf("refused: build tools missing (--install_tools)\n");
    return 1;
  }
  std::error_code ec;
  fs::create_directories(root / "user" / "logs", ec);
  install::MarkInstalled(root);
  std::vector<Command> commands;
  if (existing) {
    std::printf("== a game set up before is in %s: used as it is\n", root.string().c_str());
    if (RenameOldGameExe(root)) std::printf("== its game renamed to %s\n", folders::InstalledExeName().c_str());
  } else if (const fs::path here = SourceNextToMe(); !here.empty()) {
    std::printf("== the port's files from %s\n", here.string().c_str());
    fs::copy(here, root / "source", fs::copy_options::recursive, ec);
    if (ec) {
      std::printf("copy failed: %s\n", ec.message().c_str());
      return 1;
    }
  } else {
    std::printf("refused: no source\\ next to this installer (unpack the release .zip)\n");
    return 1;
  }
  if (auto program = ProgramCopyCommand(root)) {
    if (!RunPrinted("program", {std::move(*program)}, root / "user" / "logs" / ("install-" + Stamp() + ".log"))) return 1;
  }
  if (std::string error; !CopySelfInto(root, &error)) std::printf("== %s\n", error.c_str());
  const folders::Folders folders = folders::Find(root);
  setup::Setup setup(folders, feed, root / install::kInstallerName);
  const int code = setup.RunHeadless("all", iso);
  std::fflush(stdout);
  if (code != 0) return code;
  if (existing && folders.ExeExists() && folders.DiscExists() && !fs::is_regular_file(root / "program" / "crash_mom.ico", ec)) {
    RunPrinted("icon", setup::Setup::IconJob(folders), root / "user" / "logs" / ("install-" + Stamp() + ".log"));
  }
  return Finish(folders, desktop);
}

}  // namespace

// =============================================================================

int main(int argc, char** argv) {
  std::string feed, iso, install_to, check_install_to, screenshot;
  fs::path update_window;            // --update_window=<install> (the self-update's hand-over)
  bool self_update = false;          // --self_update (the Manage page's Update now, without a window)
  fs::path override_root, embed_icon_exe, embed_icon_ico;
  bool uninstall = false, quiet = false, remove_user = false, after_update = false;
  bool check_tools = false, install_tools = false, do_update = false, desktop = false;
  double screenshot_after = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* key) { return arg.substr(std::strlen(key)); };
    if (arg.starts_with("--update_feed=")) feed = value("--update_feed=");
    else if (arg.starts_with("--iso=")) iso = value("--iso=");
    else if (arg.starts_with("--install_to=")) install_to = value("--install_to=");
    else if (arg.starts_with("--check_install_to=")) check_install_to = value("--check_install_to=");
    else if (arg.starts_with("--game_folder=")) override_root = value("--game_folder=");
    else if (arg.starts_with("--screenshot=")) screenshot = value("--screenshot=");
    else if (arg.starts_with("--screenshot_after=")) screenshot_after = std::atof(value("--screenshot_after=").c_str());
    else if (arg == "--uninstall") uninstall = true;
    else if (arg == "--quiet") quiet = true;
    else if (arg == "--remove_user") remove_user = true;
    else if (arg == "--after_update") after_update = true;
    else if (arg == "--check_tools") check_tools = true;
    else if (arg == "--install_tools") install_tools = true;
    else if (arg == "--update") do_update = true;
    else if (arg.starts_with("--update_window=")) update_window = value("--update_window=");
    else if (arg == "--self_update") self_update = true;
    else if (arg == "--desktop") desktop = true;
    else if (arg == "--limited") setup::SetLimitedResources(true);  // headless "use fewer resources"
    // Developer test of the setup's icon step on any exe (platform::EmbedIcon).
    else if (arg.starts_with("--embed_icon=")) embed_icon_exe = value("--embed_icon=");
    else if (arg.starts_with("--ico=")) embed_icon_ico = value("--ico=");
  }

  // Windows: never slowed down to save power, minimized or not (platform.h).
  platform::KeepFullSpeed();

  // Where the install is: --game_folder, else this exe's own folder when it
  // is an install's "Mind over Recomp.exe".
  const fs::path self = platform::SelfPath();
  if (override_root.empty() && install::LooksInstalled(self.parent_path())) {
    override_root = self.parent_path();
  }
  folders::Folders found;
  if (!override_root.empty()) found = folders::Find(override_root);
  texts::Init(found.user, found.exe.parent_path(), found.source);
  texts::AddBuiltIn("pt_BR", reinterpret_cast<const char*>(kPtBrPo));

  // --- without a window -----------------------------------------------------------
  if (!embed_icon_exe.empty()) {
    std::string error;
    const bool ok = platform::EmbedIcon(embed_icon_exe, embed_icon_ico, &error);
    std::printf("%s\n", ok ? "icon written" : error.c_str());
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
  }
  if (check_tools || install_tools) {
    setup::Status status = setup::Setup::CheckToolsOnly(SourceNextToMe());
    PrintTools(status);
    // What installs the missing tools (winget = App Installer): its path, or
    // "not found". Helps when a player says it's installed but the page says
    // it isn't (the app execution alias case, platform_win.cpp FindProgram).
    const fs::path winget = platform::FindProgram("winget");
    std::printf("  winget: %s\n", winget.empty() ? "not found" : winget.string().c_str());
    if (install_tools && !AllToolsFound(status)) {
      std::error_code ec;
      const bool ok = RunPrinted("tools", setup::Setup::ToolsJob(status),
                                 fs::temp_directory_path(ec) / ("MindOverRecomp-tools-" + Stamp() + ".log"));
      std::printf("== checking again\n");
      status = setup::Setup::CheckToolsOnly(SourceNextToMe());
      PrintTools(status);
      std::fflush(stdout);
      std::_Exit(ok && AllToolsFound(status) ? 0 : 1);
    }
    std::fflush(stdout);
    std::_Exit(AllToolsFound(status) ? 0 : 1);
  }
  // A dry run of --install_to: what an install into that folder would do
  // (take over a game set up before? the folder check, each setup step's
  // state). Changes nothing.
  if (!check_install_to.empty()) {
    const fs::path existing_root = install::ExistingGameAt(check_install_to);
    const bool existing = !existing_root.empty();
    const fs::path root = existing ? existing_root : fs::path(check_install_to) / install::kFolderName;
    std::printf("folder: %s\nexisting game: %s\n", root.string().c_str(), existing ? "yes (used as it is)" : "no");
    const std::string why = install::WhyNotHere(root, NeededGb(), existing);
    std::printf("folder check: %s\n", why.empty() ? "fine" : why.c_str());
    if (existing) {
      const folders::Folders here = folders::Find(root);
      std::printf("disc files: %s\n", here.DiscExists() ? "there (no .iso needed)" : "missing (needs the .iso)");
      setup::Setup check(here, feed, root / install::kInstallerName);
      check.RunHeadless("check", {});
    }
    std::fflush(stdout);
    std::_Exit(why.empty() ? 0 : 1);
  }
  if (!install_to.empty()) {
    const auto begin = Clock::now();
    const int code = HeadlessInstall(install_to, iso, feed, desktop);
    std::printf("== finished in %s\n", Duration(std::chrono::duration<double>(Clock::now() - begin).count()).c_str());
    std::fflush(stdout);
    std::_Exit(code);
  }
  if (self_update) {
    // The Manage page's "Update now" without a window (tests): the feed, the
    // download, then the NEW installer's --update (waited for here, its
    // output printed by it into its own console).
    if (found.root.empty() || !found.release) {
      std::printf("self_update: not an install (--game_folder)\n");
      std::_Exit(1);
    }
    update::Updater check(found, feed);
    check.Check();
    while (check.checking()) SDL_Delay(100);
    update::Release latest;
    bool available = false;
    std::string error;
    if (!check.Latest(&latest, &available, &error)) {
      std::printf("self_update: %s\n", error.c_str());
      std::_Exit(1);
    }
    std::printf("== installed %s, newest %s (updater %d, ours %d)\n", check.installed().c_str(),
                latest.version.c_str(), latest.updater, update::kUpdaterVersion);
    const bool unfinished = update::UnfinishedUpdate(found);
    if (unfinished) std::printf("== unfinished update: the game is %s\n", update::ProgramVersion(found.root).c_str());
    if (!available && !(unfinished && !update::Newer(check.installed(), latest.version))) std::_Exit(0);
    if (latest.updater > update::kUpdaterVersion) {
      std::printf("self_update: that release needs the manual way\n");
      std::_Exit(2);
    }
    auto exe = std::make_shared<fs::path>();
    if (!RunPrinted("download", update::SelfUpdateJob(latest, found.root, exe),
                    found.root / "user" / "logs" / ("self-update-" + Stamp() + ".log"))) {
      std::_Exit(1);
    }
    std::vector<std::string> argv2 = {exe->string(), "--update", "--game_folder=" + found.root.string()};
    if (!iso.empty()) argv2.push_back("--iso=" + iso);
    std::printf("== handing over to %s\n", exe->string().c_str());
    std::fflush(stdout);
    bool ok = false;
    const std::string out = task_runner::Capture(argv2, &ok, exe->parent_path(), 30 * 60 * 1000);
    std::printf("%s\n== new installer %s\n", out.c_str(), ok ? "succeeded" : "FAILED");
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
  }
  if (do_update) {
    // The window's Update route (App::DrawUpdate / StartFiles): this
    // installer's files swapped into the install, the build kept, then
    // everything that's left built.
    if (found.root.empty() || !found.release) {
      std::printf("update: not an install (--game_folder)\n");
      std::_Exit(1);
    }
    const auto begin = Clock::now();
    const std::string from = update::InstalledVersion(found);
    std::printf("== update %s -> %s in %s\n", from.c_str(), kVersion, found.root.string().c_str());
    if (!from.empty() && update::Newer(from, kVersion)) {
      std::printf("refused: the install is newer than this installer\n");
      std::_Exit(1);
    }
    const fs::path log = found.root / "user" / "logs" / ("update-" + Stamp() + ".log");
    // A failed update puts the version it replaced back (as the window does).
    const bool keeps = from != kVersion;
    auto failed = [&] {
      if (keeps) {
        std::string message;
        update::Rollback(found.root, &message);
        std::printf("== rollback: %s\n", message.c_str());
      }
      std::fflush(stdout);
      std::_Exit(1);
    };
    if (keeps && !RunPrinted("backup", {BackupCommand(found.root)}, log)) std::_Exit(1);
    if (from != kVersion) {
      RenameOldGameExe(found.root);
      const fs::path here = SourceNextToMe();
      if (here.empty()) {
        std::printf("refused: no source\\ next to this installer (unpack the release .zip)\n");
        std::_Exit(1);
      }
      if (!RunPrinted("update", update::LocalUpdateJob(here, found.root), log)) failed();
    }
    if (auto program = ProgramCopyCommand(found.root)) {
      if (!RunPrinted("program", {std::move(*program)}, log)) failed();
      RunPrinted("prune", {PruneOldBuildCommand(found.root)}, log);
    }
    if (std::string error; !CopySelfInto(found.root, &error)) std::printf("== %s\n", error.c_str());
    std::error_code ec;
    const folders::Folders now = folders::Find(found.root);
    const bool had_disc = now.DiscExists();
    int code = setup::Setup(now, feed, now.root / install::kInstallerName).RunHeadless("all", iso);
    // The disc check (the window's update does it before the build; here
    // after it: the build needs only default.xex, and Setup's checks found
    // Python by now). Skipped without --iso, or when Setup extracted it all.
    if (code == 0 && had_disc && !iso.empty() &&
        !RunPrinted("disc", {setup::DiscRepairCommand(fs::path(iso), now.source, now.disc)}, log)) {
      code = 1;
    }
    if (code != 0) failed();
    code = Finish(now, false);
    std::printf("== finished in %s\n", Duration(std::chrono::duration<double>(Clock::now() - begin).count()).c_str());
    std::fflush(stdout);
    std::_Exit(code);
  }
  if (uninstall && quiet) {
    if (found.root.empty()) {
      std::printf("uninstall: no install found (--game_folder)\n");
      std::_Exit(1);
    }
    install::RemoveShortcuts(found.root);
    std::error_code ec;
    if (fs::equivalent(install::RegisteredRoot(), found.root, ec)) install::Unregister();
    install::ForgetCMakePackages(found.root);
    std::string error;
    const bool ok = install::RemoveInstall(found.root, !remove_user, {}, &error);
    std::printf("%s %s\n", ok ? "uninstalled" : "uninstalled with problems:", error.c_str());
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
  }
  // The uninstall window runs from a copy in %TEMP% (install.h).
  if (uninstall && !install::RunningFromTemp() && !found.root.empty()) {
    if (install::RelaunchFromTemp({"--uninstall", "--game_folder=" + found.root.string()})) {
      std::_Exit(0);
    }
  }

  // --- the window -------------------------------------------------------------------
  SDL_SetAppMetadata("Mind over Recomp", kVersion, "mind_over_recomp");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "installer: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  const std::string title = std::string("Mind over Recomp ") + kVersion;
  SDL_Window* window = SDL_CreateWindow(
      title.c_str(), int(860 * scale), int(680 * scale),
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "installer: no window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetWindowMinimumSize(window, int(640 * scale), int(480 * scale));
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    std::fprintf(stderr, "installer: no renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ApplyStyle(scale);

  // The window's language: the game's text language when this is an install
  // whose game has one set, else Windows' display language (see above).
  // (Without a window, above: English, like every log.)
  if (texts::Code().empty()) texts::SetLanguage(WindowsLanguage());

  auto app = std::make_unique<App>();
  app->fonts = LoadFonts();
  app->feed = feed;
  if (!iso.empty()) app->iso = iso;
  {
    // Windows: an update renamed the previous installer aside (update.cpp).
    std::error_code ec;
    fs::path aside = self;
    aside += ".old";
    fs::remove(aside, ec);
  }
  if (!update_window.empty()) {
    // The self-update's hand-over (update.h SelfUpdateJob): this new
    // installer updates that install, pressing Next by itself.
    app->mode = Mode::kInstall;
    app->updating = true;
    app->auto_update = true;
    app->update_folder = update_window.string();
    app->update_checked_for = "\x01";
    app->page = Page::kUpdate;
  } else if (uninstall && !found.root.empty()) {
    app->mode = Mode::kUninstall;
    app->folders = found;
  } else if (!found.root.empty() && found.release) {
    app->mode = Mode::kManage;
    app->folders = found;
    app->setup = std::make_unique<setup::Setup>(found, feed, found.root / install::kInstallerName,
                                                after_update);
    app->was_setting_up = after_update;
    app->finish_on_start = after_update;
    app->desktop = false;  // a refresh never adds a desktop shortcut the player didn't keep
    // A finished self-update's download (its installer has quit by now, or
    // it's still open: then what's in use stays until the next start).
    std::error_code ec;
    fs::remove_all(found.root / update::kDownloadFolder, ec);
    app->updater = std::make_unique<update::Updater>(found, feed);
    app->updater->Check();
  }

  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  bool running = true;
  int frames = 0;
  const Clock::time_point launched = Clock::now();
  while (running) {
    SDL_Event event;
    // Sleep until something happens, or 4 times a second (jobs' progress).
    if (SDL_WaitEventTimeout(&event, 250)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
    }
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
    }
    // The steps move on whether or not the window is drawn (App::Update).
    app->Update();
    if (app->quit) running = false;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app->Draw();
    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 23, 23, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (!screenshot.empty() && ++frames >= 4 &&
        Clock::now() - launched >= std::chrono::duration<double>(screenshot_after)) {
      if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
        SDL_SavePNG(shot, screenshot.c_str());
        SDL_DestroySurface(shot);
      }
      running = false;
    }
    SDL_RenderPresent(renderer);
    if (app->quit) running = false;  // e.g. the installed copy was opened instead
  }

  // Closing during a build stops it (its processes end with this program).
  const bool from_temp = app->mode == Mode::kUninstall && install::RunningFromTemp();
  app.reset();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (from_temp) install::DeleteSelfAfterExit();
  std::_Exit(0);
}
