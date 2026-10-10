// =============================================================================
// saves.h -- the player's saves, read straight from the files
// =============================================================================
//
// The Play page lists the saves so the player can jump straight into one
// (the game's --load_save=<number>, src/saves/quick_load.h). The launcher
// reads the files itself; the game isn't running at that point.
//
// WHERE (the game's src/saves/save_files.h has the full story):
//   user/saves/CrashMOM GameSlot N.sav
// N = the save's number (1, 2, 3, ...: the game's save list goes past 3).
// Before 2026-10-10 the saves sat in the emulated Xbox's folders,
//   user/saves/<profile id>/565507FA/00000001/CrashMOM GameSlot N/CrashMOM GameSlot N
// and the game copies them into the flat layout once, at its first start after
// the update: until then (no .sav files, no copied-from-xbox-layout.txt) the
// list comes from the old folders.
//
// WHAT WE READ from a save file (big-endian, docs/findings/24 section 1):
//   +0   u32   total size (20,205 bytes)
//   +4   the game's save block; its first 104 bytes are the Load Game
//        screen's summary:
//          block +12  saved at: u16 year, u8 month, day, hour, minute, second
//          block +20  the name typed at New Game: 32 UTF-16 characters
//          block +88  play time in seconds (float)
//          block +92  progress in % (u32)
//          block +96  difficulty (u32: 0 easy, 1 normal, 2 hard)
//
// LAST PLAYED (the list's order, the same as the game's save list): the later
// of the file's time (= last saved) and its line in
// user/saves/save_library_played.txt (old layout: <profile>/565507FA/...;
// "N <ticks>", written by the
// game when a save is loaded; ticks = std::filesystem file-clock counts).
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace saves {

struct Save {
  int number = 0;               // N: what --load_save=<N> takes
  std::string name;             // as typed in the game (UTF-8 here)
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;  // when it was saved
  float play_seconds = 0;
  int percent = 0;
  int difficulty = 0;           // 0 easy, 1 normal, 2 hard
  int64_t played = 0;           // file-clock ticks: last saved or loaded
  std::filesystem::path file;   // the save file itself (report.h can include it)
};

// Every save under `user_folder`/saves, most recently played first. Files
// that can't be read are skipped.
std::vector<Save> List(const std::filesystem::path& user_folder);

// "Easy" / "Normal" / "Hard".
const char* DifficultyName(int difficulty);

}  // namespace saves
