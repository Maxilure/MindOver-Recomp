// =============================================================================
// options/options_settings.h -- WHAT the Options screen offers: tabs, rows,
// their values, and what changing one does (live, no restart)
// =============================================================================
//
// The screen (options_menu.cpp) only shows rows and turns presses into
// "next / previous value"; everything about a setting lives here:
//   * its label, its values (as the player reads them), a description and
//     an optional warning per value (yellow line: "this changes how the
//     game plays");
//   * where it is read and written: a port setting (a REXCVAR flag, the
//     same ones the launcher's Settings tab and user/settings.toml hold), or
//     the GAME'S OWN option (the three volumes and Invert Axis, kept by the
//     game itself as the original Options screen did);
//   * applying it right away: flags our code reads every frame just work
//     (frame-rate cap, co-op camera); others get a nudge (renderer:
//     NativeRenderer::SetShowNative; fullscreen: the SDK's own change
//     callback resizes the window).
// Port flags changed here are written to user/settings.toml when the screen
// closes (only the changed keys; every other line of the file is kept).
//
// WHAT'S LEFT OUT (2026-10-10, the plan in notes/plans.md s.3): resolution
// above 720p and motion blur (features that don't exist yet), the keyboard
// on / off (the driver is made at start-up only).
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace options_settings {

// What a row's code gets: the hook's registers (for game calls), guest
// memory and the player whose menu this is (front end +8536, 0-3).
struct Env {
  PPCContext& ctx;
  uint8_t* base;
  int player;
};

enum class Kind {
  kChoice,  // a list of values, left / right walks it (no wrap)
  kStars,   // 0-5 stars (the game's volume rows)
  kAction,  // A does something (Rebind Keys)
};

struct Row {
  std::u16string label;
  Kind kind = Kind::kChoice;
  // kChoice: the values in order. Can depend on the moment (a frame-rate cap
  // set by hand to 75 adds "75" to the list).
  std::function<std::vector<std::u16string>(Env&)> values;
  // The current value: an index into values(), or the number of stars.
  std::function<int(Env&)> get;
  // Change it (index / stars, already clamped) and apply it.
  std::function<void(Env&, int)> set;
  // Text under the rows for this value; empty = none.
  std::function<std::u16string(Env&, int)> describe;
  std::function<std::u16string(Env&, int)> warn;
  // kAction: pressing A on the row.
  std::function<void(Env&)> activate;
  // kStars rows: called when the row gets / loses the cursor (Music plays
  // its preview while selected, as in the original screen).
  std::function<void(Env&, bool)> focus;
  // Show the Brightness guide (the calibration grey scale) on this row.
  bool guide = false;
  // An X button action on this row (the prompt bottom right while the row is
  // selected and x_shown says so for its value): Renderer's "Dual Mode".
  std::u16string x_label;
  std::function<bool(Env&, int)> x_shown;
  std::function<void(Env&)> x_action;
};

struct Tab {
  std::u16string name;
  std::vector<Row> rows;
};

// The tabs, built once.
const std::vector<Tab>& Tabs();

// What the screen needs from the rest of the port (set by CrashMomApp).
struct Hooks {
  // The native renderer: null functions = there is none (not Vulkan).
  std::function<bool()> native_shown;        // the main window shows our picture
  std::function<void(bool)> show_native;     // = F9
  std::function<void()> emulated_drawing;    // NativeRenderer::UpdateEmulatedDrawing
  std::function<bool()> emulated_only;       // --emulated_only (the native renderer idle, F9 / F8 locked)
  // Opens the F6 Controls window (keys); runs on the UI thread.
  std::function<void()> open_controls;
  // The window's swapchain made again (a new present mode: V-sync).
  std::function<void()> refresh_present;
  // Dual mode (F8): the second window with the native picture, opened or
  // closed (UI thread).
  std::function<void()> toggle_dual;
};
void SetHooks(Hooks hooks);

// Writes the port flags changed since the last call to user/settings.toml.
void SaveChanged();

}  // namespace options_settings
