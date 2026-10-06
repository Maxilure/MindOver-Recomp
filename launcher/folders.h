// =============================================================================
// folders.h -- where the game, its disc files and the player's files are
// =============================================================================
//
// The launcher ships on its own, so it has to FIND the game it starts. Two
// layouts exist (the game's own src/game_folder.h decides the same way):
//
//   INSTALLED                          BUILT FROM SOURCE (this repo)
//   <game folder>/                     <repo>/
//     crash_mom_launcher                 out/build/launcher/crash_mom_launcher
//     program/crash_mom                  out/build/linux-amd64-relwithdebinfo/crash_mom
//     game/      (extracted disc)        game/
//     user/      (saves, settings...)    user/
//
// Search: starting at the launcher's own folder and going up, the first
// folder that has "program/crash_mom" (installed) or "crash_mom_manifest.toml"
// (the source tree) is the game folder. `--game_folder=<path>` on the
// launcher's command line skips the search.
//
// The extracted disc is <game folder>/game (what tools/xiso_extract.py
// writes); the player's files are <game folder>/user. Saves:
// user/saves/<profile id>/565507FA/00000001/ (saves.h).
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace folders {

struct Folders {
  std::filesystem::path root;      // the game folder (empty = not found)
  std::filesystem::path exe;       // the game's executable (may not exist yet)
  std::filesystem::path disc;      // the extracted disc files (game/)
  std::filesystem::path user;      // the player's files (user/)
  bool from_source = false;        // the source tree layout (see above)

  bool ExeExists() const;
  bool DiscExists() const;         // game/default.xex is there
};

// Finds the folders (see above). `override_root` = --game_folder, or empty.
Folders Find(const std::filesystem::path& override_root);

// "~/Documents/..." style, for showing a path to the player.
std::string Pretty(const std::filesystem::path& path);

}  // namespace folders
