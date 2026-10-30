// =============================================================================
// input/bindings.cpp -- see bindings.h
// =============================================================================
#include "bindings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "game_folder.h"

REXCVAR_DEFINE_STRING(controls_file, "", "CrashMoM",
                      "Keyboard + mouse controls file (empty = user/controls.toml in the game folder; "
                      "written by the Controls menu, F1 Tools)");

namespace kbm {
namespace {

using VK = rex::ui::VirtualKey;
constexpr Input V(VK key) { return static_cast<Input>(key); }
using namespace rex::input;  // X_INPUT_GAMEPAD_* button bits

// ---------------------------------------------------------------------------
// Key names.
// ---------------------------------------------------------------------------
// `name` is what the file says; the SDK's own key names (rex/ui/keybinds.cpp,
// used by --keybind_* and the F-key binds) where it has one, so the same
// spelling works everywhere. `label` is what the menu shows.
struct KeyName {
  Input input;
  const char* name;
  const char* label;
};
constexpr KeyName kKeyNames[] = {
    // Mouse.
    {V(VK::kLButton), "LMB", "Left mouse"},
    {V(VK::kRButton), "RMB", "Right mouse"},
    {V(VK::kMButton), "MMB", "Middle mouse"},
    {V(VK::kXButton1), "Mouse4", "Mouse 4 (back)"},
    {V(VK::kXButton2), "Mouse5", "Mouse 5 (forward)"},
    {kWheelUp, "WheelUp", "Wheel up"},
    {kWheelDown, "WheelDown", "Wheel down"},
    // Big keys and modifiers. SDL reports left and right Shift / Ctrl / Alt
    // as one key each (rex/ui/sdl_virtual_key.cpp), so there's no "LShift".
    {V(VK::kSpace), "Space", "Space"},
    {V(VK::kReturn), "Return", "Enter"},
    {V(VK::kEscape), "Escape", "Esc"},
    {V(VK::kTab), "Tab", "Tab"},
    {V(VK::kBack), "Backspace", "Backspace"},
    {V(VK::kShift), "Shift", "Shift"},
    {V(VK::kControl), "Control", "Ctrl"},
    {V(VK::kMenu), "Alt", "Alt"},
    {V(VK::kCapital), "CapsLock", "Caps Lock"},
    // Navigation block.
    {V(VK::kUp), "Up", "Up arrow"},
    {V(VK::kDown), "Down", "Down arrow"},
    {V(VK::kLeft), "Left", "Left arrow"},
    {V(VK::kRight), "Right", "Right arrow"},
    {V(VK::kInsert), "Insert", "Insert"},
    {V(VK::kDelete), "Delete", "Delete"},
    {V(VK::kHome), "Home", "Home"},
    {V(VK::kEnd), "End", "End"},
    {V(VK::kPrior), "PageUp", "Page Up"},
    {V(VK::kNext), "PageDown", "Page Down"},
    // Punctuation (US layout names; SDL reports the key's POSITION, so on
    // other layouts the label is the US key at that spot).
    {V(VK::kOem3), "Backtick", "`"},
    {V(VK::kOemMinus), "Minus", "-"},
    {V(VK::kOemPlus), "Plus", "="},
    {V(VK::kOem4), "LBracket", "["},
    {V(VK::kOem6), "RBracket", "]"},
    {V(VK::kOem5), "Backslash", "\\"},
    {V(VK::kOem1), "Semicolon", ";"},
    {V(VK::kOem7), "Quote", "'"},
    {V(VK::kOemComma), "Comma", ","},
    {V(VK::kOemPeriod), "Period", "."},
    {V(VK::kOem2), "Slash", "/"},
    // Numpad extras (digits are generated below).
    {V(VK::kAdd), "NumpadPlus", "Numpad +"},
    {V(VK::kSubtract), "NumpadMinus", "Numpad -"},
    {V(VK::kMultiply), "NumpadStar", "Numpad *"},
    {V(VK::kDivide), "NumpadSlash", "Numpad /"},
    {V(VK::kDecimal), "NumpadPeriod", "Numpad ."},
    // Rarely used.
    {V(VK::kPause), "Pause", "Pause"},
    {V(VK::kScroll), "ScrollLock", "Scroll Lock"},
    {V(VK::kNumLock), "NumLock", "Num Lock"},
    {V(VK::kSnapshot), "PrintScreen", "Print Screen"},
    {V(VK::kApps), "Menu", "Menu key"},
};

// A few extra spellings people write by hand.
struct Alias {
  const char* alias;
  Input input;
};
constexpr Alias kAliases[] = {
    {"Enter", V(VK::kReturn)},       {"Esc", V(VK::kEscape)},
    {"Ctrl", V(VK::kControl)},       {"Mouse1", V(VK::kLButton)},
    {"Mouse2", V(VK::kRButton)},     {"Mouse3", V(VK::kMButton)},
    {"LeftMouse", V(VK::kLButton)},  {"RightMouse", V(VK::kRButton)},
    {"MiddleMouse", V(VK::kMButton)}, {"NumpadEnter", V(VK::kReturn)},
};

bool EqualsNoCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

// ---------------------------------------------------------------------------
// The action table (order = the Action enum). Defaults: WASD + mouse, the
// usual PC layout, with the titan moves on E / F / R next to the movement
// keys, and the menus on Enter / Backspace / Esc / arrows.
// ---------------------------------------------------------------------------
constexpr uint16_t kA = X_INPUT_GAMEPAD_A, kB = X_INPUT_GAMEPAD_B;
constexpr uint16_t kX = X_INPUT_GAMEPAD_X, kY = X_INPUT_GAMEPAD_Y;

constexpr Context kPlay = Context::kPlay, kMenu = Context::kMenu, kBoth = Context::kBoth;

constexpr ActionInfo kActions[kActionCount] = {
    // id, label, hint, pad, group, output, button, x, y, defaults, context
    {"move_up", "Move up", "Also moves through menus.", "Left stick", Group::kMoving,
     Output::kLeftStick, 0, 0, 1, "W", kBoth},
    {"move_down", "Move down", "", "Left stick", Group::kMoving, Output::kLeftStick, 0, 0, -1, "S",
     kBoth},
    {"move_left", "Move left", "", "Left stick", Group::kMoving, Output::kLeftStick, 0, -1, 0, "A",
     kBoth},
    {"move_right", "Move right", "", "Left stick", Group::kMoving, Output::kLeftStick, 0, 1, 0, "D",
     kBoth},
    {"walk", "Walk (hold)",
     "Hold with a direction: the stick leans only part of the way, so Crash goes slower.",
     "Left stick", Group::kMoving, Output::kWalk, 0, 0, 0, "Control", kPlay},
    {"spin", "Spin",
     "The spin attack (on a controller: \"Rotate the Left Analog Stick\"). Hold to keep "
     "spinning. With other keys: the spin moves (Spin, then Heavy attack; Spin, then Jack to "
     "jump off a titan...). Crash still moves with the movement keys while spinning.",
     "Left stick circles", Group::kMoving, Output::kSpin, 0, 0, 0, "Q, MMB", kPlay},
    {"jump", "Jump", "Press again in the air for a double jump.", "A", Group::kMoving,
     Output::kButton, kA, 0, 0, "Space", kPlay},

    {"light_attack", "Light attack", "", "X", Group::kFighting, Output::kButton, kX, 0, 0,
     "LMB, J", kPlay},
    {"heavy_attack", "Heavy attack", "", "Y", Group::kFighting, Output::kButton, kY, 0, 0,
     "RMB, K", kPlay},
    {"block", "Block (hold)", "Hold to block an enemy's light attack.", "RT",
     Group::kFighting, Output::kRightTrigger, 0, 0, 0, "Shift", kPlay},

    // "Use" too: the 360's B also uses save totems (the game draws one shared
    // floating B picture over totems and stunned titans), so this one key does both.
    {"jack", "Jack / use",
     "Stun an enemy, then press to jump on and control it (\"jack\"). Press again to get off. "
     "Also uses save totems.",
     "B", Group::kTitans, Output::kButton, kB, 0, 0, "E", kPlay},
    {"titan_special", "Titan special attack",
     "The special attack of the titan you control (watch its mutant meter).", "LT",
     Group::kTitans, Output::kLeftTrigger, 0, 0, 0, "F", kPlay},
    {"pocket", "Pocket / unpocket", "Store the titan you control, and bring it back later.",
     "RB", Group::kTitans, Output::kButton, X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, "R", kPlay},

    // Map = Back: also in menus since 2026-10-30 (the pause menu's Back drops
    // a player out: players/pause_drop_out.h).
    {"map", "Map", "In the pause menu: drop out (another player stays in).", "Back", Group::kMenus,
     Output::kButton, X_INPUT_GAMEPAD_BACK, 0, 0, "Tab, M", kBoth},
    {"pause", "Pause", "Opens the pause menu (in menus, Menu: back closes it).", "Start",
     Group::kMenus, Output::kButton, X_INPUT_GAMEPAD_START, 0, 0, "Escape", kPlay},
    {"menu_confirm", "Menu: confirm", "Choose the selected item.", "A", Group::kMenus,
     Output::kButton, kA, 0, 0, "Return, Space", kMenu},
    {"menu_back", "Menu: back", "Back to the previous screen; in the pause menu: play on.", "B",
     Group::kMenus, Output::kButton, kB, 0, 0, "Escape", kMenu},
    {"menu_tab_left", "Menu: previous tab", "The Options screen's tabs.", "LB", Group::kMenus,
     Output::kButton, X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, "Q", kMenu},
    {"menu_tab_right", "Menu: next tab", "", "RB", Group::kMenus, Output::kButton,
     X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, "E", kMenu},
    {"menu_x", "Menu: X action", "What the menu's X prompt says: rename a save, Dual Mode, "
     "erase a letter on the name screen.", "X", Group::kMenus, Output::kButton, kX, 0, 0, "X",
     kMenu},
    {"menu_y", "Menu: Y action", "What the menu's Y prompt says: delete a save, remove a key "
     "(Options -> Controls).", "Y", Group::kMenus, Output::kButton, kY, 0, 0, "Backspace", kMenu},
    {"dpad_up", "D-pad up", "", "D-pad", Group::kMenus, Output::kButton, X_INPUT_GAMEPAD_DPAD_UP,
     0, 0, "Up", kBoth},
    {"dpad_down", "D-pad down", "", "D-pad", Group::kMenus, Output::kButton,
     X_INPUT_GAMEPAD_DPAD_DOWN, 0, 0, "Down", kBoth},
    {"dpad_left", "D-pad left", "", "D-pad", Group::kMenus, Output::kButton,
     X_INPUT_GAMEPAD_DPAD_LEFT, 0, 0, "Left", kBoth},
    {"dpad_right", "D-pad right", "", "D-pad", Group::kMenus, Output::kButton,
     X_INPUT_GAMEPAD_DPAD_RIGHT, 0, 0, "Right", kBoth},

    {"right_stick_up", "Right stick up", "The camera follows Crash by itself; this is for the "
     "few things that use the right stick.", "Right stick", Group::kOther, Output::kRightStick,
     0, 0, 1, "Numpad8", kPlay},
    {"right_stick_down", "Right stick down", "", "Right stick", Group::kOther,
     Output::kRightStick, 0, 0, -1, "Numpad2", kPlay},
    {"right_stick_left", "Right stick left", "", "Right stick", Group::kOther,
     Output::kRightStick, 0, -1, 0, "Numpad4", kPlay},
    {"right_stick_right", "Right stick right", "", "Right stick", Group::kOther,
     Output::kRightStick, 0, 1, 0, "Numpad6", kPlay},
    {"left_bumper", "Left bumper", "No use in play found so far.", "LB", Group::kOther,
     Output::kButton, X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, "", kPlay},
    {"left_stick_click", "Left stick click", "No use in the game found so far.", "L3",
     Group::kOther, Output::kButton, X_INPUT_GAMEPAD_LEFT_THUMB, 0, 0, "", kPlay},
    {"right_stick_click", "Right stick click", "No use in the game found so far.", "R3",
     Group::kOther, Output::kButton, X_INPUT_GAMEPAD_RIGHT_THUMB, 0, 0, "", kPlay},
};

// "LMB, J" -> slots. Unknown names logged, extra keys ignored.
void ParseKeyList(std::string_view list, std::array<Input, kSlots>& out, std::string_view what) {
  out.fill(kNoInput);
  int slot = 0;
  while (!list.empty()) {
    size_t comma = list.find(',');
    std::string_view token = Trim(list.substr(0, comma));
    list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    if (token.empty()) {
      continue;
    }
    Input input = ParseInput(token);
    if (input == kNoInput) {
      REXLOG_WARN("Controls: unknown key \"{}\" for {} (skipped)", token, what);
    } else if (IsReservedInput(input)) {
      REXLOG_WARN("Controls: {} is taken by a tool key, not bound to {}", token, what);
    } else if (slot < kSlots) {
      out[slot++] = input;
    } else {
      REXLOG_WARN("Controls: {} has more than {} keys, \"{}\" skipped", what, kSlots, token);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------

std::string InputName(Input input) {
  if (input >= V(VK::kA) && input <= V(VK::kZ)) {
    return std::string(1, char('A' + (input - V(VK::kA))));
  }
  if (input >= V(VK::k0) && input <= V(VK::k9)) {
    return std::string(1, char('0' + (input - V(VK::k0))));
  }
  if (input >= V(VK::kNumpad0) && input <= V(VK::kNumpad9)) {
    return "Numpad" + std::to_string(input - V(VK::kNumpad0));
  }
  if (input >= V(VK::kF1) && input <= V(VK::kF24)) {
    return "F" + std::to_string(input - V(VK::kF1) + 1);
  }
  for (const KeyName& k : kKeyNames) {
    if (k.input == input) {
      return k.name;
    }
  }
  return {};
}

std::string InputLabel(Input input) {
  if (input >= V(VK::kNumpad0) && input <= V(VK::kNumpad9)) {
    return "Numpad " + std::to_string(input - V(VK::kNumpad0));
  }
  for (const KeyName& k : kKeyNames) {
    if (k.input == input) {
      return k.label;
    }
  }
  std::string name = InputName(input);
  if (name.empty()) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Key 0x%02X", input);
    return buf;
  }
  return name;
}

Input ParseInput(std::string_view name) {
  name = Trim(name);
  if (name.empty()) {
    return kNoInput;
  }
  // Single letters and digits.
  if (name.size() == 1) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (c >= 'A' && c <= 'Z') return V(VK::kA) + Input(c - 'A');
    if (c >= '0' && c <= '9') return V(VK::k0) + Input(c - '0');
  }
  // F1-F24 and Numpad0-9.
  int number = 0;
  if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3 &&
      std::from_chars(name.data() + 1, name.data() + name.size(), number).ec == std::errc() &&
      number >= 1 && number <= 24) {
    return V(VK::kF1) + Input(number - 1);
  }
  if (name.size() == 7 && EqualsNoCase(name.substr(0, 6), "Numpad") && name[6] >= '0' &&
      name[6] <= '9') {
    return V(VK::kNumpad0) + Input(name[6] - '0');
  }
  for (const KeyName& k : kKeyNames) {
    if (EqualsNoCase(name, k.name)) {
      return k.input;
    }
  }
  for (const Alias& a : kAliases) {
    if (EqualsNoCase(name, a.alias)) {
      return a.input;
    }
  }
  return kNoInput;
}

bool IsMouseInput(Input input) {
  switch (input) {
    case V(VK::kLButton):
    case V(VK::kRButton):
    case V(VK::kMButton):
    case V(VK::kXButton1):
    case V(VK::kXButton2):
    case kWheelUp:
    case kWheelDown:
      return true;
    default:
      return false;
  }
}

bool IsReservedInput(Input input) {
  switch (input) {
    // Since 2026-10-29 the tools are buttons in ONE menu (ui/tools_menu.h);
    // only its key, the HUD switch and fullscreen keep a function key. The others (F2-F10,
    // F12, backtick) stay out of the game's keys too: a player may give a
    // tool its old key back in settings.toml (bind_photo = "F10"...).
    case V(VK::kF1):    // ours: the Tools menu
    case V(VK::kF2):    // ours: hide / show the HUD (was: save rename)
    case V(VK::kF3):    // (was SDK: debug overlay)
    case V(VK::kF4):    // (was SDK: settings)
    case V(VK::kF5):    // (was: the Cheats menu)
    case V(VK::kF6):    // (was: the Controls menu)
    case V(VK::kF7):    // (was SDK: achievements)
    case V(VK::kF8):    // (was: native window, dual mode)
    case V(VK::kF9):    // (was: emulated <-> native picture)
    case V(VK::kF10):   // (was: photo)
    case V(VK::kF11):   // ours: fullscreen
    case V(VK::kF12):   // (was: particle experiments)
    case V(VK::kOem3):  // (was SDK: log console, backtick)
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Actions.
// ---------------------------------------------------------------------------

const char* GroupLabel(Group group) {
  switch (group) {
    case Group::kMoving:
      return "Moving";
    case Group::kFighting:
      return "Fighting";
    case Group::kTitans:
      return "Titans";
    case Group::kMenus:
      return "Menus and map";
    case Group::kOther:
      return "Rest of the controller";
  }
  return "";
}

const ActionInfo& Info(Action action) {
  return kActions[action];
}

bool ContextsOverlap(Context a, Context b) {
  return a == Context::kBoth || b == Context::kBoth || a == b;
}

Bindings Bindings::Defaults() {
  Bindings b;
  for (int a = 0; a < kActionCount; ++a) {
    ParseKeyList(kActions[a].defaults, b.keys[a], kActions[a].id);
  }
  return b;
}

bool Bindings::Has(Action action, Input input) const {
  if (input == kNoInput) {
    return false;
  }
  const auto& slots = keys[action];
  return std::find(slots.begin(), slots.end(), input) != slots.end();
}

Action Bindings::Assign(Action action, int slot, Input input) {
  Action previous = kActionCount;
  if (input != kNoInput) {
    for (int a = 0; a < kActionCount; ++a) {
      if (!ContextsOverlap(kActions[a].context, kActions[action].context)) {
        continue;  // e.g. Escape: Pause in play, Menu: back in menus
      }
      for (Input& k : keys[a]) {
        if (k == input && !(a == action)) {
          k = kNoInput;
          previous = Action(a);
        }
      }
    }
    // Already in the other slot of the same action: move it, don't duplicate.
    for (Input& k : keys[action]) {
      if (k == input) {
        k = kNoInput;
      }
    }
  }
  keys[action][slot] = input;
  return previous;
}

// ---------------------------------------------------------------------------
// The file.
// ---------------------------------------------------------------------------

std::filesystem::path ControlsFilePath() {
  const std::string& custom = REXCVAR_GET(controls_file);
  if (!custom.empty()) {
    return custom;
  }
  return game_folder::UserFolder() / "controls.toml";  // game_folder.h
}

Bindings LoadBindings(const std::filesystem::path& path) {
  Bindings b = Bindings::Defaults();
  std::ifstream file(path);
  if (!file) {
    REXLOG_INFO("Controls: no {} yet, using the default keys", path.string());
    return b;
  }
  int lines = 0;
  int version = 1;  // files without the line: written before menus had keys of their own
  std::array<bool, kActionCount> from_file{};  // actions the file sets
  std::string line;
  while (std::getline(file, line)) {
    std::string_view view = line;
    // Strip a comment (a '#' outside quotes) and the quotes themselves.
    // (A player line's comment is the controller's name: kept.)
    std::string_view comment;
    bool quoted = false;
    for (size_t i = 0; i < view.size(); ++i) {
      if (view[i] == '"') {
        quoted = !quoted;
      } else if (view[i] == '#' && !quoted) {
        comment = Trim(view.substr(i + 1));
        view = view.substr(0, i);
        break;
      }
    }
    size_t eq = view.find('=');
    if (eq == std::string_view::npos) {
      continue;  // blank, comment or a [section] line
    }
    std::string_view key = Trim(view.substr(0, eq));
    std::string_view value = Trim(view.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    ++lines;
    if (key.substr(0, 7) == "player.") {
      // player.<device key> = 1-4 or off (players.h).
      PlayerChoice choice;
      choice.name = std::string(comment);
      int number = 0;
      if (EqualsNoCase(value, "off")) {
        choice.player = -1;
      } else if (std::from_chars(value.data(), value.data() + value.size(), number).ec ==
                     std::errc() &&
                 number >= 1 && number <= 4) {
        choice.player = number - 1;
      } else {
        REXLOG_WARN("Controls: {} = \"{}\": not 1-4 or off (skipped)", key, value);
        continue;
      }
      b.players[std::string(key.substr(7))] = std::move(choice);
      continue;
    }
    if (key.size() == 13 && key.substr(0, 12) == "icons.player" && key[12] >= '2' && key[12] <= '4') {
      // icons.player2-4 = 0-5 (Settings::pad_icons).
      int style = 0;
      if (std::from_chars(value.data(), value.data() + value.size(), style).ec != std::errc() || style < 0 ||
          style > 5) {
        REXLOG_WARN("Controls: {} = \"{}\": not 0-5 (skipped)", key, value);
        continue;
      }
      b.settings.pad_icons[size_t(key[12] - '1')] = style;
      continue;
    }
    if (key == "controls_version") {
      std::from_chars(value.data(), value.data() + value.size(), version);
      continue;
    }
    if (key == "spin_turns_per_second" || key == "walk_tilt") {
      float v = 0.0f;
      std::istringstream(std::string(value)) >> v;
      if (key == "spin_turns_per_second") {
        b.settings.spin_turns_per_second = std::clamp(v, 1.0f, 8.0f);
      } else {
        // Files from before 2026-10-15 hold 0.5 (the old default, inside the
        // game's dead zone): those get the new default.
        b.settings.walk_tilt = v < kWalkTiltMin - 0.15f ? Settings{}.walk_tilt
                                                        : std::clamp(v, kWalkTiltMin, kWalkTiltMax);
      }
      continue;
    }
    bool known = false;
    for (int a = 0; a < kActionCount; ++a) {
      if (key == kActions[a].id) {
        ParseKeyList(value, b.keys[a], kActions[a].id);
        from_file[a] = true;
        known = true;
        break;
      }
    }
    if (!known) {
      REXLOG_WARN("Controls: unknown setting \"{}\" in {} (skipped)", key, path.string());
    }
  }
  // One key = one job at a time (the menu's rule; a play action and a menu
  // action may share it). A key on two overlapping actions: a line in
  // the file beats a default it didn't mention (`jump = "Enter"` takes Enter
  // from Menu: confirm); between two file lines the later one keeps it.
  for (int a = 0; a < kActionCount; ++a) {
    for (int s = 0; s < kSlots; ++s) {
      const Input k = b.keys[a][s];
      for (int other = a + 1; other < kActionCount && k != kNoInput; ++other) {
        if (!ContextsOverlap(kActions[a].context, kActions[other].context)) continue;
        for (Input& o : b.keys[other]) {
          if (o != k) {
            continue;
          }
          const bool keep_a = from_file[a] && !from_file[other];
          const int keeper = keep_a ? a : other, loser = keep_a ? other : a;
          if (from_file[a] || from_file[other]) {
            REXLOG_WARN("Controls: {} is on {} and {}; kept for {}", InputName(k),
                        kActions[a].id, kActions[other].id, kActions[keeper].id);
          }
          for (Input& l : b.keys[loser]) {
            if (l == k) l = kNoInput;
          }
        }
      }
    }
  }
  // Version 1 files (before 2026-10-11): Menu: confirm was only Return and
  // Menu: back only Backspace, because Space and Escape belonged to Jump and
  // Pause in every context. Now menus have their own context: give them the
  // usual PC keys too, where the player hasn't put something else there.
  if (version < 2) {
    auto add = [&](Action action, Input input, bool first) {
      auto& slots = b.keys[action];
      if (std::find(slots.begin(), slots.end(), input) != slots.end()) return;
      for (int a = 0; a < kActionCount; ++a) {
        if (a != action && ContextsOverlap(kActions[a].context, kActions[action].context) &&
            b.Has(Action(a), input)) {
          return;  // the player's own choice
        }
      }
      if (slots[kSlots - 1] != kNoInput) return;  // no room
      if (first) {
        for (int s = kSlots - 1; s > 0; --s) slots[s] = slots[s - 1];
        slots[0] = input;
      } else {
        *std::find(slots.begin(), slots.end(), kNoInput) = input;
      }
      REXLOG_INFO("Controls: {} also on {} (menus have keys of their own now)",
                  kActions[action].id, InputName(input));
    };
    add(kMenuConfirm, V(VK::kSpace), false);
    add(kMenuBack, V(VK::kEscape), true);
  }
  // Version 2 files (before 2026-10-15): Backspace was a second Menu: back
  // and the Y key was Menu: Y. Backspace is the menus' "remove / delete" now
  // (Options -> Controls: remove a key; the save list: delete a save), as a
  // keyboard player expects. Only where the file still has the old layout:
  // Backspace leaves Menu: back if Escape (or another key) stays there, and
  // Menu: Y goes from Y to Backspace if it was only Y.
  if (version < 3) {
    const Input backspace = V(VK::kBack), y = V(VK::kY);
    auto& back = b.keys[kMenuBack];
    const bool back_kept = std::any_of(back.begin(), back.end(),
                                       [&](Input k) { return k != kNoInput && k != backspace; });
    if (back_kept && b.Has(kMenuBack, backspace)) {
      for (Input& k : back) {
        if (k == backspace) k = kNoInput;
      }
      if (back[0] == kNoInput) std::swap(back[0], back[1]);
      REXLOG_INFO("Controls: Backspace isn't Menu: back any more (it removes / deletes in menus)");
    }
    // (Empty too: a file without a menu_y line got the new default
    // Backspace, which its menu_back line took back while loading.)
    auto& menu_y = b.keys[kMenuY];
    if ((menu_y[0] == y || menu_y[0] == kNoInput) && menu_y[1] == kNoInput &&
        !b.Has(kMenuBack, backspace)) {
      bool free = true;
      for (int a = 0; a < kActionCount; ++a) {
        if (a != kMenuY && ContextsOverlap(kActions[a].context, Context::kMenu) && b.Has(Action(a), backspace)) {
          free = false;
        }
      }
      if (free) {
        menu_y[0] = backspace;
        REXLOG_INFO("Controls: Menu: Y is Backspace now (was Y)");
      }
    }
  }
  REXLOG_INFO("Controls: loaded {} ({} settings)", path.string(), lines);
  // What differs from the defaults, so a play log says which keys were used.
  const Bindings defaults = Bindings::Defaults();
  std::string changed;
  for (int a = 0; a < kActionCount; ++a) {
    if (b.keys[a] != defaults.keys[a]) {
      std::string list;
      for (Input k : b.keys[a]) {
        if (k != kNoInput) list += (list.empty() ? "" : "+") + InputName(k);
      }
      changed += std::string(changed.empty() ? "" : ", ") + kActions[a].id + "=" +
                 (list.empty() ? "none" : list);
    }
  }
  REXLOG_INFO("Controls: changed from the defaults: {} (spin {:.2f} circles/s, walk {:.2f})",
              changed.empty() ? "nothing" : changed, b.settings.spin_turns_per_second,
              b.settings.walk_tilt);
  return b;
}

bool SaveBindings(const Bindings& b, const std::filesystem::path& path) {
  std::ostringstream out;
  out << "# Crash: Mind over Mutant (Mind over Recomp): keyboard and mouse controls.\n"
         "# Written by the Controls menu (F1 Tools) on every change. Hand edits work too, with\n"
         "# the game closed: up to " << kSlots << " keys per action, separated by commas.\n"
         "# Key names: A-Z, 0-9, F1-F24, Space, Return, Escape, Tab, Backspace, Shift,\n"
         "# Control, Alt, Up/Down/Left/Right, Numpad0-9, LMB, RMB, MMB, Mouse4, Mouse5,\n"
         "# WheelUp, WheelDown (more: src/input/bindings.cpp). F1-F12 and the\n"
         "# backtick key belong to the tools and can't be bound.\n"
         "# The comment after each line is the Xbox 360 control the game sees.\n"
         "# Actions marked [menus] work while a menu is on screen, [play] in play; a key\n"
         "# may have one of each (Escape: Pause in play, Menu: back in menus).\n\n"
         "controls_version = 3\n";
  char number[32];
  std::snprintf(number, sizeof(number), "%.2f", b.settings.spin_turns_per_second);
  out << "spin_turns_per_second = " << number << "   # Spin held: spins per second\n";
  std::snprintf(number, sizeof(number), "%.2f", b.settings.walk_tilt);
  out << "walk_tilt = " << number << "   # Walk: how far the stick leans (1 = all the way)\n";
  Group group = Group::kOther;
  for (int a = 0; a < kActionCount; ++a) {
    const ActionInfo& info = kActions[a];
    if (a == 0 || info.group != group) {
      group = info.group;
      out << "\n# " << GroupLabel(group) << "\n";
    }
    std::string list;
    for (Input k : b.keys[a]) {
      if (k != kNoInput) {
        list += (list.empty() ? "" : ", ") + InputName(k);
      }
    }
    std::string entry = std::string(info.id) + " = \"" + list + "\"";
    out << entry << std::string(entry.size() < 34 ? 34 - entry.size() : 1, ' ') << "# "
        << info.pad
        << (info.context == Context::kMenu   ? " [menus]"
            : info.context == Context::kPlay ? " [play]"
                                             : "")
        << "\n";
  }

  // Players (players.h): only the devices given a player by hand.
  out << "\n# Players: which device plays as which player (Players tab of the Controls menu).\n"
         "# player.<device> = 1-4 or off; a device not listed is automatic (the keyboard\n"
         "# and the first controller = player 1, the second controller = player 2...).\n";
  for (const auto& [key, choice] : b.players) {
    if (choice.player == -2) {
      continue;  // automatic: nothing to write
    }
    std::string entry = "player." + key + " = " +
                        (choice.player < 0 ? std::string("off") : std::to_string(choice.player + 1));
    out << entry << "   # " << (choice.name.empty() ? std::string("?") : choice.name) << "\n";
  }

  // Players 2-4's button pictures (Settings::pad_icons): only the chosen ones.
  {
    static constexpr const char* kStyles[6] = {"Automatic", "Xbox 360", "Xbox Series", "PlayStation 5",
                                               "PlayStation 4", "Switch"};
    bool header = false;
    for (int p = 1; p < 4; ++p) {
      const int style = b.settings.pad_icons[size_t(p)];
      if (style <= 0 || style > 5) continue;
      if (!header) {
        out << "\n# Players 2-4's button pictures (Players screen; player 1's = Options ->\n"
               "# Controller Icons): 1 Xbox 360, 2 Xbox Series, 3 PlayStation 5, 4 PlayStation 4,\n"
               "# 5 Switch; not listed = automatic (by the controller's maker).\n";
        header = true;
      }
      out << "icons.player" << (p + 1) << " = " << style << "   # " << kStyles[style] << "\n";
    }
  }

  // Write a temporary file, then rename it over the old one: a crash halfway
  // never leaves a cut-off controls file behind.
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::trunc);
    if (!file || !(file << out.str()) || !file.flush()) {
      REXLOG_ERROR("Controls: can't write {}", temp.string());
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    REXLOG_ERROR("Controls: can't replace {}: {}", path.string(), ec.message());
    return false;
  }
  REXLOG_INFO("Controls: saved {}", path.string());
  return true;
}

}  // namespace kbm
