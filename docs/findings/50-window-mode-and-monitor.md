# 50. Window mode and monitor

Options -> Display gets two rows in place of the old Fullscreen On / Off:
**Window Mode** (Windowed / Fullscreen, the fullscreen being the borderless
kind) and **Monitor** (Automatic or one of the connected monitors: the one
Fullscreen fills). Both change live and are saved
in `user/settings.toml` when the Options screen closes. The launcher's
Settings tab has the same two choices under "Window".

Code: `src/ui/window_mode.*` (modes, monitor list), rows in
`src/options/options_settings.cpp` (WindowModeRow, MonitorRow), launcher
`launcher/settings_page.cpp` (section "Window").

## 1. What was already there

The SDK (ReXGlue) already had three window flags, each with a change
callback that updates the window on the UI thread
(`ReXApp::SetupPresentation`, `window_sdl.cpp`):

| Flag | Default | What it does |
|---|---|---|
| `fullscreen` | true | the window covers the screen (SDL_SetWindowFullscreen) |
| `fullscreen_exclusive` | false | with fullscreen: switch the monitor to a real video mode (SDL_SetWindowFullscreenMode with a mode; the `resolution` flag picks it, else the desktop's); false = SDL's "desktop fullscreen", a borderless window at the desktop's mode |
| `monitor` | 0 | 0 = where the desktop puts the window; 1..N = `SDL_GetDisplays()` order: the window is centred on that display (SDL_WINDOWPOS_CENTERED_DISPLAY), and goes fullscreen there |

So no SDK change was needed: the port's rows set these flags.

## 2. Two modes, fullscreen = borderless

| Mode | fullscreen | fullscreen_exclusive |
|---|---|---|
| Windowed | false | false |
| Fullscreen | true | false |

A first version offered three modes (Windowed / Borderless / Fullscreen with a
real video-mode switch). The mode switch only behaves well on Windows; on
Linux desktops it looks the same as borderless and moving between monitors
gets unreliable, so the port keeps one fullscreen: the borderless kind (a
window covering the monitor at the desktop's own mode, quick to switch away
from). A settings file with `fullscreen_exclusive = true` still reads as
Fullscreen, and choosing a mode in Options or the launcher sets it back to
false. The exclusive flag goes first, so the window is switched once.

## 3. The monitor list

SDL's display calls belong on the UI thread; the Options rows run on the
game's thread. `window_mode` reads `SDL_GetDisplays` / `SDL_GetDisplayName` /
`SDL_GetDesktopDisplayMode` on the UI thread and keeps a copy for the rows.
The row asks for a new read each time it is drawn, so a monitor plugged in
later appears the next time.

Names on Linux are long: maker, model, serial number and connector, e.g.
`Acme Display Corporation X27Q SER ABC123456789 (DP-2)`. The row shows a short
name = the first word with a digit (the model) and the word before it (the
brand), skipping company words (Corporation, Technology, Group, Inc., ...)
and serial numbers (words with 5+ digits): `Acme X27Q`. The side card
title shows the same; its text just says Fullscreen fills that monitor. A
monitor picked earlier that is no longer connected stays in the list as
"Not Connected"; the window then stays where it is.

After a change the monitor's refresh rate is read again (V-sync paces the
game to it, `frame_rate::UpdateDisplayRefresh`): right away and twice more
within 1.5 s, because a Wayland compositor moves the window when it gets to it.

## 4. Wayland (most Linux desktops)

A Wayland program cannot place its own window: in Windowed mode the Monitor
choice does nothing (the desktop decides). Fullscreen does go to the chosen
monitor, which is what the row is for.

## 5. Other window shapes

The picture is always the game's 16:9. In a window of any other shape the
presenter fits it inside with black bars (the SDK's `present_letterbox`, on
by default), and the port's own pointer maths (mouse in menus, the Wii-style
cursor) fit the same 16:9 rectangle. Menus and the HUD are therefore never
stretched or cut, whatever the window's shape.

A wider picture (21:9, 32:9) would mean more: the camera's aspect (VectorCamera
+28) and field of view, the HUD's corner pieces anchored to the wider edges,
full-screen passes (depth of field, the mojo flash, shadows' full-screen quad)
and the movies pillarboxed. Not done.

## 6. Tested

Console-driven runs on an installed-style test copy, several monitors connected
(a Wayland desktop): Fullscreen -> Windowed -> Fullscreen live, starting from an old
`fullscreen_exclusive = true` (set back to false at the first change);
Monitor = 1 live and saved; a fresh start with the saved monitor opens
fullscreen there; a start with `--fullscreen=false` opens a window. 0 errors.
Not tested: moving to another monitor (kept off screens in use), Windows.
