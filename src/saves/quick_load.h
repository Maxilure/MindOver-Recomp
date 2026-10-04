// =============================================================================
// saves/quick_load.h -- --load_save: start the game straight into a save
// =============================================================================
//
// WHAT: `--load_save=<save>` boots past the intro movies, the title screen,
// the main menu and the Load Game list, and loads the save given:
//   --load_save=7            save file number 7 ("CrashMOM GameSlot 7")
//   --load_save="ice tower"  the save with that name (as shown in the list;
//                            letters' case ignored), or else the ONE save
//                            whose name contains it
//   --load_save=last         the most recently played save (the top of the
//                            Load Game list)
// For testing: every run starts at the same spot in a few seconds, with no
// inputs to script. A save that can't be found is reported in the log
// (with the list of saves) and the game boots normally.
//
// HOW (findings/25): the front end's screens are states of the fight tree
// fighttrees/Frontend.bfig, and each state's compiled "decision" returns
// the exit to take (data/fight_tree.h). The engine follows ANY exit's
// target, child of the current state or not (findings/24 section 7). So the
// route only needs three answers changed, and every script the game runs
// on the way is its own:
//   44 LoadPersistentPostBootUp  (decision sub_8211B4E0) returns 50 = the
//        intro movie (or 49, Radical's "skip to level" switch). We take
//        87 instead: the main menu's ExitLoadGame, whose scripts open a
//        LOAD session (SaveGameManager BeginAccess, ESaveMode_LOAD). It
//        leads to 113 AccessMemoryCard (device / profile checks) and
//        153 ReadingCard (the scan of the saves).
//   153 ReadingCard              (sub_8211BE58, wrapped in save_library.cpp)
//        returns 159 = open the list. We pick the save (the save library
//        maps slot 0 to its file) and take 187 instead: slot 1's
//        ExitLoad / ExitSlotNotEmpty, whose script calls LoadGame(0),
//        into 278 LoadGameScreen.
//   282 LoadComplete             (sub_8211CE30) waits for a button under
//        "Load successful". We take 283 ExitToLevel at once: its script
//        is StartLevelFromLastCheckPoint, into 486 InGame.
// Skipped: 51 Movie, 53/55 the title screen, 66/68 the main menu, 160 the
// list. A failed load goes the game's own way (287 LoadFailed).
//
// Needs the save library (--save_library, on by default): it is what turns
// "slot 0" into any save file.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, load_save);

namespace quick_load {

// ReadingCard's decision returned `exit` (game thread; save_library.cpp's
// wrapper asks). Returns the exit to take instead, or -2 to keep it.
int32_t OnReadingCard(int32_t exit);

}  // namespace quick_load
