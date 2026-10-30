// =============================================================================
// input/background_input.h -- what happens while the game window isn't focused
// =============================================================================
//
// WHY: a PC window can lose the focus (another window clicked, Alt+Tab, a
// second monitor). The SDK's input check gave the game NO controller input
// then (ReXApp::ConstructRuntime's "active" check): the game kept running
// with nobody at the controls, so Crash stood still while enemies hit him.
// Two ways out, picked by one setting, `background_input`:
//
//   * ON (the default): CONTROLLERS KEEP PLAYING. Two things stood in the way:
//       1. the SDK's "active" check (replaced by the app's own, which asks
//          Allowed() here: crash_mom_app.h UseEitherWindowForInput);
//       2. SDL itself drops controller events while none of its windows has
//          the keyboard focus (SDL_joystick.c ShouldIgnoreEvent), unless the
//          hint SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS is "1". The hint is
//          watched by SDL (a hint callback), so it can change while running.
//     The keyboard and mouse still stop (their keys go to the other window).
//
//   * OFF: THE GAME PAUSES ITSELF the moment the window loses the focus in a
//     level, with the game's own pause menu, as if START had been pressed.
//     No START is faked (a fake press could leak into the next screen): the
//     front end's in-game decision (sub_8211E850, the fight-tree node that
//     opens the pause menu on START, findings/26 s.31) is wrapped, and when
//     it has nothing else to do (returns -1) it takes the pause menu's exit,
//     516, the same one START's check takes (0x8211EAF4 -> 0x8211EB18), after
//     the same two checks START makes (not paused already, not a cutscene).
//     Once per focus loss: closing the menu while still unfocused plays on.
//     The pause stays up when the focus comes back (the player closes it).
//     Outside a level (menus, loading) nothing pauses; a focus lost there
//     pauses at the level's first playable frame, unless it came back first.
//
// Threads: Allowed() / Apply() any thread; the focus check is set once by the
// app (UI state read from game and input threads, like the SDK's own check).
// =============================================================================
#pragma once

#include <functional>

namespace background_input {

// Is the setting on (controllers play while the window isn't focused)?
bool Allowed();

// Tells SDL the setting's current value (the background-events hint). Called
// at start-up and whenever the setting changes (Options row); cheap.
void Apply();

// The app's "is one of the game's windows focused?" (main or second window).
void SetFocusCheck(std::function<bool()> focused);

// Is a game window focused now? True until the app gives a check.
bool Focused();

}  // namespace background_input
