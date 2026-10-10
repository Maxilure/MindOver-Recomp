// =============================================================================
// game_folder.h -- everything the player owns lives INSIDE the game's folder
// =============================================================================
//
// The port is "portable": saves, settings, controls, photos and logs sit in
// the game's own folder instead of being scattered across the system
// (~/.local/share on Linux, %APPDATA% on Windows). Copying the folder moves
// the whole game, progress included; deleting it removes the game.
//
// Layout (the "game folder" is the folder the player installed into):
//
//   <game folder>/
//     program/        crash_mom(.exe), libraries, shaders, assets/
//                     (replaced by every update)
//     user/           THE PLAYER'S FILES: updates never touch this folder
//       saves/        the saves, CrashMOM GameSlot N.sav (saves/save_files.h:
//                     the PC save drive's files, Backups/, Deleted saves/,
//                     the save list's play order); also the SDK's "user data"
//                     (achievements; before 2026-10-10 the emulated profile's
//                     <profile>/565507FA/00000001/..., copied over once)
//       settings.toml every changed setting (was crash_mom.toml next to the
//                     exe; the SDK's F4 menu saves here)
//       controls.toml keyboard + mouse keys and which device is which player
//       markers/      the player's own player 3/4 marker pictures (optional)
//       photos/       F10 photos
//       logs/         a log per session (session_log.h: header, event logs,
//                     oldest deleted past --logs_budget_mb)
//       reports/      the launcher's "Report a problem" .zip files
//     cache/          shader cache + patched game data (rebuilt when missing)
//
// Which folder is the game folder (decided from where the exe is):
//   1. the exe sits in a folder named "program" -> its parent (installed);
//   2. the exe sits inside this source tree     -> the source tree (a build
//      from source: <repo>/user/ and <repo>/cache/, both gitignored, so a
//      wiped build folder (out/) never takes the saves with it);
//   3. anything else                            -> the exe's own folder.
//
// Command-line flags still win (tests use them on purpose): --user_data_root
// (saves), --cache_root, --controls_file, --photo_dir, --log_file.
//
// FIRST START (one time): when user/saves/ doesn't exist yet, the saves from
// the old system folder (~/.local/share/crash_mom) are COPIED in, file dates
// kept (the save list orders saves by them); settings and controls next to
// the exe likewise. The originals are left where they were, as a backup.
//
// The game folder must be writable (e.g. not C:\Program Files): if it isn't,
// the game says so in a message box and quits, instead of quietly saving
// somewhere else (saves in two places = lost progress later).
// =============================================================================

#pragma once

#include <filesystem>

#include <rex/rex_app.h>

namespace game_folder {

// Fills in the folders (called from CrashMomApp::OnConfigurePaths, BEFORE
// logging exists: what it did is logged later by LogWhatHappened). Quits the
// program with a message box if the game folder can't be written to.
void Configure(rex::PathConfig& paths);

// Points the per-run log files (no --log_file given) into user/logs.
void ConfigureLogging(rex::LogConfig& config);

// Logs the chosen folders and any first-start copies (after logging is up).
void LogWhatHappened();

// <game folder>/user: settings, controls, photos, markers (see above).
std::filesystem::path UserFolder();

// The saves folder (user/saves, or --user_data_root for test runs on a copy),
// absolute. Empty before Configure ran.
std::filesystem::path SavesFolder();

}  // namespace game_folder
