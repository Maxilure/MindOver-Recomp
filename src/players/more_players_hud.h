// =============================================================================
// players/more_players_hud.h -- a HUD for players 3 and 4 (see the .cpp header)
// =============================================================================
// Player 3 bottom left, player 4 bottom right, each with player 1 / 2's set
// (portrait, bars, mojo count, combo multiplier, "Join Game"). Only with
// --local_players 3 or 4. Laid out as players 1-2's mirrored top to bottom.
// =============================================================================
#pragma once

namespace more_players_hud {

// Registers the data patch that adds players 3-4's HUD parts to the in-game
// menus packages. Call before data_patcher::Install().
void Register();

// THE COMBO METERS' HEIGHT (the green star with the hit count; findings/26
// s.25): with 3-4 players, players 1-2's meter moves up under their mojo
// count, players 3-4's mirrored down over theirs. y counts up from the bottom
// of the 480-unit screen; player = 0-3.
float ComboMeterY(int player);

}  // namespace more_players_hud
