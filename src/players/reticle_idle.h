// =============================================================================
// players/reticle_idle.h -- a mask's aiming reticle hides while nobody aims
// =============================================================================
//
// WHAT THE PLAYER GETS: a co-op player riding as a mask (Aku Aku on a Crash
// on foot) has an aiming reticle on screen all the time in the original,
// even while they don't aim at all: a big red / green circle in the middle of
// the picture. Now that reticle shows only while it's used: it appears the
// moment its player moves it (stick, WASD or the mouse) or shoots, and goes
// away again after `reticle_hide_idle_ms` (default 2000 ms) of no aiming.
// A player who just became a mask starts with it hidden.
//
// Only for plain masks. A mask whose host rides a titan (the reticle then
// aims the mutant's abilities) keeps the original's always-on reticle, and
// so does anyone who isn't a mask (co-op state 1). 0 = the original rule.
//
// HOW (findings/26 s.57): two objects make up a reticle.
//  - CReticleController (front end +8420 + 52 x player; players 3-4's ours):
//    the 2D spot the stick moves (its Update sub_82255160, wrapped in
//    input/mouse_reticle.cpp, which also puts it on the mouse). That wrapper
//    tells us, per player, whether the spot moved this frame
//    (NoteReticleUpdate).
//  - CReticleAimingBehaviour (one per host actor, its Update sub_82161B40):
//    the reticle that is DRAWN (draw sub_82162540, over the table 0x8259AFAC
//    or, with 3-4 players, the port's own table in more_players_frontend.cpp).
//    Its ray reads the controller of a player (sub_822AD0A0, by controller ->
//    sub_822661B8): that read tells us which player a behaviour aims for.
// Hiding = the reticle's own switch-off animation (LOCK OUT) played on the
// behaviour only; aiming again = its LOCK IN. The 2D spot stays on, so moving
// it still counts. A fresh mask that never aimed is hidden at once (no
// lock in + lock out on becoming a mask).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(int32_t, reticle_hide_idle_ms);

namespace reticle_idle {

// Game thread, around the original CReticleAimingBehaviour Update
// (r3 = behaviour, r4 = its owner = the host actor: a Crash or a titan).
// Switches a reticle that should hide / show with ITS OWN animations (lock
// out / lock in) before the Update, and keeps the Update from undoing that.
void BeginBehaviourUpdate(PPCContext& ctx, uint8_t* base, uint32_t behaviour, uint32_t owner);
void EndBehaviourUpdate();
// The behaviour is destroyed (its destructor body, sub_82161A08).
void Forget(uint32_t behaviour);

// Game thread, after a CReticleController Update (mouse following included):
// its player, whether it's on (+48 bit 0x80) and whether its spot moved.
void NoteReticleUpdate(PPCContext& ctx, uint8_t* base, int player, bool on, bool moved);

// Game thread: true = this behaviour's reticle should be hidden now (rule).
bool Hidden(uint32_t behaviour);
// Game thread, in the draw: true = don't draw it (hidden, lock out finished).
bool SkipDraw(uint32_t behaviour);

}  // namespace reticle_idle
