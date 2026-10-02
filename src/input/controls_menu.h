// =============================================================================
// input/controls_menu.h -- the Controls menu (F6): see and change the keys
// =============================================================================
//
// WHAT: a window over the game (main window) with two tabs.
//
// PLAYERS: every connected device (keyboard and mouse, each controller) and
// which player it plays as (input/players.h): e.g. keyboard = player 1,
// controller = player 2 for the game's two-player co-op.
//
// KEYS: every keyboard / mouse
// action (input/bindings.h) in sections (Moving, Fighting, Titans, Menus...),
// with the Xbox 360 control each one presses (that's what the game's own
// button prompts show) and its two key slots.
//   * Click a key slot, then press the new key / mouse button / wheel notch
//     (Esc cancels). A key already used elsewhere moves here (one key = one
//     action), and the menu says where it came from.
//   * Right-click a slot to empty it.
//   * Two sliders: Spin speed (stick circles per second) and Walk (how far
//     the stick leans).
//   * "Reset keys to defaults" (the Players tab's choices stay).
// Every change is saved to the controls file at once (bindings.h) and takes
// effect immediately. While the menu is open the game gets no keyboard /
// mouse input (keyboard_mouse.h), so clicking around doesn't make Crash jump.
//
// HOW: an ImGui dialog, drawn by the SDK's ImGuiDrawer like overlay_banner.*
// (only in the main window: dual mode's second window has no ImGui). It only
// asks for continuous repaints while open (SDK patch 0006). The "press a
// key" part is KeyboardMouseDriver::Capture: the driver sees every key first
// and hands the next one here.
// =============================================================================
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace controls_menu {

// Creates the dialog, closed (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer);

// F6 (UI thread): opens it if closed, closes it if open. Does nothing (with a
// banner) when there's no keyboard / mouse driver (--keyboard_mouse=false).
void Toggle();

}  // namespace controls_menu
