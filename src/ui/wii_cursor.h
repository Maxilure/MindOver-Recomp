// =============================================================================
// ui/wii_cursor.h -- the Wii version's gameplay cursor, for the mouse
// =============================================================================
//
// WHAT THE PLAYER GETS: while a keyboard plays, the pointer IS a glowing
// cyan bubble in every MENU, whatever the Mouse Aiming setting (the Wii
// version points at its menus with the same bubble), and in PLAY too with
// mouse_reticle = 2 ("crosshair"). It stretches along its motion, leaves a short trail on fast
// flicks, and grows over targets. It steps aside while the keyboard's player
// aims as a mask (the aiming reticle follows the mouse then,
// input/mouse_reticle.h) and over the port's own ImGui windows (F6, F5),
// where the system pointer comes back.
//
// WHY A BUBBLE: the Wii version's cursor code is still in the 360 build
// (CGameplayCursorBehaviour, findings/47 s.3), but not its look: the object it
// draws ("MOJO_VAC") isn't on the 360 disc. The 360 data has a round bubble,
// bubble01.bmp (package\bca1bc0b.p3d in default.rcf: a 32 x 32 PNG, grey on
// black, no alpha, drawn additively by the game), read from the player's own
// disc at run time and tinted.
//
// HOW: drawn by us over the finished picture (an ImGui dialog's foreground
// draw list, like the frame-rate counter, fps_overlay.h): it follows the
// pointer with no frame of delay and doesn't depend on the game's pages. When
// to show it is decided once per game frame (a frame-end listener, game
// thread). The look: the bubble in cyan over a soft halo (glow01.bmp, same
// package) and again in near-white for its rim, like the Wii cursor's.
//
// THE GAME'S OWN CURSOR TOO: every Crash on the 360 still carries the Wii
// version's CGameplayCursorBehaviour, never switched on. In crosshair mode the
// port switches it on for the keyboard player's Crash on foot: its Update
// casts a ray through the reticle's spot (= the mouse, input/mouse_reticle.h)
// and puts its mojo collector ("MOJO_VAC") where it lands, like the Wii
// pointer. A second ray from that spot looks for TARGETS (what the aiming
// reticle can lock on to): over one, the bubble grows, as the Wii cursor did
// over things to break. findings/47 s.5.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace wii_cursor {

// Registers the frame-end listener (CrashMomApp, with the other installs).
void Install();
// Switches the game's own cursor behaviour (findings/47 s.5) on for the
// keyboard player's Crash on foot in crosshair mode, off again otherwise.
// Needs a game context: called once per frame from the reticle's Update
// wrapper (input/mouse_reticle.cpp, player 1's reticle). Game thread.
void SwitchGameCursors(PPCContext& ctx, uint8_t* base);
// Makes the overlay (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);

}  // namespace wii_cursor
