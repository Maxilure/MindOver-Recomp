// =============================================================================
// players/more_players_hud.h -- a HUD for players 3 and 4 (see the .cpp header)
// =============================================================================
// Player 3 bottom left, player 4 bottom right, each with player 1 / 2's set
// (portrait, bars, mojo count, combo multiplier, "Join Game"). Only with
// --local_players 3 or 4; --hud_bottom_y moves them up / down.
// =============================================================================
#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(double, hud_bottom_y);

namespace more_players_hud {

// Registers the data patch that adds players 3-4's HUD parts to the in-game
// menus packages. Call before data_patcher::Install().
void Register();

}  // namespace more_players_hud
