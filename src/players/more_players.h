// =============================================================================
// players/more_players.h -- room for up to four local players (findings/26)
// =============================================================================
//
// WHAT: the game's drop-in co-op is for two. It keeps what each player is
// doing in small global tables with TWO entries, indexed by the player's
// number (0 = player 1, 1 = player 2). This module gives every one of those
// tables FOUR entries, so players 3 and 4 have their own, and creates players
// 3 and 4 the way the game creates player 2.
//
// THE TABLES and how each one gets four entries (addresses = guest):
//
//   0x8259B11C  u32 x2  co-op state (ECoOpPlayerState: 0 not joined, 1 mask,
//                        2 in game, 3 entering the mask, 4 invalid). 37 code
//                        sites, so it GROWS IN PLACE to 0x8259B12B; what sat
//                        there moves to our block (below):
//   0x8259B124  u8 x2   a per-player flag (digging, the turret, co-op)
//   0x8259B126  u8      a flag (getter/setter sub_822635E8/F8)
//   0x8259B127  u8      a flag (sub_82270608 sets it, sub_82270678 reads it)
//   0x8259B128  u32     a pointer (the co-op behaviours' script helpers)
//               -> 20 references redirected: tiny functions rewritten here,
//                  the rest by midasm hooks that point a register at our block.
//   0x824F3C30  u32 x2  starting state ([2, 0]): MOVES to our block (5 sites).
//   0x824F3C38  u32 x2  starting sub-state ([9, 9], 24 sites): GROWS IN PLACE
//                        to 0x824F3C47; 0x824F3C40 (no references) and the
//                        float 0x824F3C44 (sub_821AD970 reads + writes it,
//                        sub_821AE1A0 reads it) give way: the float moves.
//   front end +8524  s32 x2  each player's controller (+(2131 + p) * 4):
//                        only a getter/setter touch it -> players 3-4 here.
//   0x8259B1C0  u32 x2  each player's character (getter/setter): 3-4 here,
//                        Carbon Crash (0; player 2 becomes Coco, 9, later).
//   0x825A6280, 0x825A6A88  2 x 1028 bytes each: per-player lists read when
//                        spawning (count, then items; -1 = none): players
//                        3-4 get empty lists (2 midasm hooks in the spawn).
//   game object (0x825B0008) +16/+20 characters, +24/+28 JACKED TITANS, +108/+172
//                        carried-actor names (64 bytes): players 3-4's are in our
//                        block (midasm hooks on every "player < 2" read/write site,
//                        the tiny helpers rewritten; findings/26 s.10 and s.15).
//   sub_82234B58 = the "reset co-op" function that fills all of the above:
//                        rewritten for four players.
//
// SPAWNING: every level creates one CActionSpawnPlayer event per player
// (sub_82283670: allocate 92 bytes, construct with sub_8229C550, add to the
// level's list). After player 2's, players 3 and 4 get theirs: constructed AS
// player 2 (same character, spawn point, name) and then renumbered, with a
// name hash of their own (else the level has two actors of one name). Only
// with --local_players 3 or 4.
//
// The table moves are ALWAYS ON (from launch: a table can't change address
// halfway through a run); they change nothing for players 1 and 2. Only the
// extra spawns depend on --local_players.
//
// Masks (findings/26 s.12, s.15): up to three per Crash, the nearest Crash on
// foot as host, riders follow a host that turns into a mask, the host kept by
// player number across levels, "may I turn into a mask" for every player.
// Not done yet: the camera, the HUD, the aiming reticle table (0x8259AFAC: two
// per-player entries + one shared), titans carried into the next level for
// players 3-4.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, local_players);

namespace more_players {

// Moves the tables (copies their current values to the new places and
// clears the grown entries). Call once before the game's code runs
// (CrashMomApp::OnPreLaunchModule).
void Install();

// How many local players levels make room for (--local_players, 2-4).
int LocalPlayerCount();

// Is a level being played (game state 5, its pause and menus included)?
// Safe from any thread.
bool InPlay();

// Player p's character (Crash), 0 if none (the game object's list / ours).
uint32_t CharacterOfPlayer(int p);

// The titan player p has jacked (rides), 0 if on foot / none.
uint32_t TitanOfPlayer(int p);

}  // namespace more_players

struct PPCContext;
namespace menu_input {
// The player whose menu is on screen (menu_input.cpp's rules: who paused, who
// interacted with the totem, ...), -1 = anyone (also outside play). For menu
// buttons we read ourselves (the save list's X / Y, the name screen's X).
int CurrentMenuOwner(PPCContext& ctx, uint8_t* base);
// Does player p (0-3) play right now? (Player 1 always does.)
bool PlayerPlays(int p);
}  // namespace menu_input

namespace more_players_frontend {
// Sets up players 3-4's counter prompts (call once the in-game HUD is set up).
void SetUpInGame(PPCContext& ctx, uint8_t* base);
// One hit on player p's (0-3) combo meter, like a landed hit (debug FIFO).
void AddComboHit(PPCContext& ctx, uint8_t* base, int player);
}  // namespace more_players_frontend
