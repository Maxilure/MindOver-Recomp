// =============================================================================
// setup.h -- the launcher's Setup tab: from a disc image to a playable game
// =============================================================================
//
// The steps of docs/01-building.md, each with a check (is it done?) and,
// where it only touches the game's own folder, a button that does it:
//
//   1. Build tools   clang 20+, CMake 3.25+, Ninja, Python 3, Git, glslc,
//                    pkg-config, the GTK 3 headers, the Vulkan loader.
//                    ONLY CHECKED AND LISTED: each missing one with the name
//                    of the package that provides it on this distro (Arch,
//                    Debian/Ubuntu, Fedora, openSUSE: /etc/os-release). The
//                    launcher never installs system packages, and doesn't
//                    hand out install commands either: the player installs
//                    them their own way.
//   2. Source parts  the SDK submodule and its dependencies
//                    (git submodule update --init --recursive)
//   3. Disc          game/default.xex there? Else an .iso found in the game
//                    folder (or picked), checked with the extractor's --list
//                    (a 360 disc that has default.xex), extracted into game/
//                    (tools/xiso_extract.py), progress = files done / files
//   4. ReXGlue SDK   our patches applied to the submodule, checked as a stack
//                    (git apply --check, all reversed / all forward; 0013
//                    builds on 0009): all or none, a partly patched tree is
//                    reported, not touched; then
//                    configured and installed (Release + RelWithDebInfo) into
//                    thirdparty/rexglue-install; "out of date" when a patch
//                    is newer than the installed runtime
//   5. The game      configure, recompile (codegen), configure again (first
//                    time), compile with -j from the RAM FREE at the start
//                    (2.5 GB per job; the SDK 1 GB per job);
//                    "out of date" when src/, the manifest or the SDK is newer
//                    than the exe. Refused while the game runs (the build
//                    replaces the libraries a running game has loaded).
//
// "Set up everything" runs whatever is left of 2-5 in order. Every run's
// output goes to the tab (live) and user/logs/setup-<date>_<time>.log.
// Works from the source tree only (a git clone); an installed copy has no
// sources to build.
//
// Checks run on a thread of their own (a few git / tool version calls) at
// start, after every job, and on "Check again".
// =============================================================================
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "folders.h"
#include "task_runner.h"
#include "update.h"

namespace setup {

struct Look {
  ImFont* bold = nullptr;
  ImFont* mono = nullptr;
  ImVec4 muted, good, warn, bad, accent;
};

enum class StepState { kChecking, kDone, kOutdated, kMissing, kBlocked };

// One build tool's check (step 1).
struct Tool {
  std::string label;       // "clang 20 or newer"
  bool found = false;
  std::string detail;      // "22.1.8" / "not found" / "too old: 18.1"
  std::string package;     // the package that provides it on this distro
};

// What the checks found (copied as a whole between threads).
struct Status {
  bool done = false;                      // the checks have run
  std::vector<Tool> tools;
  std::string distro;                     // "CachyOS"
  StepState tools_state = StepState::kChecking;
  StepState source_state = StepState::kChecking;
  StepState disc_state = StepState::kChecking;
  StepState sdk_state = StepState::kChecking;
  StepState game_state = StepState::kChecking;
  std::string source_detail, disc_detail, sdk_detail, game_detail;
  std::vector<std::filesystem::path> isos;  // .iso files in the game folder
  int patches_total = 0, patches_applied = 0;
  bool sdk_needs_reset = false;  // release: patched by an older set (setup.cpp)
  // Windows: Visual Studio's x64 build environment (vcvars64.bat), given to
  // every build step; null on Linux (the launcher's own environment).
  std::shared_ptr<const platform::Environment> build_env;
};

class Setup {
 public:
  // `feed` = the update feed ("" = GitHub's, update.h); `launcher` = this
  // program's file (an update replaces it); `after_update` = started by an
  // update's restart: run "Set up everything" once the checks are done.
  Setup(folders::Folders folders, std::string feed = {}, std::filesystem::path launcher = {},
        bool after_update = false);
  ~Setup();

  // True when the game can't be played yet (the launcher opens on this tab).
  bool NeedsAttention();

  // Draws the tab. `game_running`: building is refused meanwhile.
  void Draw(const Look& look, bool game_running);

  // Every frame, whichever tab is open: carries "Set up everything" on (after
  // the SDK download, and the rebuild after an update's restart). It used to
  // run only while this tab was drawn: switching to Play during the first
  // checks after an update left the game unbuilt (0.1.0 -> 0.1.1 test).
  void Tick(bool game_running);

  // The game is built but older than its sources (e.g. right after an
  // update) or a build is running: the Play page says so.
  bool GameNeedsBuild();
  bool Building();

  // An update is out (release layout, after the quiet check at start).
  bool UpdateAvailable(std::string* version);

  // A job from the command line (--setup_run=<step>, for tests): runs it
  // without a window, printing its output; returns the exit code.
  int RunHeadless(const std::string& step, const std::filesystem::path& iso);

 private:
  void Check();                             // starts the checks (thread)
  Status RunChecks();                       // the checks themselves
  std::vector<task_runner::Command> SourceJob();
  bool DiscJob(const std::filesystem::path& iso, std::vector<task_runner::Command>* out,
               std::string* error);
  bool SdkJob(std::vector<task_runner::Command>* out, std::string* error);
  std::vector<task_runner::Command> GameJob();
  void StartJob(const std::string& title, std::vector<task_runner::Command> commands);
  void SetUpEverything(const Status& status, const std::filesystem::path& iso);
  void DrawUpdates(bool busy);
  int RunHeadlessJob(const std::string& step, std::vector<task_runner::Command> commands);
  void UseBuildEnvironment(std::vector<task_runner::Command>& commands);
  void DrawStep(int number, const char* title, StepState state, const std::string& detail);
  void DrawJob();

  folders::Folders folders_;
  std::mutex mutex_;
  Status status_;
  std::thread checker_;
  std::atomic<bool> checking_{false};
  task_runner::Runner runner_;
  task_runner::Snapshot job_;
  std::filesystem::path iso_;               // the picked disc image
  std::string iso_error_;
  std::string message_;                     // last refusal / note
  const Look* look_ = nullptr;
  bool follow_ = true;
  std::atomic<bool> continue_setup_{false};  // run "Set up everything" again when idle
  std::unique_ptr<update::Updater> updater_;  // release layout only
  std::filesystem::path launcher_;
  bool update_applied_ = false;               // restart to finish
  update::Release pending_;                   // the update being applied
};

}  // namespace setup
