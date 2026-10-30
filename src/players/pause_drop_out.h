// =============================================================================
// players/pause_drop_out.h -- BACK (SELECT) DROPS A PLAYER OUT, in the pause menu
// =============================================================================
// The original's two-player pause page lists "Drop Out" among its items
// (Resume, Missions, Tutorials, Drop Out, Options, Save, Quit), and only the
// menu's OWNER (who paused) can pick it. With our extra items (Players,
// Achievements) that list grew long, so (2026-10-30) the item is gone
// (options_page.cpp RemoveDropOut) and Back / Select does it instead, for
// EVERY player in game, while the pause menu is up:
//   * the menu's owner presses Back: the screen picks like A does (its own
//     sounds, its closing animation), then the pause decision answers the
//     game's own Drop Out exit (882; 914 on the pause screen without Save) =
//     what picking the old item did (the owner drops out, play goes on).
//     (First version answered the exit at once: the menu vanished without
//     its closing; playtest 2026-10-30);
//   * another player presses Back: that player drops out at once
//     (lost_controller::DropOut = the same game call with the owner set to
//     them, sub_82266040); the menu stays up for its owner, the player
//     leaves when play goes on;
//   * the last player in game can't drop out (as before: the two-player page
//     only shows with 2+ players in game).
// The keyboard's Back is the Map key (Tab / M), which now also answers in
// menus (input/bindings.cpp). A "Drop Out" prompt with the Back picture
// stands in the pause page's free upper-left corner (FE_Buttons) while
// someone may drop out.
//
// How it hooks in: both pause decisions are wrapped in
// achievements/achievements_screen.cpp (they run every frame while the pause
// state is current); after the game's own answer they call
// AfterPauseDecision, which reads every player's controller as the game last
// read it (input_record::LastRead) for a NEW Back press.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace pause_drop_out {

// The pause decisions' exits for "Drop Out" (two-player branch, index 3):
// PauseScreen (sub_82121B48) and NoSavePauseScreen (sub_82121EA8).
constexpr int32_t kDropOutExit = 882;
constexpr int32_t kNoSaveDropOutExit = 914;

// Called after a pause decision ran (game main thread): `exit` = the game's
// answer (-1 = stay), `drop_out_exit` = that decision's Drop Out exit.
// Returns the exit to use: `exit`, or `drop_out_exit` when the menu's owner
// pressed Back and may drop out.
// Owner's Back: that frame nothing is decided yet; the screen's "open" step
// (sub_820D9E48, wrapped) picks like A (PickForOwner), the screen closes as
// usual, and once the game sets its "picked" flag this returns the Drop Out
// exit instead of the item under the cursor.
int32_t AfterPauseDecision(PPCContext& ctx, uint8_t* base, int32_t exit, int32_t drop_out_exit);

// For the sub_820D9E48 wrapper: the owner pressed Back and the screen hasn't
// picked for it yet. PickForOwner = the screen's own A step for `action`
// (the menu screen action: state 5, select / back / arm_wipe sounds, input off).
bool OwnerPickWaiting();
void PickForOwner(PPCContext& ctx, uint8_t* base, uint32_t action);

}  // namespace pause_drop_out
