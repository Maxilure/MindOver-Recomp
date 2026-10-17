// =============================================================================
// input/title_any_button.h -- the title screen starts on ANY button
// =============================================================================
//
// WHAT THE PLAYER GETS: on the title screen ("Press START" on the Xbox) any
// controller button or trigger, any key and any mouse button starts the
// game, and the title says "Press Any Button" (in English; the other
// languages keep their own text).
//
// HOW (2026-10-15):
//   * Input: the title's own check is in the front end's input update
//     (sub_82264988, 0x822651DC): while front end +8594 bit 0x10 is set
//     ("the title waits for START"; read with the debug console: 0x18 at the
//     title, 0x28 in the main menu), a controller's START makes it player 1
//     (findings/25 s.3). FilterPad, run on every XInputGetState the game
//     makes (the wrapper in saves/save_library.cpp), turns a NEW press of
//     anything on that socket into a short START press while the bit is up,
//     and hides every other button then (so a B or a Y can't do something of
//     its own). After the title, buttons still held from it stay hidden until
//     let go: the Enter that left the title isn't also an A in the main menu.
//   * Text: page GameStart_Title.pag (package cdd70a8c), text "PressStart"
//     (400 x 41, centred, Titans_Small). Its English string "Press START" is
//     11 characters, too short for the new one to go over it in the text bank
//     the way the save messages are reworded (saves/save_screen_waits.cpp),
//     so FrontEndFrame sets the text element's string while the front end is
//     in state 55 (the title).
//
// THE SAME FILTER serves a movie's Skip prompt in a level (2026-10-17,
// input/movie_skip.h): while it is hidden, any press becomes START, which is
// what brings it up.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace title_any_button {

// From the game's XInputGetState wrapper, after the keyboard's merge and the
// input replay: `gamepad` = the guest X_INPUT_GAMEPAD (12 big-endian bytes)
// of socket `user`. Game threads.
// Returns the buttons it ADDED (its START; 0 = none), so the prompt pictures
// don't take them for a controller (prompt_icons::SeePad).
uint16_t FilterPad(uint32_t user, uint8_t* gamepad, uint8_t* base);

// Every front-end update (players/more_players_frontend.cpp): the title's
// "Press Any Button" text. Main thread.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

}  // namespace title_any_button
