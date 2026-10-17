// =============================================================================
// players/victory_scenes.h -- players 3-4 in the victory scenes, all in view
// =============================================================================
//
// THE GAME (docs/findings/26 s.39): after a titan fight the level script
// poses the players for a short in-engine "victory" shot:
//     DO_TeleportPlayer(L1_R8_VictoryLocator3, L1_R8_VictoryLocator4)
//     WAIT_PlayCinematic(CAM_l1_r8_ratcicle_victory)
// (levels/L1/objectives_era1, the Ratcicle; every victory scene is built the
// same way, each with its own two spots and camera). DO_TeleportPlayer's
// action (vtable 0x820405C0, update slot 1 sub_8229DE30) moves the focus
// player's body (game +0x28; the titan they ride, else their Crash) to the
// first spot and the other player's to the second (sub_8229E3C8): TWO
// players, and the camera shot is framed for two.
//
// THE PORT: players 3-4 in game are moved too, beside the two spots on the
// line through them (player 3 beyond the first, player 4 beyond the second),
// facing the same way; and while the cinematic that follows plays, its
// camera (a VectorCamera, rebuilt through sub_82373D18) is pulled straight
// back just enough that every player's feet and head are in the picture.
// The shot's direction and movement stay the game's. With 1-2 players
// nothing changes.
// =============================================================================
#pragma once

#include <cstdint>

namespace victory_scenes {

// The camera is about to rebuild its matrices from position / direction / up
// (r3 of sub_82373D18): pulls a victory shot back when needed.
void BeforeCameraRebuild(uint32_t camera);

}  // namespace victory_scenes
