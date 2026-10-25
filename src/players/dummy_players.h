// =============================================================================
// players/dummy_players.h -- dummy controllers you can half-steer, for co-op tests
// =============================================================================
//
// WHAT: testing 3-4 players needs 3-4 controllers. The fake controllers of
// debug_input_script.h (--debug_fake_pads=1-3, or `debug_fake_pads = 3` in
// user/settings.toml: "Dummy controller 2..4", players 2-4 by default, movable
// in F6 like any pad) can now be driven from the F5 Cheats menu's "Dummy
// players" section, each in one of three ways:
//   IDLE    nothing but what you click: the menu's buttons (START joins, B
//           leaves a mask, A jumps, ...) and its hold-to-move stick pad.
//   FOLLOW  walks after a player you pick (left stick toward them, relative to
//           the camera like a person would push it), stops ~3 units away, and
//           jumps when it stays stuck below them (a ledge, a step).
//   COPY    repeats a player's controller exactly as the game read it (sticks,
//           triggers, buttons), minus START and BACK (pause / menus stay yours).
// The menu's buttons and stick pad work in every mode (the stick pad wins
// over follow / copy while held).
//
// HOW: once per game frame (a frame-end listener, game thread) each dummy's
// held state is worked out and handed to the fake controller driver
// (ScriptedInputDriver::SetLiveState: analog sticks); the game reads it at its
// next controller poll like a real pad. Follow reads the players' positions
// (world matrix) and the game camera's direction (VectorCamera, findings/27);
// copy reads input_record::LastRead (what the game got from that player).
// A dummy's player = the player its device plays as (input/players.h), so a
// dummy moved in F6 still acts as itself.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace dummy_players {

enum class Mode { kIdle, kFollow, kCopy };

// Dummies 0..2 = "Dummy controller 2..4" (the fake controllers after the first).
constexpr int kMaxDummies = 3;

// How many dummies exist this session (--debug_fake_pads; 0 = none). Any thread.
int Count();
// The player (0-3) dummy `d` plays as right now, -1 if none. Any thread.
int PlayerOf(int d);

// Mode + the player it follows / copies (0-3). Any thread (the menu).
void SetMode(int d, Mode mode, int target);
Mode GetMode(int d);
int GetTarget(int d);

// The menu's stick pad: held direction (-1..1 each, +y = up / away), 0,0 =
// let go. Any thread.
void SetHeldStick(int d, float x, float y);
// The menu's buttons: "a", "b", "start", "lt"... (debug_input_script.h
// names), tapped for `hold_ms`. False if the dummy doesn't exist.
bool Tap(int d, const std::string& input, int hold_ms = 150);

// WHICH CONTROLLER A DUMMY PRETENDS TO BE (for the Automatic button pictures,
// input/prompt_icons.h): the detector reads a controller's maker and model
// from its SDL GUID; a dummy given a model hands the detector a real-looking
// GUID of that model (Xbox 360, Xbox Series, DualShock 4, DualSense, Switch
// Pro, another maker), so detection runs as for a plugged-in pad. Model 0 =
// none (the dummy's own "debug-pad-N" id: --debug_fake_pad_styles or the 360
// look). The dummy's player slot is untouched (F6 keys it by "debug-pad-N").
int ModelCount();
const char* ModelName(int model);
void SetModel(int d, int model);
int GetModel(int d);
// The device key the detector should read for dummy d ("pad:<GUID>"), "" =
// none chosen. Any thread.
std::string DetectorKey(int d);

// Registers the per-frame update (CrashMomApp::OnPreLaunchModule).
void Install();

}  // namespace dummy_players
