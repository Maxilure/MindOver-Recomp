// =============================================================================
// folders.h -- where the game, its disc files and the player's files are
// =============================================================================
//
// The launcher ships on its own, so it has to FIND the game it starts. Two
// layouts exist (the game's own src/game_folder.h decides the same way):
//
//   RELEASE (a downloaded .tar.gz)      DEVELOPER (a git clone of this repo)
//   <game folder>/                      <repo>/
//     Crash Mind over Mutant  launcher    out/build/launcher/crash_mom_launcher
//     READ ME FIRST.txt
//     source/      the port's code        (the repo itself)
//     program/crash_mom  the built game,  out/build/linux-amd64-relwithdebinfo/crash_mom
//                  copied there by Setup
//     game/        (extracted disc)       game/
//     user/        (saves, settings...)   user/
//
// Search: starting at the launcher's own folder and going up, the first
// folder that has "source/crash_mom_manifest.toml" (release),
// "crash_mom_manifest.toml" (developer) or just "program/crash_mom" (an
// installed copy without sources: plays, can't build) is the game folder. `--game_folder=<path>` on the
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
  std::filesystem::path source;    // the port's source tree (release: root/source,
                                   // developer: root; empty = none)
  std::filesystem::path exe;       // the game's executable (may not exist yet)
  std::filesystem::path disc;      // the extracted disc files (game/)
  std::filesystem::path user;      // the player's files (user/)
  bool from_source = false;        // there are sources to build from (release or developer)
  bool release = false;            // the release layout (see above)

  // Where a build puts the game: <source>/out/build/linux-amd64-relwithdebinfo/crash_mom.
  std::filesystem::path BuiltExe() const;

  bool ExeExists() const;
  bool DiscExists() const;         // game/default.xex is there
};

// Finds the folders (see above). `override_root` = --game_folder, or empty.
Folders Find(const std::filesystem::path& override_root);

// "~/Documents/..." style, for showing a path to the player.
std::string Pretty(const std::filesystem::path& path);

}  // namespace folders
