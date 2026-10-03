// =============================================================================
// saves/save_files.h -- the game's save files on disk (find, rename, set aside)
// =============================================================================
//
// WHAT: the host-side half of the save library (saves/save_library.h): which
// save files exist, the name inside one, renaming one, moving one out of the
// game's sight ("delete"). No game code runs here; the game reads and writes
// the same files through the runtime's Xbox content services.
//
// WHERE THE FILES ARE (the runtime's content layout, ReXGlue
// ContentManager): for the signed-in profile's id (16 hex digits) and the
// game's title id 565507FA, content type 00000001 = saved game:
//
//   <user data>/<xuid>/565507FA/00000001/CrashMOM GameSlot N/CrashMOM GameSlot N   the save
//   <user data>/<xuid>/565507FA/Headers/00000001/CrashMOM GameSlot N.header        its header
//
// The game itself only ever names N = 1, 2, 3 (one file per slot); the save
// library maps more slots onto N = 4, 5, ... (findings/24).
//
// THE SAVE FILE (findings/24 section 1), 20,205 bytes:
//   +0      u32 BE   total size (0x4EED)
//   +4      the game's 20,174-byte save block; its first 104 bytes are what
//           the Load Game screen shows:
//             block +8   u32 format word 0x4ECE
//             block +12  the save time (u16 year, u8 month, day, hour, min, s)
//             block +20  the NAME typed at New Game, 32 UTF-16 (BE) characters
//           ...
//   +20178  27 bytes, the same in every file (no checksum anywhere).
// So a rename only rewrites the 64 name bytes at file offset 24.
//
// THE HEADER (.header, 328 bytes) is the runtime's XCONTENT_AGGREGATE_DATA:
// device id, content type, a 128-character UTF-16 (BE) display name, the
// file name, the profile id, the title id. The game writes the display name
// as "Name: <name> Slot: <n>  Last Save: <m>.<d>.<yy>  Time: <hh>:<mm>"; the
// game itself never reads it back (only the Xbox dashboard would show it).
//
// LAST PLAYED: the save library lists saves most recently played first. A
// save's file time is when it was last saved (the game writes the file then;
// a rename keeps the time); when it was last LOADED is kept in a small text
// file of ours, <user data>/<xuid>/565507FA/save_library_played.txt ("N
// <time>" lines). Played = the later of the two.
//
// "DELETE" moves the save and its header into
//   <user data>/<xuid>/565507FA/Deleted saves/CrashMOM GameSlot N (<date time>)/
// where the game doesn't look: nothing is lost by a wrong press, and the
// files can be moved back by hand.
//
// Threads: called from the game's main thread (save_library.cpp), while
// the game's save manager is idle (no request of its own in flight).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace save_files {

// The save of file number N (1, 2, 3, ...): "CrashMOM GameSlot N".
std::string ContentName(int number);

// <user data>/<xuid>/565507FA/00000001 for the signed-in profile (empty
// path if the runtime isn't up yet).
std::filesystem::path SavesFolder();

// Whether save N exists (its folder is there).
bool Exists(int number);

// The highest N with a save folder, 0 if there is none.
int HighestNumber();

// Every save (a "CrashMOM GameSlot N" folder holding its file, N >= 1), most
// recently played first.
struct SaveInfo {
  int number = 0;
  int64_t played = 0;  // file-clock ticks: the later of last saved / last loaded
};
std::vector<SaveInfo> ListSaves();

// The smallest N >= 1 without a save: where "Create New Save" writes.
int FreeNumber();

// Save N was just loaded (its "played" time is now) / is gone.
void MarkPlayed(int number);
void ForgetPlayed(int number);

// The first `size` bytes of save N's game block (file offset 4): what the
// game's own scan copies into its slot table. False if there is no such save
// or it is shorter.
bool ReadBlockStart(int number, uint8_t* out, size_t size);

// Writes `name` (at most kMaxNameLength characters) into save N's file and
// its header's display name. False + `error` if the file isn't a save of
// the expected size, or writing fails (the old file then stays as it was:
// the new one is written next to it and renamed over it).
inline constexpr size_t kMaxNameLength = 16;
bool Rename(int number, std::u16string_view name, std::string* error);

// Moves save N (folder + header) into "Deleted saves". `moved_to` gets the
// new folder. False + `error` if it isn't there or the move fails.
bool MoveToDeleted(int number, std::filesystem::path* moved_to, std::string* error);

}  // namespace save_files
