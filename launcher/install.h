// =============================================================================
// install.h -- the Windows installer's own pieces: the Apps list, shortcuts,
//              uninstalling
// =============================================================================
//
// The installer ("Mind over Recomp.exe", installer.cpp) builds the game on the
// player's PC (setup.h does the building). This file is what makes the result
// a normal Windows app:
//
// THE FOLDER (the player picks where; we always make a folder of our own in
// it, so an uninstall never touches anything that was there before):
//   <picked>\Crash Mind over Mutant\
//     Mind over Recomp.exe        this installer: updates, rebuilds, uninstalls
//     source\                     the port's code + the SDK + the build
//     game\                       the disc's files (extracted from the .iso)
//     program\Crash Mind over Mutant.exe   THE GAME (+ its DLLs and assets)
//     user\                       saves, settings, controls, logs, photos
//     cache\                      shader cache, patched game data
//
// THE APPS LIST ("Installed apps" in Settings, and the old Control Panel):
//   HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\
//   MindOverRecomp: name, version, size, the game's icon, and the uninstall
//   command ("Mind over Recomp.exe" --uninstall). Per user: no administrator
//   rights needed, like the rest of the install.
//
// SHORTCUTS (Start menu, %APPDATA%\Microsoft\Windows\Start Menu\Programs):
//   Crash Mind over Mutant.lnk   the game
//   Mind over Recomp.lnk         this installer (updates, uninstall)
//   + optionally "Crash Mind over Mutant.lnk" on the desktop.
//
// UNINSTALLING removes ONLY the entries listed under THE FOLDER above (plus
// the leftovers of an interrupted update / install), then the folder itself
// if that left it empty: a disc image the player put in there survives.
// "Keep my saves, settings and logs" keeps user\. Links inside the folder
// (source\game is a junction to game\) are removed as links, never followed.
// Long paths (the SDK's libraries nest deep) and read-only files (git's own
// files) are handled: plain std::filesystem::remove_all fails on both.
// A running program can't delete its own file, so the uninstall first copies
// the installer to %TEMP% and continues from there; that copy deletes itself
// a few seconds after it quits.
//
// Linux: none of this (the port's Linux setup will be a terminal tool); the
// functions are stubs there so the installer still compiles for UI work.
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace install {

// Names (see the header).
constexpr const char* kFolderName = "Crash Mind over Mutant";   // made inside the picked folder
constexpr const char* kInstallerName = "Mind over Recomp.exe";  // the installer, inside it
constexpr const char* kAppsKey = "MindOverRecomp";              // the Apps list's key
constexpr const char* kMarkerName = "install.txt";              // "this folder is an install"

// Where the folder picker starts: %USERPROFILE%\Games (made if missing).
std::filesystem::path DefaultParent();

// Why a folder can't be installed into ("" = fine): not writable (Program
// Files, a CD), too long a path (the SDK's files nest deep: Windows' 260-
// character limit), not enough free space, already holds something.
std::string WhyNotHere(const std::filesystem::path& root, double needed_gb);

// --- the Apps list -----------------------------------------------------------

struct Entry {
  std::filesystem::path root;       // the game folder
  std::filesystem::path installer;  // root\Mind over Recomp.exe
  std::filesystem::path game_exe;   // root\program\Crash Mind over Mutant.exe (its icon)
  std::string version;
};
// Writes (or rewrites) the entry; the size is measured here (seconds).
bool Register(const Entry& entry, std::string* error);
void Unregister();
// The folder the entry points at ("" if none).
std::filesystem::path RegisteredRoot();

// --- shortcuts ----------------------------------------------------------------

bool WriteShortcuts(const Entry& entry, bool desktop, std::string* error);
// Removes our shortcuts that point into `root` (not other installs' ones).
void RemoveShortcuts(const std::filesystem::path& root);

// --- uninstalling ---------------------------------------------------------------

// Is `root` one of our install folders (the installer + the marker below, or
// a built game in program\)? Opens the MANAGE page; the uninstall refuses
// anything else.
bool LooksInstalled(const std::filesystem::path& root);
// Writes the marker (root\install.txt) when an install starts.
void MarkInstalled(const std::filesystem::path& root);

// Removes the install (see the header). `progress(files)` is called now and
// then with the number of files removed so far. False + error if something
// couldn't be removed (the rest is still removed).
bool RemoveInstall(const std::filesystem::path& root, bool keep_user,
                   const std::function<void(uint64_t)>& progress, std::string* error);

// The SDK's build registers itself in CMake's per-user package list
// (HKCU\Software\Kitware\CMake\Packages\rexglue); entries pointing into
// `root` are removed.
void ForgetCMakePackages(const std::filesystem::path& root);

// Copies the running installer to %TEMP% and starts it there with `args`
// (the uninstall continues from that copy). False if that failed.
bool RelaunchFromTemp(const std::vector<std::string>& args);
// True when this installer runs from %TEMP% (the copy above).
bool RunningFromTemp();
// The copy in %TEMP% deletes itself a few seconds after it quits.
void DeleteSelfAfterExit();

// Starts a program on its own (not tied to the installer: the game).
bool StartDetached(const std::filesystem::path& exe, const std::filesystem::path& work_folder);

}  // namespace install
