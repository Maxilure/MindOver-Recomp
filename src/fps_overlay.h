// =============================================================================
// fps_overlay.h -- the port's own frame-rate counter, over the picture
// =============================================================================
//
// WHAT THE PLAYER SEES: one line in a corner of the picture (or top centre;
// --fps_overlay_position, Options -> Display -> FPS Position) in the game's
// own font (Titans_Small, read from the disc data: ui/game_font.h), ALWAYS:
// boot movies, loading screens, menus AND their transitions, pause, play.
// Styles (Options -> Display -> FPS Counter, or --fps_overlay):
//   1  Simple    "60 FPS"
//   2  Average   "60 FPS   AVG 59.8   1% LOW 54.2"
//   3  Detailed  ... + "16.7 MS   GAME 8.1   SWAP 7.9"
// Coloured against the frame-rate cap: green at it, yellow below 90 %, red
// below 75 %, with the game's dark outline.
// HOW IT'S DRAWN: by us, on the SDK's overlay layer over the finished picture
// (ImGui's foreground draw list, textured with the font's own pages), in
// both renderers. NOT in F10 photos (they're the guest picture only) and not
// in dual mode's second window.
// HISTORY (2026-10-10): it was a text on the game's own pages first (the
// button prompts' FE_Buttons.pag in menus + the HUD's InGame.pag in play).
// The game hides those pages during every menu transition (a screen's exit
// fade closes its pages, then for a moment no screen is up at all), so the
// counter blinked out with them. Drawing it ourselves depends on nothing.
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
// The text changes 4 times a second (every frame would be unreadable).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/cvar.h>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

REXCVAR_DECLARE(int32_t, fps_overlay);
REXCVAR_DECLARE(int32_t, fps_overlay_position);

namespace fps_overlay {

// The frame listener (CrashMomApp::OnPostSetup).
void Install();

// The drawing (CrashMomApp::OnCreateDialogs, UI thread): `immediate` makes
// the font's textures.
void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);

}  // namespace fps_overlay
