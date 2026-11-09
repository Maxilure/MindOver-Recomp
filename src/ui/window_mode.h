// =============================================================================
// ui/window_mode.h -- Windowed / Borderless / Fullscreen and which monitor
// =============================================================================
//
// Options -> Display -> "Window Mode" (Windowed / Fullscreen) and "Monitor"
// (2026-10-24, findings/50).
//
// The SDK already has the three switches, each with a change callback that
// moves the window right away (ReXApp::SetupPresentation, window_sdl.cpp):
//
//   fullscreen            false = a window you can move and resize
//   fullscreen_exclusive  with fullscreen: switch the monitor's video mode
//                         (SDL_SetWindowFullscreenMode with a real mode);
//                         false = "borderless": a window covering the screen
//                         at the desktop's own mode (SDL desktop fullscreen)
//   monitor               0 = wherever the desktop puts the window, 1..N =
//                         SDL_GetDisplays()'s order (window centred there,
//                         fullscreen on it)
//
// This module turns them into three modes (Windowed, Fullscreen = borderless,
// Borderless Window = our own flag borderless_window, see below), and keeps a list
// of the connected monitors (names, sizes, refresh rates) for the Monitor
// row. SDL's display calls belong on the UI thread, while the Options rows
// run on the game's thread: the list is read on the UI thread and cached.
//
// WAYLAND (Linux desktops like Hyprland, GNOME, KDE): a program can't place
// its own window, so Monitor only moves a FULLSCREEN window (windowed = the
// desktop decides).
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace window_mode {

// Fullscreen = the BORDERLESS kind (fullscreen_exclusive false): the port
// offers no video-mode switch (it worked well only on Windows; on Linux
// desktops it looked the same and mixed up monitors). A settings file with
// fullscreen_exclusive = true still reads as Fullscreen, and picking a mode
// in Options sets it back to false.
//
// BORDERLESS WINDOW (2026-11-09): an ordinary window (fullscreen flag OFF)
// with no border, placed and sized over the whole monitor by us (SDL
// SetWindowBordered / SetWindowPosition / SetWindowSize on the UI thread).
// Why a third mode when Fullscreen is already borderless: on Windows, a
// window SDL marks as fullscreen can get its frames sent straight to the
// screen past the desktop compositor ("independent flip"); screen sharing
// that captures through the compositor (Discord) then shows one stale frame.
// Windowed shares fine. Whether an unmarked window over the same area is
// treated differently depends on Windows and the GPU driver: this mode is
// the try. debug_borderless_window_gap = pixels left off the bottom (a
// window that doesn't cover the monitor is never flipped past the
// compositor) for players to test when the exact size still freezes.
// Wayland: the compositor places windows, so only the size applies.
enum class Mode : int { kWindowed = 0, kFullscreen = 1, kBorderlessWindow = 2 };

// One connected monitor, in SDL_GetDisplays() order (= the monitor flag's
// 1..N numbering).
struct Display {
  std::string name;   // SDL_GetDisplayName (the monitor's model, or the port name)
  std::string short_name;  // brand + model ("Acme X27Q"): the Options row's value
  std::string port;        // the connector ("DP-2"), when the name ends with one
  int width = 0;      // its desktop mode
  int height = 0;
  float refresh = 0;  // Hz
  bool primary = false;
};

// At setup: how to run work on the UI thread (app_context().CallInUIThread)
// and the call that re-reads the monitor's refresh rate (V-sync paces to it).
// Reads the monitor list once.
// `main_title` = the main window's title: the SDL window with it is the one
// the Borderless Window mode moves (the dual-mode 2nd window isn't).
void Install(std::function<void(std::function<void()>)> post_to_ui, std::function<void()> on_moved,
             std::string main_title);

// The mode the three flags describe now.
Mode Current();
// Sets the flags for a mode (through `set_flag`, the Options screen's setter
// that also remembers them for settings.toml). Exclusive off first, so the
// window goes straight to the borderless kind of fullscreen.
void Set(Mode mode, const std::function<void(const char*, const std::string&)>& set_flag);

// The monitors as last read (any thread), and a fresh read requested (the
// Options screen asks whenever its rows are drawn: monitors plugged in later).
std::vector<Display> Displays();
void RefreshDisplays();

// After a mode or monitor change: the window may land on another monitor a
// moment later (Wayland answers asynchronously), so the refresh rate and the
// list are read again now and twice more within 1.5 s.
void AfterChange();


}  // namespace window_mode
