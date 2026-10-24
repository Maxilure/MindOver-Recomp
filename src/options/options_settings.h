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
//     same ones the launcher's Settings tab and user/settings.toml hold).
//     The GAME'S OWN options (the three volumes and Invert Axis) are flags
//     too since 2026-10-11, pushed into the game by global_options.cpp (the
//     360 kept them in every save file);
//   * applying it right away: flags our code reads every frame just work
//     (frame-rate cap, co-op camera); others get a nudge (renderer:
//     NativeRenderer::SetShowNative; fullscreen: the SDK's own change
//     callback resizes the window).
// Port flags changed here are written to user/settings.toml when the screen
// closes (only the changed keys; every other line of the file is kept).
//
// WHAT'S LEFT OUT (2026-10-10, the plan in notes/plans.md s.3): resolution
// above 720p (doesn't exist yet), the keyboard on / off (the driver is made
// at start-up only). Motion blur joined on 2026-10-11 (Graphics tab).
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
  kAction,  // A does something (Default Keys)
  kKey,     // a keyboard / mouse action's keys (the Controls tab): A / X =
            // press a new first / second key, Y = remove one (options_menu.cpp)
  kHeader,  // a heading between rows (the keys' "Keyboard and Mouse Keys"):
            // the label only, in the titles' cyan, never selected
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
  // kAction: a question asked first in the game's Yes / No box ("Put every
  // key back?"); activate() runs only on Yes. Empty = no question.
  std::u16string confirm;
  // kStars rows: called when the row gets / loses the cursor (Music plays
  // its preview while selected, as in the original screen).
  std::function<void(Env&, bool)> focus;
  // GREYED OUT: a non-empty reason = the row can't be used right now (a
  // native-renderer effect while the emulated picture is shown): drawn
  // see-through, its value can't change, the reason as the card's warning.
  std::function<std::u16string(Env&)> unavailable;
  // kKey: which keyboard / mouse action (kbm::Action, input/bindings.h).
  int key_action = -1;
  // Show the Brightness guide (the calibration grey scale) on this row.
  bool guide = false;
  // An X button action on this row (the prompt bottom right while the row is
  // selected and x_shown says so for its value): Renderer's "Dual Mode".
  std::u16string x_label;
  std::function<bool(Env&, int)> x_shown;
  std::function<void(Env&)> x_action;
  // A question asked first in the game's Yes / No box ("Restart now?"):
  // x_action runs only on Yes. Empty text = none (x_action at once).
  std::function<std::u16string(Env&)> x_confirm;
};

struct Tab {
  std::u16string name;
  std::vector<Row> rows;
};

// The tabs, built once.
const std::vector<Tab>& Tabs();

// The keys of a keyboard / mouse action as a row shows them ("Left Mouse,
// J"; "None"), and the action's name as the rows write it ("Light Attack").
// For the Controls tab's key rows (options_menu.cpp).
std::u16string KeysText(int action);
// The same keys as PICTURES (input/prompt_icons.h KeyGlyph: one font
// character per key, space between), "" when there are none or no pictures.
std::u16string KeyPictures(int action);
std::u16string ActionName(int action);

// What the screen needs from the rest of the port (set by CrashMomApp).
struct Hooks {
  // The native renderer: null functions = there is none (not Vulkan).
  std::function<bool()> native_shown;        // the main window shows our picture
  std::function<void(bool)> show_native;     // = F9
  std::function<void()> emulated_drawing;    // NativeRenderer::UpdateEmulatedDrawing
  std::function<bool()> emulated_only;       // --emulated_only (the native renderer idle, F9 / F8 locked)
  // The window's swapchain made again (a new present mode: V-sync).
  std::function<void()> refresh_present;
  // Dual mode (F8): the second window with the native picture, opened or
  // closed (UI thread).
  std::function<void()> toggle_dual;
};
void SetHooks(Hooks hooks);

// Sets a port flag by name (the F4 settings' parsing and checks) and
// remembers it for SaveChanged. For settings changed outside the Options
// screen (the outfits picked at the chest, players/outfits.h).
void SetFlagToSave(const char* name, const std::string& value);

// Writes the port flags changed since the last call to user/settings.toml.
void SaveChanged();

}  // namespace options_settings
