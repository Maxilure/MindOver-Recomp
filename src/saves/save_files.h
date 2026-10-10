// =============================================================================
// saves/save_files.h -- the save files on disk (find, rename, set aside, copy over)
// =============================================================================
//
// WHAT: every host-side operation on the save files: which saves exist, the
// name inside one, renaming one, moving one out of the game's sight
// ("delete"), the backups taken before the game deletes or overwrites one, and
// the one-time copy from the old Xbox-style layout. The game reads and writes
// the same files through our PC save drive (saves/pc_save_drive.h).
//
// WHERE THE FILES ARE (since 2026-10-10, flat):
//
//   user/saves/CrashMOM GameSlot N.sav     save number N (1, 2, 3, ...)
//
// (or --user_data_root, which test runs point at a COPY). The file name is
// the game's own name for the save ("%s GameSlot %d", findings/24 s.2.2) plus
// ".sav": no translation between what the game asks for and what's on disk.
// The game itself only ever names N = 1, 2, 3 (one file per slot); the save
// library maps more slots onto N = 4, 5, ... (findings/24).
//
// THE SAVE FILE (findings/24 section 1, findings/32), 20,205 bytes, byte for
// byte what the Xbox 360 wrote into its content package:
//   +0      u32 BE   total file size (0x4EED): the save DRIVE's own header
//                    (Radical's XenonSaveDrive writes it, checks it on open)
//   +4      the game's 20,174-byte save block; its first 104 bytes are what
//           the Load Game screen shows:
//             block +8   u32 format word 0x4ECE
//             block +12  the save time (u16 year, u8 month, day, hour, min, s)
//             block +20  the NAME typed at New Game, 32 UTF-16 (BE) characters
//           ...
//   +20178  27 bytes, the same in every file (no checksum anywhere).
// So a rename only rewrites the 64 name bytes at file offset 24.
//
// THE OLD LAYOUT (the emulated Xbox's content folders, until 2026-10-10):
//   user/saves/<profile id>/565507FA/00000001/CrashMOM GameSlot N/CrashMOM GameSlot N
//   user/saves/<profile id>/565507FA/Headers/00000001/CrashMOM GameSlot N.header
// CopyFromXboxLayout() copies those files ONCE into the flat layout (same
// bytes, same dates) and never touches the old folders: they stay as they
// were, a full backup, and an older build of the port still finds them.
//
// LAST PLAYED: the save library lists saves most recently played first. A
// save's file time is when it was last saved (a rename keeps the time); when
// it was last LOADED is kept in user/saves/save_library_played.txt ("N
// <time>" lines). Played = the later of the two.
//
// "DELETE" (the save list's Y) moves the save into
//   user/saves/Deleted saves/CrashMOM GameSlot N (<date time>).sav
// THE GAME'S OWN DELETE (it deletes a save before writing over it) moves the
// old file into
//   user/saves/Backups/CrashMOM GameSlot N (<date time>).sav
// keeping the newest kBackupsPerSave per save: an overwrite can be undone by
// hand, and nothing is ever really deleted by the game.
//
// Threads: the save list calls in from the game's main thread, the PC save
// drive from Radical's drive thread: both hold Lock() around file work.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace save_files {

// The save of file number N (1, 2, 3, ...): "CrashMOM GameSlot N" (the
// game's name for it, without ".sav").
std::string ContentName(int number);

// user/saves (or --user_data_root), absolute; empty before the game folder
// is set up.
std::filesystem::path SavesFolder();

// The file behind a name the game asks for ("CrashMOM GameSlot 4" ->
// user/saves/CrashMOM GameSlot 4.sav), and behind save number N.
std::filesystem::path FileOf(std::string_view game_name);
std::filesystem::path FileOf(int number);

// Held by everything that reads or changes save files (two threads use them).
std::recursive_mutex& Lock();

// Whether save N exists.
bool Exists(int number);

// The highest N with a save, 0 if there is none.
int HighestNumber();

// Every save ("CrashMOM GameSlot N.sav", N >= 1), most recently played first.
struct SaveInfo {
  int number = 0;
  int64_t played = 0;  // file-clock ticks: the later of last saved / last loaded
};
std::vector<SaveInfo> ListSaves();

// Every save file name the game could ask for, without ".sav" (the PC save
// drive's FindFirst/FindNext list these).
std::vector<std::string> GameNames();

// The smallest N >= 1 without a save: where "Create New Save" writes.
int FreeNumber();

// Save N was just loaded (its "played" time is now) / is gone.
void MarkPlayed(int number);
void ForgetPlayed(int number);

// The first `size` bytes of save N's game block (file offset 4): what the
// game's own scan copies into its slot table. False if there is no such save
// or it is shorter.
bool ReadBlockStart(int number, uint8_t* out, size_t size);

// Writes `name` (at most kMaxNameLength characters) into save N's file.
// False + `error` if the file isn't a save of the expected size, or writing
// fails (the old file then stays as it was: the new one is written next to
// it and renamed over it).
inline constexpr size_t kMaxNameLength = 16;
bool Rename(int number, std::u16string_view name, std::string* error);

// Moves save N into "Deleted saves". `moved_to` gets the new file. False +
// `error` if it isn't there or the move fails.
bool MoveToDeleted(int number, std::filesystem::path* moved_to, std::string* error);

// The game deletes (or writes over) the save file `file`: move it into
// "Backups" instead and drop that save's oldest backups beyond
// kBackupsPerSave. False + `error` if the move fails (the file then stays).
inline constexpr int kBackupsPerSave = 10;
bool MoveToBackups(const std::filesystem::path& file, std::filesystem::path* moved_to,
                   std::string* error);

// Writes `data` next to `path` and renames it over it: a crash mid-write
// leaves the old file whole. False + `error` on failure.
bool ReplaceFile(const std::filesystem::path& path, const std::vector<uint8_t>& data,
                 std::string* error);

// The one-time copy from the old Xbox-style layout (see above), at startup
// before the game runs. Does nothing once user/saves/copied-from-xbox-layout.txt
// exists (it lists what was copied); otherwise copies every old save whose
// flat file doesn't exist yet. Returns the lines to log.
std::vector<std::string> CopyFromXboxLayout();

}  // namespace save_files
