// =============================================================================
// desktop_entry.h -- the launcher in the desktop's applications menu
// =============================================================================
//
// One button on the Play tab writes (or removes) a standard Linux menu entry
// (freedesktop.org "Desktop Entry" file):
//
//   $XDG_DATA_HOME/applications/crash_mom_launcher.desktop
//   (~/.local/share/applications/ when XDG_DATA_HOME isn't set)
//
// The file name matches the launcher's app id ("crash_mom_launcher",
// SDL_SetAppMetadata in main.cpp), which is how Wayland desktops tie the
// running window to the entry (taskbar icon, grouping). It says:
//   Name   Crash: Mind over Mutant
//   Exec   the launcher, by its full path (quoted as the spec asks)
//   Icon   the port's own picture if there is one (see IconFile), else the
//          desktop's standard "applications-games" icon
//   one extra action (right-click the entry): "Continue last save" =
//   the launcher with --play=last (straight into the most recent save)
//
// FIRST START: the launcher adds the entry by itself the first time it runs
// (AddOnFirstStart), and notes that in user/launcher.toml
// (`menu_entry_added = true`), so an entry the player removed stays removed.
//
// The entry holds the launcher's path: after moving the game folder, the
// button says the entry is out of date and rewrites it.
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace desktop_entry {

enum class Status { kMissing, kCurrent, kOutdated };

// Whether our entry exists and points at `launcher` (the running exe).
Status Check(const std::filesystem::path& launcher);

// Writes the entry (see above). False + `error` on failure.
bool Write(const std::filesystem::path& launcher, const std::filesystem::path& game_folder,
           std::string* error);

// Removes it (nothing to do if it isn't there).
bool Remove(std::string* error);

// The first-start step (see above): adds the entry unless user/launcher.toml
// says it was done before; true if it added it now.
bool AddOnFirstStart(const std::filesystem::path& launcher,
                     const std::filesystem::path& game_folder,
                     const std::filesystem::path& user_folder);

// Where the entry goes.
std::filesystem::path EntryPath();

// The port's own icon if there is one: <game folder>/assets/icon/crash_mom.png
// (a build from source) or <game folder>/program/assets/icon/crash_mom.png
// (installed). Original art only, never a picture from the game (rule of the
// repo's assets/ folder). Empty when there is none.
std::filesystem::path IconFile(const std::filesystem::path& game_folder);

}  // namespace desktop_entry
