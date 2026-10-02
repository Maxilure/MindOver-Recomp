// =============================================================================
// input/keyboard_mouse.cpp -- see keyboard_mouse.h
// =============================================================================
#include "keyboard_mouse.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(keyboard_mouse, true, "CrashMoM",
                    "Play with keyboard and mouse (keys in controls.toml next to the exe, F6 = "
                    "Controls menu); a controller keeps working too");
REXCVAR_DEFINE_BOOL(debug_kbm_trace, false, "CrashMoM",
                    "Debug: log every change of the controller state the keyboard / mouse "
                    "produce (buttons, triggers, sticks)");

using namespace rex;
using namespace rex::input;

namespace kbm {
namespace {

constexpr DeviceId kDevice = KeyboardMouseDriver::kDeviceId;

// Above ImGui's 64 (rex_app.cpp SetupOverlays) and the app's 0: we see every
// key first, which a capture needs (it must swallow the key before ImGui or
// the F-key binds see it). Outside a capture nothing is swallowed.
constexpr size_t kZOrder = 128;

// Mouse wheel notch = this long a press, then this long a gap.
constexpr auto kWheelPress = std::chrono::milliseconds(60);
constexpr auto kWheelGap = std::chrono::milliseconds(40);
constexpr int kWheelMaxQueued = 4;

constexpr float kTwoPi = 6.2831853f;
// Spin: how far past the last whole circle the stick goes before letting go.
constexpr float kSpinOvershootTurns = 0.15f;

std::atomic<KeyboardMouseDriver*> g_driver{nullptr};

int16_t ToStickAxis(float v) {
  return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

// ---------------------------------------------------------------------------
// Creation, windows.
// ---------------------------------------------------------------------------

std::unique_ptr<KeyboardMouseDriver> KeyboardMouseDriver::Create() {
  if (!REXCVAR_GET(keyboard_mouse)) {
    REXLOG_INFO("Controls: keyboard and mouse off (--keyboard_mouse=false)");
    return nullptr;
  }
  std::unique_ptr<KeyboardMouseDriver> driver(
      new KeyboardMouseDriver(LoadBindings(ControlsFilePath())));
  g_driver.store(driver.get());
  return driver;
}

KeyboardMouseDriver* KeyboardMouseDriver::Get() {
  return g_driver.load();
}

// No window yet (Attach), z-order unused by the base class here.
KeyboardMouseDriver::KeyboardMouseDriver(Bindings bindings)
    : InputDriver(nullptr, kZOrder), bindings_(std::move(bindings)) {}

KeyboardMouseDriver::~KeyboardMouseDriver() {
  g_driver.store(nullptr);
  // Normally empty here: every window detaches us when it closes (OnClosing),
  // and the SDK destroys the main window before the input system.
  for (rex::ui::Window* window : std::vector<rex::ui::Window*>(windows_)) {
    Detach(window);
  }
}

void KeyboardMouseDriver::Attach(rex::ui::Window* window) {
  if (!window || std::find(windows_.begin(), windows_.end(), window) != windows_.end()) {
    return;
  }
  windows_.push_back(window);
  window->AddInputListener(this, kZOrder);
  window->AddListener(this);
  // The pointer hides after a few seconds without mouse movement (the SDK's
  // auto-hide), so it doesn't sit over the picture while the mouse buttons
  // attack. Moving the mouse shows it again.
  if (!menu_open_.load()) {
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void KeyboardMouseDriver::Detach(rex::ui::Window* window) {
  auto it = std::find(windows_.begin(), windows_.end(), window);
  if (it == windows_.end()) {
    return;
  }
  windows_.erase(it);
  window->RemoveInputListener(this);
  window->RemoveListener(this);
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
}

void KeyboardMouseDriver::SetPausedCheck(std::function<uint32_t()> paused) {
  std::lock_guard<std::mutex> lock(mutex_);
  paused_check_ = std::move(paused);
}

void KeyboardMouseDriver::SetMenuOpen(bool open) {
  if (menu_open_.exchange(open) == open) {
    return;
  }
  // Keys held when the menu opens would otherwise stay "held" to the game
  // after it closes (their key-up went to the menu).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseAll();
  }
  for (rex::ui::Window* window : windows_) {
    window->SetCursorVisibility(open ? rex::ui::Window::CursorVisibility::kVisible
                                     : rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

Bindings KeyboardMouseDriver::bindings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bindings_;
}

void KeyboardMouseDriver::SetBindings(const Bindings& bindings) {
  std::lock_guard<std::mutex> lock(mutex_);
  bindings_ = bindings;
}

// ---------------------------------------------------------------------------
// Capture (the Controls menu's "press a key").
// ---------------------------------------------------------------------------

void KeyboardMouseDriver::Capture(std::function<void(Input)> done) {
  capture_done_ = std::move(done);
}

void KeyboardMouseDriver::CancelCapture() {
  capture_done_ = nullptr;
}

bool KeyboardMouseDriver::capturing() const {
  return static_cast<bool>(capture_done_);
}

bool KeyboardMouseDriver::HandleDown(Input input) {
  if (!capture_done_ || input == kNoInput || IsReservedInput(input)) {
    return false;  // not capturing, or a tool key: let it do its job
  }
  // Move the callback out first: it may start the next capture.
  auto done = std::move(capture_done_);
  capture_done_ = nullptr;
  done(input == static_cast<Input>(rex::ui::VirtualKey::kEscape) ? kNoInput : input);
  return true;
}

// ---------------------------------------------------------------------------
// Events.
// ---------------------------------------------------------------------------

void KeyboardMouseDriver::Press(Input input, bool down) {
  if (input == kNoInput || input >= kInputCount) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (down && !held_[input]) {
    pressed_at_[input] = ++press_counter_;  // key repeats don't count
  }
  held_[input] = down;
}

void KeyboardMouseDriver::ReleaseAll() {
  held_.fill(false);
  debug_release_.fill(Clock::time_point{});
  for (WheelPulse& w : wheel_) {
    w = WheelPulse{};
  }
}

Input KeyboardMouseDriver::MouseInput(const rex::ui::MouseEvent& e) {
  using Button = rex::ui::MouseEvent::Button;
  using VK = rex::ui::VirtualKey;
  switch (e.button()) {
    case Button::kLeft:
      return static_cast<Input>(VK::kLButton);
    case Button::kRight:
      return static_cast<Input>(VK::kRButton);
    case Button::kMiddle:
      return static_cast<Input>(VK::kMButton);
    case Button::kX1:
      return static_cast<Input>(VK::kXButton1);
    case Button::kX2:
      return static_cast<Input>(VK::kXButton2);
    default:
      return kNoInput;
  }
}

void KeyboardMouseDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  const Input input = static_cast<Input>(e.virtual_key());
  if (input >= 0x100) {
    return;  // not a keyboard key we know (Xbox pad "keys" live up there)
  }
  if (HandleDown(input)) {
    e.set_handled(true);
    return;
  }
  Press(input, true);
}

void KeyboardMouseDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  const Input input = static_cast<Input>(e.virtual_key());
  if (input < 0x100) {
    Press(input, false);
  }
}

void KeyboardMouseDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  const Input input = MouseInput(e);
  if (HandleDown(input)) {
    // Its button-up must not reach ImGui either (it would click whatever is
    // under the pointer).
    swallow_mouse_up_ = input;
    e.set_handled(true);
    return;
  }
  Press(input, true);
}

void KeyboardMouseDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  const Input input = MouseInput(e);
  if (input != kNoInput && input == swallow_mouse_up_) {
    swallow_mouse_up_ = kNoInput;
    e.set_handled(true);
  }
  Press(input, false);
}

void KeyboardMouseDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (e.scroll_y() == 0) {
    return;
  }
  // SDL scroll: positive = away from the user ("up"). One notch = 120.
  const bool up = e.scroll_y() > 0;
  if (HandleDown(up ? kWheelUp : kWheelDown)) {
    e.set_handled(true);
    return;
  }
  const int notches =
      std::max(1, std::abs(e.scroll_y()) / int(rex::ui::MouseEvent::kScrollPerDetent));
  std::lock_guard<std::mutex> lock(mutex_);
  WheelPulse& w = wheel_[up ? 0 : 1];
  w.queued = std::min(kWheelMaxQueued, w.queued + notches);
}

void KeyboardMouseDriver::OnLostFocus(rex::ui::UISetupEvent& e) {
  (void)e;
  // Keys held while the focus left would never get their key-up.
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAll();
}

void KeyboardMouseDriver::OnClosing(rex::ui::UIEvent& e) {
  // A closing window must forget us (the main window dies before the input
  // system at shutdown; the dual-mode window is re-attached when reopened).
  Detach(e.target());
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAll();
}

bool KeyboardMouseDriver::DebugTap(std::string_view key_name, int64_t hold_ms) {
  KeyboardMouseDriver* driver = Get();
  const Input input = ParseInput(key_name);
  if (!driver || input == kNoInput) {
    return false;
  }
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(driver->mutex_);
  if (input == kWheelUp || input == kWheelDown) {
    WheelPulse& w = driver->wheel_[input == kWheelUp ? 0 : 1];
    w.queued = std::min(kWheelMaxQueued, w.queued + 1);
    return true;
  }
  if (!driver->held_[input]) {
    driver->pressed_at_[input] = ++driver->press_counter_;
  }
  driver->held_[input] = true;
  driver->debug_release_[input] = now + std::chrono::milliseconds(hold_ms);
  return true;
}

// ---------------------------------------------------------------------------
// The pad the game sees.
// ---------------------------------------------------------------------------

bool KeyboardMouseDriver::ActionHeld(Action action) const {
  return ActionPressOrder(action) != 0;
}

uint32_t KeyboardMouseDriver::ActionPressOrder(Action action) const {
  uint32_t order = 0;
  for (Input k : bindings_.keys[action]) {
    if (k != kNoInput && held_[k] && !(mouse_blocked_ && IsMouseInput(k))) {
      order = std::max(order, pressed_at_[k]);
    }
  }
  return order;
}

float KeyboardMouseDriver::Axis(Action negative, Action positive) const {
  const uint32_t n = ActionPressOrder(negative), p = ActionPressOrder(positive);
  if (!n && !p) return 0.0f;
  return p > n ? 1.0f : -1.0f;  // both held: the later press wins
}

void KeyboardMouseDriver::BuildState(Clock::time_point now, X_INPUT_GAMEPAD& pad) {
  std::memset(&pad, 0, sizeof(pad));

  // Wheel pulses become held / released "keys" for this poll.
  for (int d = 0; d < 2; ++d) {
    WheelPulse& w = wheel_[d];
    if (now >= w.gap_until && w.queued > 0) {
      --w.queued;
      w.down_until = now + kWheelPress;
      w.gap_until = w.down_until + kWheelGap;
    }
    const Input input = d == 0 ? kWheelUp : kWheelDown;
    const bool down = now < w.down_until;
    if (down && !held_[input]) {
      pressed_at_[input] = ++press_counter_;
    }
    held_[input] = down;
  }

  // Buttons and triggers. (X_INPUT_GAMEPAD is the guest's big-endian layout:
  // be<> fields convert on assignment, so the bits are gathered here first.)
  uint16_t buttons = 0;
  for (int a = 0; a < kActionCount; ++a) {
    const ActionInfo& info = Info(Action(a));
    if (!ActionHeld(Action(a))) {
      continue;
    }
    switch (info.output) {
      case Output::kButton:
        buttons |= info.button;
        break;
      case Output::kLeftTrigger:
        pad.left_trigger = 0xFF;
        break;
      case Output::kRightTrigger:
        pad.right_trigger = 0xFF;
        break;
      default:
        break;
    }
  }
  pad.buttons = buttons;

  // Left stick: movement keys, then Walk, then (overriding both) Spin.
  float lx = Axis(kMoveLeft, kMoveRight), ly = Axis(kMoveDown, kMoveUp);
  if (lx != 0.0f && ly != 0.0f) {
    lx *= 0.70710678f;  // a diagonal reaches the stick's circle, not a corner
    ly *= 0.70710678f;
  }
  if (lx != 0.0f || ly != 0.0f) {
    last_move_angle_ = std::atan2(ly, lx);
    if (ActionHeld(kWalk)) {
      lx *= bindings_.settings.walk_tilt;
      ly *= bindings_.settings.walk_tilt;
    }
  }
  const bool spin_held = ActionHeld(kSpin);
  if (spin_held && !spin_.active) {
    spin_.active = true;
    spin_.start = now;
    spin_.start_angle = last_move_angle_;
    spin_.end_turns = 0;
  }
  if (spin_.active) {
    const float turns = std::chrono::duration<float>(now - spin_.start).count() *
                        bindings_.settings.spin_turns_per_second;
    if (spin_held) {
      spin_.end_turns = 0;  // (re-)held: keep turning
    } else if (spin_.end_turns == 0) {
      spin_.end_turns = std::max(1, static_cast<int>(std::ceil(turns)));  // finish this circle
    }
    // A little past the last whole circle (like a thumb overshooting), so the
    // stick is back where the circle began when it lets go.
    if (spin_.end_turns > 0 && turns >= float(spin_.end_turns) + kSpinOvershootTurns) {
      spin_.active = false;
    } else {
      // Clockwise (on screen: up, right, down, left), whole stick.
      const float angle = spin_.start_angle - kTwoPi * (turns - std::floor(turns));
      lx = std::cos(angle);
      ly = std::sin(angle);
    }
  }
  pad.thumb_lx = ToStickAxis(lx);
  pad.thumb_ly = ToStickAxis(ly);

  // Right stick.
  float rx = Axis(kRightStickLeft, kRightStickRight), ry = Axis(kRightStickDown, kRightStickUp);
  if (rx != 0.0f && ry != 0.0f) {
    rx *= 0.70710678f;
    ry *= 0.70710678f;
  }
  pad.thumb_rx = ToStickAxis(rx);
  pad.thumb_ry = ToStickAxis(ry);
}

X_STATUS KeyboardMouseDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void KeyboardMouseDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kDevice;
  info.name = "Keyboard and mouse";
  info.synthetic = true;  // players.h: player 1 unless chosen otherwise
  out.push_back(info);
}

