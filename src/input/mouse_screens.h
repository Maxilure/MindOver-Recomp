// =============================================================================
// input/mouse_screens.h -- the mouse on Crash's house's picture screens
// =============================================================================
//
// WHAT THE PLAYER GETS (2026-10-29): the screens Crash's house opens are not
// menus (no list of items the generic mouse code of mouse_menus.h could
// point at): each shows ONE picture with a left and a right arrow and walks
// through its pictures with left / right. Now, with the mouse:
//   * the TV (movies, "MovieSelectionScreen"), the comic (concept art,
//     "ConceptArtScreen") and the tiki face (enemy info, "EnemyInfoScreen"):
//     a click on an arrow steps that way; the wheel steps too (up = left,
//     down = right), and on the chest (outfits, "SkinSelectionScreen": the
//     port's own picker, players/outfit_picker.h, has its own clicks);
//   * the TV and the comic: a click on the picture = A (the TV plays that
//     movie; the comic shows the art full screen);
//   * the tiki face: a click on one of the small portraits in its top row
//     jumps to that enemy; a click on the enemy itself (the 3D model on the
//     right) = A: its sound, the screen's little secret (Space / A too).
// Right click (B) leaves them, as in every menu (mouse_menus.h).
//
// HOW: each screen is a front-end STATE (fight_tree::CurrentFrontEnd; numbers
// from notes/scratch-tools/fig_tree.py) with its page in GameStart.prj
// (tools/scrooby_dump.py on cdd70a8c.p3d): elements LeftArrow / RightArrow
// and the picture. The screens read IsButtonPressed (sub_822652B0) buttons
// 27 / 40 (left / right), 1 (A), 12 (B) every frame of their input state
// (movies: CMovieScreenAction state +76 == 3, sub_820D4CC0; enemy info:
// CEnemyInfoScreenAction +152 == 3, sub_820C91F8), so the mouse goes in as
// D-pad / A presses (KeyboardMouseDriver::Pulse) and the game does the rest:
// its sounds, its locked pictures, its wrap-around.
//
// The tiki face's portraits: the screen's 20-entry table at 0x8259FCA8
// (84 bytes each, the order of EEnemyInfo; the portrait picture at +76)
// places at most 3 of them in the top row, the selected one drawn opaque
// (alpha 255: the top byte of element +80) and the others half (128):
// sub_820C9440 (it shows the ones from s - 1 to s + 2). A click on portrait
// i with the selected one at s = |i - s| steps toward it, ONE press at a
// time: after each step the screen loads that enemy and drops presses
// meanwhile (two pulses at once moved one place).
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace mouse_screens {

// Every front-end update (main thread), from mouse_menus::FrontEndFrame
// when no question box is up and the mouse belongs to the menu's player.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

}  // namespace mouse_screens
