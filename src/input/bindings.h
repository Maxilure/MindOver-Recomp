// =============================================================================
// input/bindings.h -- keyboard + mouse controls: the actions, their keys, the file
// =============================================================================
//
// WHAT: the list of everything a keyboard / mouse player can do in Crash: Mind
// over Mutant ("Jump", "Light attack", "Spin", ...), which Xbox 360 control
// each one presses on the game's (virtual) controller, which keys and mouse
// buttons are bound to it, and the small text file that remembers the
// player's choices (`controls.toml` next to the exe).
//
// WHY ACTIONS, NOT "BUTTON A": the game only understands a 360 controller, so
// in the end every key becomes a controller input (input/keyboard_mouse.h
// does that). But players think in moves, and two moves can share one 360
// button (Jump and "confirm in a menu" are both A). So the table below names
// the moves, each pointing at its controller input; the Controls menu (F6,
// input/controls_menu.h) lists them grouped the way a player looks for them.
//
// HOW WE KNOW WHAT EACH BUTTON DOES (docs/findings/23): the game's own data.
//   * Its input map script (script/mixins/inputmap_methods.lua in default.rcf,
//     Lua bytecode) binds controller values to game events: jump, light /
//     heavy attack, block, jack, pocket, special, map, pause...
//   * Its text bank writes button prompts with private characters ("Jump ±",
//     "Map µ"), and its font draws each of those characters as a 360 button
//     picture (± = A, ³ = X, · = RT, ...). Together: which button does what on
//     the 360, in the game's own words.
//   * Spin is not a button: "Rotate the Left Analog Stick" (the game's tip).
//     A stick-circle detector on the controller object reports full turns.
//     So our Spin action draws circles with the virtual stick (keyboard_mouse.h).
//
// THE FILE also remembers which device plays as which player (input/
// players.h): `player.keyboard = 1`, `player.pad:<guid> = 2` (with the
// controller's name as a comment).
//
// THE FILE: plain `name = "Key, Key"` lines, `#` comments, written in full by
// the Controls menu on every change (closing the game window hard-exits the
// process, so there's no "save on quit"). Hand edits work too (game closed).
// Unknown names / keys are logged and skipped; missing actions keep their
// defaults, so an old file keeps working when new actions are added.
//
// Thread safety: none here (plain data). keyboard_mouse.* guards its copy.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include <rex/input/input.h>

