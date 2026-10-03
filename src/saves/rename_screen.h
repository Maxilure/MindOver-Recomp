// =============================================================================
// saves/rename_screen.h -- a real "rename a save" screen in the game's front end
// =============================================================================
//
// WHAT: X on a save in the Load / Save Game list opens the game's own name
// entry screen (New Game's on-screen keyboard) to rename it
// (saves/save_library.h). This module gives that a route of its own in the
// game's screen tree, in the menus AND in game, and puts the keyboard's page
// into the in-game menus so it works in every level. In game it shows over
// the level, like the in-game save list: no purple frame, no blue swirl.
//
// THE ROUTE (data/fight_tree.h; findings/24 section 7): the front end's
// screens are states of the fight tree fighttrees/Frontend.bfig. A data
// patch (data/data_patcher.h) appends 10 nodes after its last one (994):
//   menu:    995 ExitRenameMenu -> 996 (taken by the list, state 160)
//            996 RenameScreen: the name entry state's actions (the keyboard
//                screen, the menus' frame and background, the prompts)
//            997 ExitDone, 998 ExitBack -> 160
//            999 ExitSave -> SaveGameScreen 289 (a copy of slot 1's
//                ExitAvailableSlot 185, its save script too)
//   in game: 1000 ExitRenameInGame -> 1001 (taken by the list, state 386)
//            1001 RenameScreen: the same WITHOUT CNVBackgroundAction (the
//                 menus' frame + swirl, which only menu states have)
//            1002 ExitDone, 1003 ExitBack -> 386
//            1004 ExitSave -> AutoSaveGameScreen 326 (copy of 408)
// ExitSave names a NEW save: "Create New Save" goes through the rename
// screen first (saves/save_library.h), Done saves under the typed name.
// They hang under the tree's last top-level state, EndOfTree (992): the
// file is the tree in pre-order, so only the last subtree can grow without
// moving any existing node's number (data/fight_tree.h). The engine follows
// the target of whatever exit a decision returns, child of the current
// state or not.
// The records are copies of the ones the first version jumped through (the
// New Game name state 93; DifficultyScreen's ExitBack 112, whose script
// runs on the way in; ReadingCard's script-less exits 159 / 385 on the way
// back; slot 1's ExitAvailableSlot for ExitSave), with new names, parents
// and targets. The new states have no
// compiled decision (the table is "none" past 994): ours (Done = front end
// flag bit 3 -> ExitDone, Cancel = bit 2 -> ExitBack, like the name state's
// sub_8211B7D8) run from the front end's dispatcher (data/fight_tree.cpp).
//
// THE PAGE: the keyboard screen's action (CNameEntryScreenAction, Enter
// sub_820D5488) finds the screen and page "GameStart_NameEntry" in the front
// end's page lookup (layer 4). In game that lookup holds the in-game menus'
// project InGame.prj (package per language, e.g. 7a8185b0 = English), so a
// second data patch adds the page + screen to it, with the pictures and the
// "frontend" font the page names, cut out of the menu package
// (package\cdd70a8c.p3d) of the player's disc. The frame (FE_NV_Frame) is
// NOT added: only CNVBackgroundAction uses it (sub_820D7960; the name
// screen's Enter never touches it).
// =============================================================================
#pragma once

#include <cstdint>

namespace rename_screen {

// Registers the two data patches (before data_patcher::Install()).
void Register();

// Whether the rename route exists (the tree was patched), and in game
// whether the keyboard page was added to the in-game menus.
bool Available(bool in_game);

// The exit into the rename screen, from the menu's / in-game save list.
int32_t EntryExit(bool in_game);

}  // namespace rename_screen
