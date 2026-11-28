// =============================================================================
// net/leave_session.h -- an online guest goes back to the main menu
// (2026-11-28, stage S1 of notes/online-coop.md s.16.9)
// =============================================================================
// WHY: when the host is gone (players/lost_controller.cpp HOST GONE: it went
// silent for 15 s, or quit), the guest's world is no use any more; the player
// presses a button and lands on the title screen, as if they had quit.
//
// HOW (the game's own way out, no level torn down by hand): the front end's
// tree (fighttrees/Frontend.bfig, notes/scratch-tools/fig_tree.py) has
//   GameRunning 489 --exit 516 ExitPause--> PauseScreen 868
//   PauseScreen 868 --exit 875 ExitGame/ExitXenon--> 53 (the title)
//   NoSavePauseScreen 901 --exit 907 ExitSaveAndExit/ExitXenon--> 53
// = what "Quit Game" -> Yes does. Request() asks for it; the GameRunning
// decision (input/background_input.cpp, sub_8211E850) answers 516 the next
// playable frame, the pause decisions (achievements/achievements_screen.cpp,
// sub_82121B48 / sub_82121EA8) answer the quit exit. Once the level is gone
// (more_players::InPlay false) FrontEndFrame closes the session (net::Leave):
// only then, because until the level is gone every system still asks "which
// player am I?" (a closed session = local play = the copies of the other
// players would be "players without a controller").
//
// THE PAUSE MENU GIVES WAY to the host box: while the host link is bad, an
// open pause menu (this PC's) closes itself (its unpause exit), so the box can
// show (it doesn't over a menu).
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace net::leave_session {

// Go back to the main menu (game's main thread or any thread). `why` = log.
void Request(const char* why);
// A request is under way (any thread).
bool Wanted();

// The GameRunning decision's answer (exit number) instead of `game_exit`
// (only when it is -1 = stay): 516 while a request is under way and play is
// on screen, else unchanged.
int32_t GameRunningExit(int32_t game_exit);
// A pause decision's answer: the quit exit while a request is under way, the
// unpause exit while the host link is bad (the host box shows instead);
// `no_save` = NoSavePauseScreen 901 (else PauseScreen 868).
int32_t PauseExit(int32_t game_exit, bool no_save);

// Every front-end update (players/more_players_frontend.cpp sub_82261C40):
// the level is gone -> the session closes.
void FrontEndFrame();

}  // namespace net::leave_session
