// =============================================================================
// input/mouse_reticle.h -- the masked player's aiming reticle follows the mouse
// =============================================================================
//
// WHAT THE PLAYER GETS: a player on keyboard and mouse who rides as a mask
// (Aku Aku) and aims (the reticle the projectiles fly to) points it with the
// MOUSE: the reticle sits where the pointer is, anywhere on the 16:9 picture.
// The original moves it with the left stick (WASD on the keyboard).
//
// Setting mouse_reticle (settings.toml):
//   0 = off: the reticle moves with WASD, as in the original;
//   1 = reticle: the mouse moves it (default);
//   2 = crosshair: the mouse moves it, plus the Wii version's gameplay cursor
//       (the port's take on it: a bubble on the pointer, ui/wii_cursor.h).
//
// HOW (findings/47): each player's reticle is a CReticleController in the
// front end (+8420 + 52 x player; players 3-4's are the port's own,
// players/more_players_frontend.cpp). Its Update (vtable 0x820396D4 slot 0,
// sub_82255160) moves a target by the stick while the reticle is on
// (+48 bit 0x80) and the game isn't paused (game object +168 bit 0x20), clamps
// it to the picture (sub_82255360: x within the 16:9 width of the 640 x 480
// canvas, y 0-480, y up) and eases the shown position (+4: two axes of 0x14
// bytes, value at +0x10 each) toward it. After that Update, the port sets
// both target and shown position to the pointer's spot (clamped by the
// game's own sub_82255360, set by sub_822EAA18), for the player the keyboard
// plays as, and the system pointer is hidden while it aims (it would sit on
// top of the reticle; keyboard_mouse.h HidePointerWhileAiming).
// =============================================================================
#pragma once

#include <cstdint>

namespace mouse_reticle {

// The mouse_reticle setting, 0-2 (see above). Any thread.
int Mode();

}  // namespace mouse_reticle
