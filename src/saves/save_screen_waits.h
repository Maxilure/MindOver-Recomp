// =============================================================================
// saves/save_screen_waits.h -- no artificial waits on the save screens
// =============================================================================
//
// WHAT: the Xbox 360 game holds every "storage" message on screen for a
// minimum time, whatever the storage does: "Accessing ... save game files"
// 1 + 1 + 2 s before the save list (in game) / 1 + 1 + 2 s before Load Game
// (menus), "Saving content" 3 s, the loading screen after picking a save 3 s.
// Console makers asked for such minimums (a message that flashes by can't be
// read). On the PC the save drive (saves/pc_save_drive.h) finishes in a few
// milliseconds, so the player just waited. Now those screens move on as soon
// as the game's own "storage done" checks say so; --save_screen_waits=true
// brings the Xbox's minimum times back.
// "Load successful." / "Save successful." (each waiting for A) are skipped
// too: the game goes on as if A had been pressed (--save_success_screens=true
// shows them again).
// A quick read or save shows no message at all: the message and its
// dimmed background wait --save_message_delay_ms (1500) before showing.
// The messages' wording also changes (with the waits on or off): no "Please
// do not turn off the Xbox 360 console" on a PC: "Reading save files...",
// "Saving...", "Loading..." (a data patch of the menu packages' text banks,
// data/data_patcher.h; see the .cpp).
//
// HOW (findings/33): each front-end state is a node of the fight tree
// fighttrees/Frontend.bfig with a compiled DECISION (table 0x82503030 by node
// number) the dispatcher sub_8211AF40 asks every frame: it returns an exit to
// take, or -1 = stay. The waiting states' decisions all compare the state's
// TIME IN STATE, the float at *(*(r3 + 176) + 28) + 132, with a constant
// (1.0 at 0x8201EE28, 2.0 at 0x8201FB74, 3.0 at 0x8201FD24), next to their
// real checks (the save manager idle, the device chosen...). Ours (through
// data/fight_tree.h's per-node decisions) sets that time to an hour for the
// one call to the original decision and puts the real value back: every
// other check stays the game's. Each of these decisions reads the time once,
// only for this comparison (scanned: notes/scratch-tools/frontend_time_gates.py, private).
//
// The states (node, decision, minimum):
//   menus, Load Game / Save Game: 113 AccessMemoryCard (1 s), 125
//     DeviceSelectorOpen (1 s), 153 ReadingCard (2 s), 278 LoadGameScreen
//     (3 s, the loading screen after a pick), 289 SaveGameScreen (3 s)
//   in game, Save Game:  359 AccessMemoryCard (1 s), 366 ShowDeviceSelector
//     (1 s), 384 ReadingCard (2 s), 326 AutoSaveGameScreen (3 s, "Saving")
//   boot: 4 CheckMemoryCard (1 s), 8 ReadingCard (2 s)
// Not touched: the boot's warning / licence / title timers (not storage),
// FadeOut (the fade into the level after New Game's save), the extras'
// loading screens.
#pragma once

namespace save_screen_waits {

// Registers the decisions and the wording patch (once, before
// data_patcher::Install()).
void Install();

}  // namespace save_screen_waits