X_RESULT KeyboardMouseDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  // Debug taps end on their own.
  bool debug_tap = false;
  for (size_t i = 0; i < kInputCount; ++i) {
    if (debug_release_[i] != Clock::time_point{}) {
      if (now >= debug_release_[i]) {
        debug_release_[i] = Clock::time_point{};
        held_[i] = false;
      } else {
        debug_tap = true;
      }
    }
  }

  // Paused: the game gets a neutral pad (debug taps bypass the focus check:
  // test runs may not have the focus).
  // (Not the SDK's is_active(): once dual mode opens, the app sets that for
  // every driver to "focused and ImGui doesn't want the mouse", which would
  // freeze the keys too. paused_check_ covers the focus of both windows.)
  const uint32_t pause = paused_check_ ? paused_check_() : 0;
  const bool paused = !debug_tap && (menu_open_.load() || (pause & kPauseAll));
  mouse_blocked_ = !debug_tap && (pause & kPauseMouse);
  X_INPUT_GAMEPAD pad{};
  if (paused || mouse_blocked_) {
    // Wheel notches that scrolled an overlay (F4's lists) aren't for the game.
    for (WheelPulse& w : wheel_) {
      w = WheelPulse{};
    }
    held_[kWheelUp] = held_[kWheelDown] = false;
  }
  if (paused) {
    spin_.active = false;
  } else {
    BuildState(now, pad);
  }

  if (std::memcmp(&pad, &last_pad_, sizeof(pad)) != 0) {
    ++packet_number_;  // XInput games compare it to spot a new state
    if (REXCVAR_GET(debug_kbm_trace)) {
      REXLOG_INFO("KBM: buttons {:04X} LT {} RT {} left ({}, {}) right ({}, {})",
                  uint16_t(pad.buttons), int(pad.left_trigger), int(pad.right_trigger),
                  int(pad.thumb_lx), int(pad.thumb_ly), int(pad.thumb_rx), int(pad.thumb_ry));
    }
    last_pad_ = pad;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = packet_number_;
    out_state->gamepad = pad;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardMouseDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                    X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (id != kDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    // A standard gamepad (type 1 = gamepad, sub_type 1 = standard
    // controller) with every button and axis, like the SDK's keyboard driver.
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardMouseDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;  // nothing to shake
  return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT KeyboardMouseDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                                 X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  // Keystrokes (text entry on the 360) aren't used by this game.
  return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace kbm
