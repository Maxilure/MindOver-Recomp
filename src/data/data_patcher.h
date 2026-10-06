// =============================================================================
// data/data_patcher.h -- changed copies of the game's data files, built from the player's disc
// =============================================================================
//
// WHY: some of the port's features need the game's DATA changed, not only
// its code: a page added to the in-game menus, new states in the front
// end's screen tree (saves/rename_screen.h). The extracted disc files must
// never be written to, and the port can't ship any game content. So the
// changes are made at run time, from the player's own files, the way a mod
// would ship them: a "patch" is code that turns the original file into the
// changed one.
//
// HOW: the game builds the path of every package and fight tree it loads in
// ONE function, sub_822E3828(category, name, buffer[512]) -> e.g.
// "package/7a8185b0.p3d" (category 6, the menus' packages) or
// "fighttrees/Frontend.bfig" (category 12; the numbers are one higher than
// levels/GlobalPackages.p3d's, traced 2026-10-03). It's wrapped: when a patch
// is registered for that category and name, the original file is read out
// of default.rcf (where the game would find it), the patches change it, the
// result goes to <game folder>/cache/patched_data/<file name>, and the path
// becomes "crashmom/<file name>". Built once per run (they follow the
// player's disc data), at the first request.
//
// SERVING THE FILE (findings/24 section 7.6): the game's own file layer
// only knows its drives (GAME:, DVD...): a path on a drive of ours
// ("crashmom:\...", tried first) made it read through a null pointer
// (+0x98). A RELATIVE path it can't find in its archives is opened as a
// loose file on D: instead (as "D:\levels\L0\objectives_era1.blua" is, a
// file of another release), here D:\crashmom\<file name>. The cache folder
// is mounted exactly there, as a host-folder device at the game drive's
// \Device\Harddisk0\Partition1 + \crashmom: SDK patch 0012 makes the file
// system pick the device with the longest matching mount path (it took the
// first registered: the disc files' folder).
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace data_patcher {

using Bytes = std::vector<uint8_t>;

// The path builder's categories (r3 of sub_822E3828).
constexpr uint32_t kCategoryFrontend = 6;    // package/<8 hex digits>.p3d: menus, in-game menus
constexpr uint32_t kCategoryFightTree = 12;  // fighttrees/<name>.bfig: the front end, characters

// A patch: the original file -> the changed one. Returns false to leave
// the file alone (not the file it's for, or it isn't what the patch
// expects: then the game gets its own file).
using Patch = std::function<bool(const Bytes& original, Bytes* patched)>;

// Registers a patch for the files of `category` named `name` (as the path
// builder gets it: "Frontend", "7a8185b0"; empty = every file of the
// category). Several patches on one file run in registration order. Call
// before Install().
void Register(std::string label, uint32_t category, std::string name, Patch patch);

// Mounts the cache folder (once at startup, after the file system is set
// up). Without it, the game always gets its own files.
void Install();

// Whether the game was given a patched copy of that file (it asked for the
// path and a patch changed it).
bool Served(uint32_t category, std::string_view name);

// The game's own path ("fighttrees/Frontend.bfig") of a path we gave it
// ("crashmom/Frontend.bfig"); empty if `path` isn't one of ours. (The game
// keys some data by a file's path: fighttrees/branchcount.txt.)
std::string OriginalPath(std::string_view path);

// One file out of the game's default.rcf (name as the archive lists it,
// e.g. "package\\cdd70a8c.p3d").
bool ReadArchiveFile(std::string_view name, Bytes* out, std::string* error);

}  // namespace data_patcher
