// =============================================================================
// ui/tools_menu.h -- the Tools menu (F1): every tool that used to sit on its
// own function key, in one window
// =============================================================================
//
// WHY (2026-10-29): the port had grown a tool on nearly every F key (the
// SDK's F3 debug overlay, F4 settings, F7 achievements, ` log console; the
// port's F2 rename, F5 cheats, F6 controls, F8 second window, F9 renderer,
// F10 photo, F11 highlight, F12 particle experiments). One stray press
// opened a window or switched the renderer in the middle of play. Now:
//   F1  = this menu (open / close)
//   F11 = fullscreen on / off (the usual PC key; window_mode.h)
// and nothing else on the function keys.
//
// HOW: every tool is still an ordinary key bind of the SDK's registry
// (rex::ui::RegisterBind), registered with NO key; the menu runs it by name
// (rex::ui::InvokeBind, SDK patch 0017). So each one can still be given a key
// by hand in settings.toml (e.g. bind_photo = "F10") by whoever wants it back.
// A button that does nothing right now (no native renderer, not on the save
// screen...) answers with the tool's own banner or log line, as the key did.
//
// An ImGui window (a developer / power-user panel, like the Cheats menu: the
// player-facing UI stays in the game's own look).
// =============================================================================
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace tools_menu {

// Creates the window, closed (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer);

// F1 (UI thread): opens it if closed, closes it if open.
void Toggle();

}  // namespace tools_menu
