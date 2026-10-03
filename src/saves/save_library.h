// =============================================================================
// saves/save_library.h -- any number of saves, a scrolling list, names (an enhancement)
// =============================================================================
//
// WHAT (docs/05-enhancements.md, "More saves, with names"): the game's Load
// Game / Save Game screen shows three save slots. Here it shows ONE LIST:
//   * Load Game: every save, most recently played first (played = the later
//     of last saved / last loaded, saves/save_files.h).
//   * Save Game (a save totem, the pause menu): "Create New Save" first, then
//     every save the same way.
//   * The three panels are a window onto the list: down on the bottom panel
//     slides the list up by one (the next save appears at the bottom), up on
//     the top panel slides it back; the list stops at both ends. Panels with
//     nothing to show are hidden (no "Empty").
//   * New Game creates its own save file right after the difficulty screen
//     (named what was typed), without showing the list.
//   * X renames a save with the game's own name entry screen (New Game's
//     on-screen keyboard), F2 with a small typing box; Y (or Delete) asks the
//     game's own question (its overwrite question, reworded) and moves the
//     save to a "Deleted saves" folder (saves/save_files.h). The screen's
//     prompts say so: "Rename X" where the Xbox's "Storage Device Y" was,
//     "Delete Y" above "Back"; Y no longer opens the storage device chooser.
//
// HOW (findings/24 section 6): the game only thinks in slots 0-2, but a slot
// becomes a file name in ONE function, sub_82258DE8 (snprintf "%s GameSlot
// %d", slot + 1). Overriding it maps slot k to the file of list item top + k
// (any number: "CrashMOM GameSlot 7"; "Create New Save" = the first free
// number; a hidden panel = number 0, a file that never exists). The screen
// fills its panels from a 3-entry table: the game's own scan (SelectCard)
// fills it when the screen opens, a slide fills it straight from the files
// (the same 104 bytes the scan copies) and runs the screen's Enter again.
// The cursor is the Scrooby menu's own: its move functions (MoveNext /
// MovePrevious) are wrapped to slide instead of wrap; hidden panels' menu
// items lose the menu's "selectable" flag.
//
// WHICH FILE A FILE OPERATION USES (the part that keeps saves safe):
//   * the screen's scan                 -> the files of the window
//   * loading / saving a slot that was PICKED on the screen -> that pick's
//     file, which becomes the game's file (the game in progress lives there)
//   * any save of "the last slot" without a pick (SaveGame calling itself
//     after removing the old file; AutoSaveGame, a direct save the 360 front
//     end never asks for: the game has NO automatic saving) -> the game's file
//   * the end of a save session (EndAccess) forgets a pick that wasn't used
//     (not the screen's Exit: the overwrite prompt closes and reopens the
//     screen between a pick and its save)
// The manager's request state tells the operations apart (scan 8-10, load
// 11, save 12-14), the screen's "selected" flag tells a pick.
//
// Threads: the game's hooks run on its main thread; the rename box is an
// ImGui dialog on the UI thread; they talk through one mutex.
// =============================================================================
#pragma once

#include <rex/cvar.h>

// --save_library (on): the list, rename, delete, the keyboard package.
REXCVAR_DECLARE(bool, save_library);

namespace rex::ui {
class ImGuiDrawer;
}

namespace save_library {

// Creates the rename box (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer);

// F2 / Delete (UI thread): the rename box, or (`delete_save`) the game's
// delete question, for the save under the cursor, if the Load / Save Game
// screen is showing. (X, the game's name screen, and Y are read from the
// controller state by the screen's update.)
void RequestDialog(bool delete_save);

}  // namespace save_library
