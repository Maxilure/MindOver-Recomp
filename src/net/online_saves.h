// =============================================================================
// net/online_saves.h -- ONLY THE HOST SAVES ONLINE (online plan s.4.5, Q4)
// =============================================================================
//
// THE RULE (decided 2026-11-17): in a lockstep session only the HOST saves the
// session. A guest can't save it; whenever the host saves, the guest's PC keeps
// a COPY of that save in its own save list, nothing more.
//
// WHY IT NEEDS CARE: both PCs run the SAME game, so both run the same save flow
// on the same frames. The original's flow opens the SAVE LIST (totem: "Do you
// want to save?" Yes -> exit 837 ExitChooseSaveSlot -> AccessMemoryCard 359 ->
// ReadingCard 384 -> GameSlotScreen 386; pause -> Save Game: 886 / 897, same
// way). That list is built from EACH PC's OWN save files: a different number
// of saves, a different cursor, "overwrite?" asked on one PC and not on the
// other = the two front ends take different exits = the worlds part. So online
// the list is never shown:
//
//   * SAVE STRAIGHT TO THE SESSION'S FILE. The storage steps run as usual
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
//     (Tried first, 2026-11-17: the original's unused "ExitAutoSaveScreen"
//     exits (totem 836, pause 885/896) straight into 326. They skip
//     BeginAccess, and the EndAccess at the end (exit 332) freed a slot table
//     that was never made: a write to 0x0000000C in sub_82360258.)
//   * A GUEST'S OWN SAVE IS REFUSED: the totem's Yes answered as No (838), the
//     pause menu's Save Game as Resume; on BOTH PCs (the same decision on the
//     same frame) the message bar says "Only the host can save online".
//     The menu's owner decides (players/menu_input: the totem = who used it,
//     the pause menu = who paused). Owner unknown = allowed (the host's file).
//
// Out of a session nothing changes (the user's rule: online changes only
// inside a session).
// =============================================================================
#pragma once
#include <cstdint>

struct PPCContext;

namespace online_saves {

// The pause menu's decision (achievements_screen.cpp's sub_82121B48 wrapper),
// after the port's own changes: returns the exit to take instead (or `exit`).
int32_t AfterPauseDecision(PPCContext& ctx, uint8_t* base, int32_t exit);

}  // namespace online_saves
