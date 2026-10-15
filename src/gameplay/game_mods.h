// =============================================================================
// gameplay/game_mods.h -- GAME MODIFICATIONS: options that change how the game
// PLAYS (the Options screen's "Gameplay" tab; 2026-10-15, findings/40)
// =============================================================================
//
// Every one of these is a change to the original game's rules, so each is a
// switch whose default is the ORIGINAL behaviour except where noted:
//
//   * friendly_fire (default true = original): players' attacks hurt each
//     other in co-op. false = a partner's hit passes harmlessly: no damage,
//     no stagger, no combo lost.
//   * friendly_hits_break_combo (default FALSE, the one default that isn't the
//     original): in the original, getting hit by a partner ends your hit combo
//     (the green star meter). false = it doesn't.
//     (The hitter never gets a combo point for a partner, in the original
//     too.) Only matters while friendly_fire is on.
//   * simple_spinning (default false = original): spin with ONE BUTTON on a
//     controller: LB (a debug button in play, otherwise unused) answers the
//     fight trees' "did the left stick just make a full circle?" like the
//     keyboard's Spin key does (findings/38 s.4); held = one spin after
//     another. The keyboard's Spin key is part of it too: off = it does
//     nothing (input/keyboard_mouse.cpp), and its Options card says so. With
//     it on, the spin prompts get a second row with the button
//     (input/prompt_icons.cpp SpinRows).
//   * keep_spinning (default false = original; needs simple_spinning): HOLD
//     the spin to keep spinning. The original's continuous spin is the "Old
//     Skool" move: spin once, then press Light attack again and again. While
//     on, a spin that was just started keeps getting those Light attack
//     presses for as long as the player keeps spinning: LB or the Spin key
//     held, or the left stick still circling.
//
// HOW THE GAME DECIDES A HIT (found 2026-10-15, findings/40 s.1):
//   Each character's CDamageableBehaviour (vtable 0x8202D2E4) gets its hits
//   as MESSAGES through vtable slot 5 (sub_82132FF8: r3 = the behaviour, r4 =
//   its actor, r5 = the message). Message type (+4) 12 = damage; subtype
//   (+16) 0 = a hit (damage, stagger, hit reaction, combo messages), 3 = a
//   direct hitpoint change. The hit's record = *(*(message + 20) + 4):
//   +20 = the actor that hit (also what the hitpoint change sub_821349E0
//   gets in r5), +40 = the actor that gets the combo point. On a hit the
//   behaviour sends message 31 subtype 1 ("you were hit") to its own actor
//   and 31 subtype 0 ("you landed one") to the +40 actor; each player's
//   CScoringBehaviour (slot 5 = sub_82166198) turns those into the combo
//   meter's "break" (sub_821661E8) / "one more hit" (sub_82166240).
//   Which player an actor belongs to = the game's own sub_8226FCD8 (Crash,
//   jacked titan, mask: players 1-4 with more_players' hooks).
// =============================================================================
#pragma once

#include <cstdint>

namespace game_mods {

// The keyboard's / a controller's state for socket `user` (= player, 0-3),
// right after the game read it (the XInputGetState wrapper in
// save_library.cpp): simple spinning takes LB here, keep spinning adds its
// Light attack presses.
// `gamepad` = the guest X_INPUT_GAMEPAD (big-endian).
void FilterPad(uint32_t user, uint8_t* gamepad, bool connected);

// The spin question (HasControlDone360, input/keyboard_mouse.cpp) answered
// "yes" for player p's Crash on foot: a spin starts.
void SpinStarted(int p);

// Simple spinning's button (LB) made a full circle for player p that the
// fight tree hasn't heard yet: true once (consumed), like the keyboard's
// TakeSpinCircle. Game threads.
bool TakeButtonSpin(int p);

// Keep spinning is feeding player p's spin right now (the keyboard's Spin
// key then hands out no new circles: a fresh spin would cut the old one).
bool Feeding(int p);

}  // namespace game_mods
