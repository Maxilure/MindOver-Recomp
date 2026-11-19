// =============================================================================
// net/online_saves.h -- EVERY PLAYER SAVES ON THEIR OWN PC (online plan s.4.5, Q4)
// =============================================================================
//
// THE RULE (2026-11-17, widened 2026-11-19): in a lockstep session a save is
// written on the PC of the player who made it:
//   * the HOST's save (totem or pause menu) overwrites the save the session
//     was loaded from, on the host's PC only;
//   * a GUEST's save goes into that guest's own copy, on the guest's PC
//     only (the host's save stays as it was).
// A guest keeps a copy of the session only by saving themself. (Until
// 2026-11-19 a guest's save was refused with a card and every host save left
// a copy on the guest's PC; the design decision then: "a guest may take a
// copy for themself, when they save".)
//
// WHY IT NEEDS CARE: both PCs run the SAME game, so both run the same save
// flow on the same frames, whoever asked. The original's flow opens the SAVE
// LIST (totem: "Do you want to save?" Yes -> exit 837 ExitChooseSaveSlot ->
// AccessMemoryCard 359 -> ReadingCard 384 -> GameSlotScreen 386; pause ->
// Save Game: 886 / 897, same way). That list is built from EACH PC's OWN save
// files: a different number of saves, a different cursor, "overwrite?" asked
// on one PC and not on the other = the two front ends take different exits
// = the worlds part. So online the list is never shown:
//
//   * SAVE STRAIGHT TO A FILE. The storage steps run as usual
//     (AccessMemoryCard 359: the save manager's BeginAccess; ReadingCard 384:
//     the scan). From ReadingCard the save library (saves/save_library.cpp,
//     sub_8211DA80) takes slot 1's "available slot" exit (408 ->
//     AutoSaveGameScreen 326, its script: SaveGame(0)) instead of opening the
//     list: the way New Game makes its save from the main menu. Slot 0 is
//     "Create New Save" on every PC (empty: no overwrite step, no question).
//     The save library then picks the FILE per PC:
//       - the host: the save the session was loaded from (overwritten);
//       - a guest: its OWN COPY: the first save of a game copies the loaded
//         file to a new number, later saves overwrite that copy. The guest's
//         own save is never written.
//     Both are existing files, so both PCs' save managers take the same
//     steps. (Tried first, 2026-11-17: the original's unused
//     "ExitAutoSaveScreen" exits (totem 836, pause 885/896) straight into
//     326. They skip BeginAccess, and the EndAccess at the end (exit 332)
//     freed a slot table that was never made: a write to 0x0000000C in
//     sub_82360258.)
//   * WHO MADE IT: the menu's owner (players/menu_input: the totem = who used
//     it, the pause menu = who paused). On a PC where that is neither this
//     PC's player nor the host (a guest's save seen from the host's PC), the
//     save drive (saves/pc_save_drive) runs every step and answers like a
//     write that worked, but leaves the file alone: the game's memory takes
//     the same steps on both PCs, only the disk differs. Owner unknown =
//     treated as the host's save.
//
// Out of a session nothing changes (online changes only inside a session).
// =============================================================================
#pragma once
#include <cstdint>

struct PPCContext;

namespace online_saves {

// A save starts (the save library's SaveGame wrapper, before the drive is
// used): does THIS PC write it? True outside a session. Logs the decision.
bool ThisPcWritesSave(PPCContext& ctx, uint8_t* base);

}  // namespace online_saves