namespace kbm {

// ---------------------------------------------------------------------------
// Inputs: one number per key / mouse button / wheel direction.
// ---------------------------------------------------------------------------
// 0x01-0xFE = the SDK's rex::ui::VirtualKey codes (Windows VK_* numbers, which
// also cover the mouse buttons: 0x01 left, 0x02 right, 0x04 middle, 0x05/0x06
// the two side buttons). The wheel has no key code, so it gets two past 0xFF.
using Input = uint16_t;
constexpr Input kNoInput = 0;
constexpr Input kWheelUp = 0x100;
constexpr Input kWheelDown = 0x101;
constexpr size_t kInputCount = 0x102;  // size of per-input arrays

// Config-file name ("W", "Space", "LMB", "WheelUp"); empty if we have none.
std::string InputName(Input input);
// What the Controls menu shows ("W", "Space", "Left mouse", "Wheel up").
std::string InputLabel(Input input);
// Config-file name -> input (case-insensitive, a few aliases such as "Enter"
// and "Esc"); kNoInput if unknown.
Input ParseInput(std::string_view name);
// A mouse button or wheel direction (not a keyboard key)?
bool IsMouseInput(Input input);
// Keys our tools already use (F3 F4 F7 SDK overlays, F6 this menu, F8-F12
// renderer tools, backtick = log console): never bound to game actions, or one
// press would do two things.
bool IsReservedInput(Input input);

// ---------------------------------------------------------------------------
// Actions.
// ---------------------------------------------------------------------------
enum Action : int {
  // Moving.
  kMoveUp,
  kMoveDown,
  kMoveLeft,
  kMoveRight,
  kWalk,  // hold: the stick only half way (slower)
  kSpin,  // hold: full circles with the stick (the game's spin)
  kJump,
  // Fighting.
  kLightAttack,
  kHeavyAttack,
  kBlock,
  // Titans (the big mutants Crash rides).
  kJack,
  kTitanSpecial,
  kPocket,
  // Menus and the rest.
  kMap,
  kPause,
  kMenuConfirm,
  kMenuBack,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  // Rarely needed: the rest of the controller, so every 360 input is reachable.
  // (The right stick doesn't move the camera: it follows Crash by itself.)
  kRightStickUp,
  kRightStickDown,
  kRightStickLeft,
  kRightStickRight,
  kLeftBumper,
  kLeftStickClick,
  kRightStickClick,
  kActionCount
};

// What an action does to the virtual 360 controller.
enum class Output : uint8_t {
  kButton,        // `button` (X_INPUT_GAMEPAD_*) held
  kLeftTrigger,   // LT fully pressed
  kRightTrigger,  // RT fully pressed
  kLeftStick,     // left stick pushed toward (`x`, `y`)
  kRightStick,    // right stick pushed toward (`x`, `y`)
  kWalk,          // shortens the left stick (Settings::walk_tilt)
  kSpin,          // turns the left stick in circles (Settings::spin_turns_per_second)
};

// Menu sections, in display order.
enum class Group : uint8_t { kMoving, kFighting, kTitans, kMenus, kOther };
const char* GroupLabel(Group group);

struct ActionInfo {
  const char* id;         // config-file name ("light_attack")
  const char* label;      // Controls menu text ("Light attack")
  const char* hint;       // menu tooltip: what it does in the game; may be ""
  const char* pad;        // the 360 control it presses, as the game's prompts show it ("X", "RT")
  Group group;
  Output output;
  uint16_t button;        // kButton: X_INPUT_GAMEPAD_*
  int8_t x, y;            // kLeftStick / kRightStick: direction (+y = up)
  const char* defaults;   // default keys, config-file syntax ("LMB, J")
};
const ActionInfo& Info(Action action);

// Up to this many keys per action (the menu's "Key 1" / "Key 2" columns).
constexpr int kSlots = 2;

// Tunables the Controls menu also shows.
struct Settings {
  // Spin: full stick circles per second while the Spin key is held. The game
  // counts a circle and forgets the count when the next one takes longer than
  // 1.0 s (findings/23), so anything from ~1.5 up works; ~3 is a brisk thumb.
  float spin_turns_per_second = 3.0f;
  // Walk: how far the stick leans while Walk is held (1 = all the way).
  float walk_tilt = 0.5f;
};

// A device's player choice (input/players.h), remembered with the name it
// had, so the file says which controller a GUID is.
struct PlayerChoice {
  int player = -2;   // kPlayerAuto (never stored) / kPlayerOff / player 0-3
  std::string name;  // "Keyboard and mouse", "Xbox Series X Controller"
};

// The player's whole setup.
struct Bindings {
  std::array<std::array<Input, kSlots>, kActionCount> keys{};
  Settings settings;
  // Device key ("keyboard", "pad:<guid>[:n]") -> its player. Absent =
  // automatic. File lines: player.<key> = 1-4 or off.
  std::map<std::string, PlayerChoice> players;

  // The built-in layout (ActionInfo::defaults).
  static Bindings Defaults();

  // Is `input` one of `action`'s keys?
  bool Has(Action action, Input input) const;
  // Puts `input` into `action`'s `slot`, taking it away from every other
  // action first (one key = one action; returns the action that lost it, or
  // kActionCount if none). kNoInput clears the slot.
  Action Assign(Action action, int slot, Input input);
};

// Where the file lives: --controls_file, else controls.toml next to the exe.
std::filesystem::path ControlsFilePath();
// Loads `path` over the defaults. Missing file = defaults (logged once).
Bindings LoadBindings(const std::filesystem::path& path);
// Writes the whole file. False (logged) if it can't be written.
bool SaveBindings(const Bindings& bindings, const std::filesystem::path& path);

}  // namespace kbm
