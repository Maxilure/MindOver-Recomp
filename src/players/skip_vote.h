// =============================================================================
// players/skip_vote.h -- with several players, a cutscene is skipped only when
// EVERY playing player has pressed to skip it (a vote)
// =============================================================================
//
// WHAT THE PLAYERS GET (2026-11-20, the user's design): in co-op (local or
// online), START brings up the game's "Skip (A)" prompt as before, and each
// player's A is one vote, in any order. The prompt then stays up and counts:
// "Skip (A) 1/2", "2/3", ... The cutscene is skipped once every playing player
// has voted. One player alone skips at once, exactly like the original.
// Setting coop_skip_vote (default on; off = the original: anyone's A skips).
//
// HOW THE GAME SKIPS (read 2026-11-20, xexdis):
//   * IN-ENGINE SCENES and camera shots: the cutscene step
//     (CMasterCinematicAction, vtable 0x82038754) in its playing state
//     (sub_82242D50): with its prompt hidden and the scene skippable (+428 bit
//     0x08), START from anyone (front end IsButtonPressed sub_822652B0, button
//     36, player -1) shows it (+428 bit 0x01, timer +420 = 2 s); while it shows,
//     button 1 (A) from anyone (the call at 0x82242E18, returning to
//     0x82242E1C) sends the step to state 4 (skip). The timer running out hides
//     the prompt.
//   * MOVIES (the cartoons): CMoviePlayerBehaviour's Update (sub_82187EF0), in
//     play, asks the input manager "did any controller press ..."
//     (sub_82274BE8(game +56, 2, event, 1)): START (event 36, back at
//     0x82188028) shows the prompt (player +296 bit 0x20, timer behaviour +52)
//     or, while it shows, hides it again; A (event 12, back at 0x82188040)
//     while it shows stops the movie.
//
// OURS: those "anyone pressed A?" questions are answered by the vote: each
// playing player is asked on their own (menu_input::PressedBy, button 1 = A),
// a press counts once per cutscene, and the answer is "yes" only when every
// playing player has voted. While a vote is open the prompt's timer is kept
// topped up (it stays up), a movie's START no longer hides it, and the
// prompt's text ("Skip" on page InGame_MovieSkip) gets the count. Online the
// votes come from the shared inputs: both PCs count the same votes on the same
// frame (only the text is each PC's own).
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace skip_vote {
// Called first thing by the IsButtonPressed wrapper (players/menu_input.cpp):
// true = this call was a cutscene's "skip?" question and has been answered
// (r3 set); false = not ours, carry on as usual.
bool AnswerSceneSkip(PPCContext& ctx, uint8_t* base);
}  // namespace skip_vote
