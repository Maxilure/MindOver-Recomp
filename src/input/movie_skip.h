// =============================================================================
// input/movie_skip.h -- any button brings up a movie's "Skip" prompt
// =============================================================================
//
// WHAT THE PLAYER GETS: while a movie plays in a level (the cartoon cutscenes,
// e.g. the one after talking to Coco on Wumpa Island), ANY controller button
// or trigger, any key and any mouse button shows the game's "Skip (A)" prompt
// in the corner; A then skips (Enter / Space on the keyboard: the movie counts
// as a menu, so the keyboard's menu keys answer). The press that brings the
// prompt up does nothing else, so no movie is skipped by accident.
//
// THE BUG IT FIXES: on the Xbox only START brings the prompt up. The keyboard
// had no way to: during a movie the front end says "a menu is on screen"
// (+8594 bit 0x80), so the keyboard is in its MENU context, where Escape is
// Back (B) and Enter is A; neither is START, and A alone does nothing while
// the prompt is hidden. Escape skipped the boot movies only because outside
// play the game takes A or START directly (see below).
//
// HOW THE GAME DOES IT (2026-10-17, read from the code):
//   * The movie is played by CMoviePlayerBehaviour (vtable 0x820334E4); its
//     Update (slot 4, sub_82187EF0, f1 = frame time) asks the game's input
//     events (sub_82274BE8(game +56, 2, event, 1) = "just pressed": event 36
//     = START, event 12 = A). Its movie player (CMoviePlayer, vtable
//     0x82041C4C) is at behaviour +36.
//   * In play (game state 5, game +8): with the prompt HIDDEN, START shows it
//     (sub_822C49D8(player, 1) sets player +296 bit 0x20) and starts a 2 s
//     timer (behaviour +52 = 2.0 from 0x8201FB74); with the prompt SHOWN,
//     START hides it again, A stops the movie (player vtable slot 6) with the
//     front end's sound, and the timer running out hides it.
//   * Outside play (boot movies): A or START stop the movie at once.
//   * The player's own update (sub_822C42A0) puts the page InGame_MovieSkip
//     (text "Skip" + the A picture, InGame.prj) on screen while +296 bit
//     0x20 is set (sub_822C49F0 reads it).
//
// OURS: the Update is wrapped only to WATCH it (it runs unchanged): after each
// call it notes whether a movie is playing in play with the prompt hidden.
// While it is, the filter in input/title_any_button.cpp (run on every
// XInputGetState the game makes) turns a NEW press of anything into a short
// START press and hides the other buttons; buttons still held when the prompt
// comes up stay hidden until let go (the A that brought it up isn't also the
// A that skips). With the prompt shown everything passes through untouched:
// the game's own A skips, START hides it again.
// =============================================================================
#pragma once

namespace movie_skip {

// True while a movie plays in a level with its Skip prompt hidden (seen by the
// movie's Update within the last quarter second). Any thread.
bool WaitsForAnyButton();

}  // namespace movie_skip
