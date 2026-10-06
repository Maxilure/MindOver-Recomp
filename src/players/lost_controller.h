// =============================================================================
// players/lost_controller.h -- a player's controller is gone: pause, say so,
// let anyone continue or drop that player out (findings/26 s.33)
// =============================================================================
//
// THE PROBLEM: when a player's controller disconnects in a level (or the
// Controls menu, F6, moves it to another player), the 360 game pauses with
// THAT player's pause menu ("P2 Paused"): its front-end check sub_82262D78
// sees the controller list's "controller missing" flags (+80, players 1-2
// only) and requests a pause owned by the player who lost it (+8536). Menus
// answer only to their owner (menu_input.cpp), so nobody could close it until
// that very controller came back. Players 3-4 had no handling at all.
//
// THE PORT'S WAY (always on in a level, --lost_controller=false = the game's):
//   * the game's own check doesn't run in a level (it still does in the
//     menus), so the owner-only pause never opens;
//   * every frame: a player IN GAME (co-op state other than "not joined")
//     with no device playing as them (input/players.h: unplugged, or moved to
//     another player) pauses the game (the game's own pause, sub_8227C8E8)
//     and the GAME'S OWN QUESTION BOX (the save
//     totem's "Do you want to save the game?", its Yes / No row hidden) says:
//         Player 2's controller disconnected.
//         Reconnect it, or press (Y) to drop out Player 2.
//     "drop out" is offered only if someone else is in game (the last player
//     in game can't drop out, like the pause menu's Drop Out);
//   * ANY player's Y drops that player out (the pause menu's own Drop Out:
//     sub_82266040 with the front end's menu owner set to them); the game
//     hears none of the presses (its pads read idle while the box is up);
//   * once a device plays as that player again the box says
//         Player 2's controller is back.
//         Player 2: press any button to resume.
//     and that player's press resumes the game.
//   * the only player in game: "Controller disconnected. Reconnect your
//     controller to continue." / "Controller reconnected. Press any button
//     to resume." (no number, no drop out);
//   * several controllers gone: one player at a time, straight from one to
//     the next (the game stays paused);
//   * a pause menu that is up when it happens comes first (the box waits);
//     if it belongs to the player without a controller, any player may work
//     it (menu_input.cpp).
// (A first version, 2026-10-06, drew its own ImGui box with "Continue"; the
// game's box looks like the rest of the game.)
//
// Testing without unplugging anything: FIFO "p<N>.unplug" / "p<N>.plug"
// (debug_input_script.h).
// =============================================================================
#pragma once

#include <cstdint>

namespace lost_controller {

// Is the box ours right now? (Any thread.)
bool Active();

// Called by the game's XInputGetState wrapper (saves/save_library.cpp,
// sub_824742F0) after each read: `user` = the socket (0-3), `gamepad` = the
// guest XINPUT_GAMEPAD (12 bytes, big-endian), `connected` = the read
// succeeded. While the box is ours it takes the presses (Y, any button) and leaves
// the game an untouched pad.
void FilterPad(int user, uint8_t* gamepad, bool connected);

}  // namespace lost_controller
