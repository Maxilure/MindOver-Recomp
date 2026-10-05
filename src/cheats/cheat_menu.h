// =============================================================================
// cheats/cheat_menu.h -- the Cheats menu (F5): testing cheats over the game
// =============================================================================
//
// WHAT: a window over the game (main window) with the cheats of cheats.h and
// free_camera.h, for testing:
//   CHARACTER (pick player 1-4): what they play as (Crash or a titan), its
//     level, mojo toward the next upgrade, health; Level up, Max level,
//     +100 / +1,000 / +10,000 mojo, Refill health, Free jack.
//   SPAWN: titans (knocked out = one hit from down, then jack; or ready to
//     fight), enemies (untested), or any template by name (experimental), beyond
//     that player as the camera sees it (spawn.h).
//   EVERYONE: God mode.
//   TIME: game speed (0.1x - 4x), Freeze, Step one frame.
//   CAMERA: Free camera (fly with W A S D, E / Q, right mouse button = look).
//   SCREEN: Hide / show the HUD.
// Unlike the Controls menu (F6), the game keeps running and keeps its keys
// while this window is open, so a cheat can be watched as it happens; only
// the mouse over the window stays out of the game (ImGui takes it).
//
// HOW: an ImGui dialog like controls_menu.*; the buttons only queue requests
// (cheats.h), which the game's own thread carries out at its next frame. It
// repaints with the game's frames, not continuously (see WantsContinuousRepaint).
// F5 with Right Shift held is ignored: that's MangoHud's logging key
// (tools/mangohud/), and our keys don't look at Shift.
// =============================================================================
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace cheat_menu {

// Creates the dialog, closed (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer);

// F5 (UI thread): opens it if closed, closes it if open.
void Toggle();

}  // namespace cheat_menu
