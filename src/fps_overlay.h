// =============================================================================
// fps_overlay.h -- the port's own frame-rate counter, in the game's HUD
// =============================================================================
//
// WHAT THE PLAYER SEES: one line in a corner of the screen (or top centre;
// --fps_overlay_position, Options -> Display -> FPS Position) in the game's
// own font, ALWAYS: in the menus, the pause menu and in play. It's a text of
// the button prompts' page FE_Buttons (always loaded, drawn over every
// screen; options/options_page.cpp adds one per place), in one of three
// styles (Options -> Display -> FPS Counter, or --fps_overlay):
//   1  Simple    "60 FPS"
//   2  Average   "60 FPS   AVG 59.8   1% LOW 54.2"
//   3  Detailed  ... + "16.7 MS   GAME 8.1   SWAP 7.9"
// Coloured against the frame-rate cap: green at it, yellow below 90 %, red
// below 75 %. Drawn by the game like its prompts, so it's in both renderers'
// pictures and in F10 photos. (A first version was an ImGui window, the next
// a HUD text: only in levels.)
//
// WHAT THE NUMBERS ARE (the GAME's frames, not the window's repaints: a
// frame-end listener on the renderer's SwapBuffers, pddi/intercept.h):
//   FPS      frames in the last 0.25 s
//   AVG      the last 5 s
//   1% LOW   the frame rate of the slowest 1 % of frames of the last 10 s
//            (its 99th percentile frame time)
//   MS       the frame time (last 0.25 s); GAME = the game's own work (+ our
//            native renderer's recording; without the frame pacer's wait for
//            the frame's start time), SWAP = inside SwapBuffers: waiting for
//            the screen refresh / the GPU. The rest of MS is spare time.
// The text changes 4 times a second (a new string = a small copy on the
// game's heap; every frame would be wasteful and unreadable).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(int32_t, fps_overlay);
REXCVAR_DECLARE(int32_t, fps_overlay_position);

namespace fps_overlay {

// The frame listener (CrashMomApp::OnPostSetup).
void Install();

// Every front-end update (players/more_players_frontend.cpp's hook on
// sub_82261C40: every frame, in the menus and in play): the counter's text,
// colour, place and visibility (the text itself 4 times a second).
void Update(PPCContext& ctx, uint8_t* base);

}  // namespace fps_overlay
