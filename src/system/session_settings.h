// =============================================================================
// system/session_settings.h -- THE SETTINGS THAT CHANGE THE WORLD
// =============================================================================
//
// WHY (2026-11-17, toward LAN / online co-op). In a lockstep session every
// PC runs the whole game and only the players' inputs travel. That works only
// while every PC's game makes the same decisions, and many of the port's
// settings change decisions: friendly fire, Keep Spinning, the ground grace,
// the co-op rules, how the helper threads are timed... One PC with a
// different value and the sessions part (a "desync"), often minutes later.
//
// So this is the list of every setting that changes the WORLD (game logic and
// its timing), each with the reason, and a FINGERPRINT of their values: a
// session's PCs compare fingerprints when they connect (later: the host sends
// its values). Settings that only change the PICTURE, the SOUND's loudness,
// menus' looks, the controls on this PC (keys, mouse, pads) or the tools are
// not in it: each PC keeps its own.
//
// Also not in it: the port's NATIVE REPLACEMENTS of game functions
// (native_matrices, native_skinning, ...): each is checked bit-identical to
// the original by its checker (decomp/check.h), so on and off give the same
// results. Turning one on that ISN'T proven bit-identical belongs here.
//
// Rule for new settings: if it changes what the game decides (or WHEN), add
// it to kWorldSettings in session_settings.cpp.
//
// Debug console: `settings` = the world settings, their values and the
// fingerprint; logged at a session's start too (fixed_step::StartSession).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace session_settings {

// FNV-1a 64 over "name=value\n" of every world setting, in table order.
uint64_t Fingerprint();

// One line per world setting ("name = value  (reason)") + the fingerprint.
std::string Report();

}  // namespace session_settings
