// =============================================================================
// input/keyboard_mouse.cpp -- see keyboard_mouse.h
// =============================================================================
#include "keyboard_mouse.h"

#include "../options/options_menu.h"

#include "players.h"
#include "../players/more_players.h"
#include "../gameplay/game_mods.h"
#include "../guest_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(keyboard_mouse, true, "CrashMoM",
                    "Play with keyboard and mouse (keys in user/controls.toml in the game folder, F6 = "
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

// Spin: a "full circle" not asked for within this long is dropped (the
// fight trees ask while a move can start; a stale one would start a spin
// seconds after the key, e.g. after a landing).
constexpr auto kSpinPulseLife = std::chrono::milliseconds(400);

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
  app_context_ = &window->app_context();
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
  ApplyPointerVisibility();
}

void KeyboardMouseDriver::HidePointerWhileAiming() {
  const auto until = Clock::now() + std::chrono::milliseconds(200);
  aim_hidden_until_.store(until.time_since_epoch().count());
  if (!aim_hidden_.exchange(true)) ApplyPointerVisibility();
}

void KeyboardMouseDriver::ApplyPointerVisibility() {
  if (!app_context_) return;
  app_context_->CallInUIThreadDeferred([this] {
    using Visibility = rex::ui::Window::CursorVisibility;
    const Visibility v = menu_open_.load()    ? Visibility::kVisible
                         : aim_hidden_.load() ? Visibility::kHidden
                                              : Visibility::kAutoHidden;
    for (rex::ui::Window* window : windows_) window->SetCursorVisibility(v);
  });
}

Bindings KeyboardMouseDriver::bindings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bindings_;
}

void KeyboardMouseDriver::SetBindings(const Bindings& bindings) {
  std::lock_guard<std::mutex> lock(mutex_);
  bindings_ = bindings;
  ++bindings_revision_;
}

rex::input::X_INPUT_GAMEPAD KeyboardMouseDriver::last_pad() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_pad_;
}

// ---------------------------------------------------------------------------
// Capture (the Controls menu's "press a key").
// ---------------------------------------------------------------------------

void KeyboardMouseDriver::Capture(std::function<void(Input)> done) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_done_ = std::move(done);
}

void KeyboardMouseDriver::CancelCapture() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_done_ = nullptr;
}

bool KeyboardMouseDriver::capturing() const {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  return static_cast<bool>(capture_done_);
}

bool KeyboardMouseDriver::InputHeld(Input input) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return input < kInputCount && held_[input];
}

bool KeyboardMouseDriver::HandleDown(Input input) {
  if (input == kNoInput || input >= kInputCount || IsReservedInput(input)) {
    return false;  // a tool key: let it do its job
  }
  {
    // A key already held = its key repeat (the OS repeats a held key's
    // "down"): the Enter that opened the in-game capture would be caught at
    // once otherwise. The wheel has no "held".
    std::lock_guard<std::mutex> lock(mutex_);
    if (held_[input]) return false;
  }
  std::function<void(Input)> done;
  {
    // Move the callback out first: it may start the next capture.
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (!capture_done_) return false;  // not capturing
    done = std::move(capture_done_);
    capture_done_ = nullptr;
  }
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
    in_menu_[input] = MenuOnScreen();      // its job until it's released
    if (in_menu_[input]) MenuMouseDown(input, Clock::now());
  }
  if (down) {
    last_key_down_ = Clock::now().time_since_epoch().count();
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

void KeyboardMouseDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  if (!text_mode_.load()) return;
  const char32_t c = char32_t(uint32_t(e.virtual_key()));
  if (c < 0x20 || c == 0x7F) return;  // control characters: the keys' own jobs
  std::lock_guard<std::mutex> lock(typed_mutex_);
  if (typed_.size() < 64) typed_.push_back(c);
  e.set_handled(true);
}

void KeyboardMouseDriver::SetTextMode(bool on) {
  if (text_mode_.exchange(on) == on) return;
  {
    std::lock_guard<std::mutex> lock(typed_mutex_);
    typed_.clear();
  }
  // The windows' text input (SDL_StartTextInput / Stop), on the UI thread.
  if (app_context_) {
    app_context_->CallInUIThread([this, on] {
      for (rex::ui::Window* w : windows_) w->SetTextInputActive(on);
    });
  }
  REXLOG_INFO("Keyboard: {}", on ? "typing (a name screen)" : "playing again");
}

std::u32string KeyboardMouseDriver::TakeTyped() {
  std::lock_guard<std::mutex> lock(typed_mutex_);
  std::u32string out;
  out.swap(typed_);
  return out;
}

bool KeyboardMouseDriver::DebugType(std::string_view text) {
  KeyboardMouseDriver* driver = Get();
  if (!driver || !driver->text_mode_.load()) return false;
  std::lock_guard<std::mutex> lock(driver->typed_mutex_);
  for (const char c : text) driver->typed_.push_back(char32_t(uint8_t(c)));
  return true;
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
  last_key_down_ = Clock::now().time_since_epoch().count();
  if (MenuOnScreen()) {
    // In a menu a notch is one step up / down (mouse_menus.h).
    for (int i = 0; i < notches; ++i) MenuMouseDown(up ? kWheelUp : kWheelDown, Clock::now());
    return;
  }
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
  // A waiting capture takes the tap, as it would a typed key (tests of the
  // Keys tab drive it this way).
  if (driver->HandleDown(input)) return true;
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(driver->mutex_);
  driver->last_key_down_ = now.time_since_epoch().count();  // a tap counts as typed
  if ((input == kWheelUp || input == kWheelDown) && driver->MenuOnScreen()) {
    driver->MenuMouseDown(input, now);
    // Counts as a held debug tap for a moment, so its pulse gets past the
    // focus pause like other taps (test windows aren't focused).
    driver->debug_release_[input] = now + std::chrono::milliseconds(300);
    return true;
  }
  if (input == kWheelUp || input == kWheelDown) {
    WheelPulse& w = driver->wheel_[input == kWheelUp ? 0 : 1];
    w.queued = std::min(kWheelMaxQueued, w.queued + 1);
    return true;
  }
  if (!driver->held_[input]) {
    driver->pressed_at_[input] = ++driver->press_counter_;
    driver->in_menu_[input] = driver->MenuOnScreen();
    if (driver->in_menu_[input]) driver->MenuMouseDown(input, now);
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
  const Context context = Info(action).context;
  for (Input k : bindings_.keys[action]) {
    // A key works for the context it was pressed in (bindings.h, "PLAY AND
    // MENUS"): Escape that opened the pause menu doesn't also close it.
    if (context != Context::kBoth && in_menu_[k] != (context == Context::kMenu)) {
      continue;
    }
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
      in_menu_[input] = MenuOnScreen();
    }
    held_[input] = down;
  }

  // Text mode: the keys type (SetTextMode); only these are buttons.
  if (text_mode_.load()) {
    using VK = rex::ui::VirtualKey;
    auto down = [&](VK k) { return held_[Input(k)]; };
    uint16_t typing = 0;
    if (down(VK::kUp)) typing |= X_INPUT_GAMEPAD_DPAD_UP;
    if (down(VK::kDown)) typing |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (down(VK::kLeft)) typing |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (down(VK::kRight)) typing |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (down(VK::kReturn)) typing |= X_INPUT_GAMEPAD_START;
    if (down(VK::kEscape)) typing |= X_INPUT_GAMEPAD_B;
    // (Backspace erases through input/name_typing.cpp, with key repeat.)
    pad.buttons = typing;
    spin_held_ = false;
    return;
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

  // Left stick: movement keys, then Walk. (Spin no longer touches the
  // stick: it's answered as "a full circle done", below.)
  float lx = Axis(kMoveLeft, kMoveRight), ly = Axis(kMoveDown, kMoveUp);
  if (lx != 0.0f && ly != 0.0f) {
    lx *= 0.70710678f;  // a diagonal reaches the stick's circle, not a corner
    ly *= 0.70710678f;
  }
  if ((lx != 0.0f || ly != 0.0f) && ActionHeld(kWalk)) {
    lx *= bindings_.settings.walk_tilt;
    ly *= bindings_.settings.walk_tilt;
  }
  // Spin: one circle when pressed, then one every 1 / spin_turns_per_second
  // while held (TakeSpinCircle hands them to the fight trees).
  const bool spin_held = ActionHeld(kSpin);
  if (spin_held && !spin_held_) {
    spin_circles_.push_back(now);
    next_spin_ = now + SpinPeriod();
  } else if (spin_held && now >= next_spin_) {
    spin_circles_.push_back(now);
    next_spin_ += SpinPeriod();
    if (next_spin_ < now) next_spin_ = now + SpinPeriod();  // after a long frame
  }
  spin_held_ = spin_held;
  while (!spin_circles_.empty() && now - spin_circles_.front() > kSpinPulseLife) {
    spin_circles_.pop_front();
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
  // Connected, but silent: the SDK only learns that a keyboard plays (so its
  // player counts as having a device: players.h, who_plays, the
  // lost-controller check). The keys reach the game by MergeInto.
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
  }
  return X_ERROR_SUCCESS;
}

void KeyboardMouseDriver::MergeInto(uint8_t* gamepad) {
  const auto now = Clock::now();
  // Aiming stopped (no HidePointerWhileAiming for a while): the pointer back.
  if (aim_hidden_.load() && now.time_since_epoch().count() > aim_hidden_until_.load() &&
      aim_hidden_.exchange(false)) {
    ApplyPointerVisibility();
  }
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

  // Paused: nothing from the keys (debug taps bypass the focus check: test
  // runs may not have the focus). paused_check_ covers the focus of both
  // windows and ImGui wanting the keyboard / mouse.
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
    spin_held_ = false;
    spin_circles_.clear();
  } else {
    BuildState(now, pad);
  }
  // Mouse pulses (a click's A, a right click's B, a wheel notch's D-pad):
  // each held 60 ms, then a 40 ms gap before the next: menus read presses as
  // changes from one frame to the next, and both last at least a frame even
  // at 30 fps (33 ms). 100 ms a pulse = ten clicks a second (80 + 50 lost one
  // click in four at that rate).
  if (now >= pulse_until_ && pulse_now_) {
    pulse_now_ = 0;
    pulse_gap_until_ = now + std::chrono::milliseconds(40);
  }
  if (!pulse_now_ && now >= pulse_gap_until_ && !pulses_.empty()) {
    pulse_now_ = pulses_.front();
    pulses_.pop_front();
    pulse_until_ = now + std::chrono::milliseconds(60);
  }
  if (pulse_now_ && !paused) pad.buttons = uint16_t(uint16_t(pad.buttons) | pulse_now_);

  if (std::memcmp(&pad, &last_pad_, sizeof(pad)) != 0) {
    if (REXCVAR_GET(debug_kbm_trace)) {
      REXLOG_INFO("KBM: buttons {:04X} LT {} RT {} left ({}, {}) right ({}, {})",
                  uint16_t(pad.buttons), int(pad.left_trigger), int(pad.right_trigger),
                  int(pad.thumb_lx), int(pad.thumb_ly), int(pad.thumb_rx), int(pad.thumb_ry));
    }
    last_pad_ = pad;
  }

  // Into the game's state (guest X_INPUT_GAMEPAD, big-endian), like devices
  // on one player are merged: buttons together, the bigger trigger, and a
  // stick the keys push replaces the controller's.
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]) | uint16_t(pad.buttons);
  gamepad[0] = uint8_t(buttons >> 8);
  gamepad[1] = uint8_t(buttons);
  gamepad[2] = std::max(gamepad[2], pad.left_trigger);
  gamepad[3] = std::max(gamepad[3], pad.right_trigger);
  auto put_stick = [&](int at, int16_t x, int16_t y) {
    if (x == 0 && y == 0) return;
    gamepad[at + 0] = uint8_t(uint16_t(x) >> 8);
    gamepad[at + 1] = uint8_t(uint16_t(x));
    gamepad[at + 2] = uint8_t(uint16_t(y) >> 8);
    gamepad[at + 3] = uint8_t(uint16_t(y));
  };
  put_stick(4, pad.thumb_lx, pad.thumb_ly);
  put_stick(8, pad.thumb_rx, pad.thumb_ry);
}

void KeyboardMouseDriver::MenuMouseDown(Input input, Clock::time_point now) {
  using namespace rex::input;
  switch (input) {
    case Input(rex::ui::VirtualKey::kLButton):
      menu_click_ = now;  // the menu code decides (TakeMenuClick)
      menu_click_x_ = pointer_x_;
      menu_click_y_ = pointer_y_;
      break;
    case Input(rex::ui::VirtualKey::kRButton):
      if (pulses_.size() < 4) pulses_.push_back(X_INPUT_GAMEPAD_B);
      break;
    // A list that scrolls by itself (the Options screen's long tabs) takes
    // the notch: the list moves, the selection doesn't (playtest, 2026-10-15:
    // a notch as a D-pad press moved the cursor first). Any other menu: a
    // step up / down.
    case kWheelUp:
      if (options_menu::WheelScrollsList()) {
        list_wheel_ = std::max(-8, list_wheel_ - 1);
      } else if (pulses_.size() < 4) {
        pulses_.push_back(X_INPUT_GAMEPAD_DPAD_UP);
      }
      break;
    case kWheelDown:
      if (options_menu::WheelScrollsList()) {
        list_wheel_ = std::min(8, list_wheel_ + 1);
      } else if (pulses_.size() < 4) {
        pulses_.push_back(X_INPUT_GAMEPAD_DPAD_DOWN);
      }
      break;
    default:
      break;
  }
}

bool KeyboardMouseDriver::TakeMenuClick() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (menu_click_ == Clock::time_point{}) return false;
  const bool fresh = Clock::now() - menu_click_ < std::chrono::milliseconds(200);
  menu_click_ = Clock::time_point{};
  return fresh;
}

bool KeyboardMouseDriver::PeekMenuClick(float* x, float* y) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (x) *x = menu_click_x_;
  if (y) *y = menu_click_y_;
  return menu_click_ != Clock::time_point{} &&
         Clock::now() - menu_click_ < std::chrono::milliseconds(200);
}

void KeyboardMouseDriver::Pulse(uint16_t buttons) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pulses_.size() < 4) pulses_.push_back(buttons);
}

bool KeyboardMouseDriver::PulseBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pulse_now_ != 0 || !pulses_.empty();
}

bool KeyboardMouseDriver::Pointer(float* x, float* y, uint32_t* moves) const {
  std::lock_guard<std::mutex> lock(mutex_);
  *x = pointer_x_;
  *y = pointer_y_;
  *moves = pointer_moves_;
  return pointer_seen_;
}

bool KeyboardMouseDriver::DebugPointer(float x, float y) {
  KeyboardMouseDriver* driver = Get();
  if (!driver) return false;
  std::lock_guard<std::mutex> lock(driver->mutex_);
  driver->pointer_x_ = x;
  driver->pointer_y_ = y;
  driver->pointer_seen_ = true;
  ++driver->pointer_moves_;
  return true;
}

void KeyboardMouseDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  rex::ui::Window* window = e.target();
  if (!window) return;
  const float w = float(window->GetActualPhysicalWidth()), h = float(window->GetActualPhysicalHeight());
  if (w <= 0 || h <= 0) return;
  // The presenter fits the 16:9 picture into the window, centred (letterbox
  // bars left/right or top/bottom; rex/ui/presenter.cpp, safe area 100%).
  float pw = w, ph = w * 9.0f / 16.0f;
  if (ph > h) {
    ph = h;
    pw = h * 16.0f / 9.0f;
  }
  const float x = (float(e.x()) - (w - pw) / 2) / pw, y = (float(e.y()) - (h - ph) / 2) / ph;
  std::lock_guard<std::mutex> lock(mutex_);
  pointer_x_ = x;
  pointer_y_ = y;
  pointer_seen_ = true;
  ++pointer_moves_;
}

bool KeyboardMouseDriver::SpinHeld() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return spin_held_;
}

int KeyboardMouseDriver::TakeListWheel() {
  std::lock_guard<std::mutex> lock(mutex_);
  const int notches = list_wheel_;
  list_wheel_ = 0;
  return notches;
}

bool KeyboardMouseDriver::TakeSpinCircle() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (spin_circles_.empty()) return false;
  spin_circles_.pop_front();
  return true;
}

bool KeyboardMouseDriver::MenuOnScreen() const {
  return menu_check_ && menu_check_();
}

void KeyboardMouseDriver::SetMenuCheck(std::function<bool()> check) {
  std::lock_guard<std::mutex> lock(mutex_);
  menu_check_ = std::move(check);
}

KeyboardMouseDriver::Clock::duration KeyboardMouseDriver::SpinPeriod() const {
  return std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<float>(1.0f / std::max(0.5f, bindings_.settings.spin_turns_per_second)));
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

// -----------------------------------------------------------------------------
// Spin, the game's way (findings/38). The fight trees ask HasControlDone360
// (sub_8221E200, r3 = the character's behaviour controller: its vtable slot 92 gives the player's
// controller object, whose left-stick circle detector at +488 answers
// sub_82272FB8 -> its "done" byte +40). The detector reports a finished
// circle ONCE (measured: true for one question, false at the next), so a
// waiting circle from the Spin key is the same answer.
// -----------------------------------------------------------------------------
REXCVAR_DECLARE(bool, simple_spinning);  // gameplay/game_mods.cpp
extern "C" REX_FUNC(__imp__sub_8221E200);
// An actor (CActor) holds its behaviour controller (here class "Crash", a
// CScriptBehaviourController) at +0x40: that's what the question gets
// (measured: P1's Crash at ...A30, the question for ...A70).
constexpr uint32_t kActorBehaviours = 0x40;

// Is this character's controller LOCKED (cutscenes, in-game scenes that take
// the player's control)? The game's own answer first walks up to the root
// controller (sub_82272D68: +36 = parent, root's +516 top byte > 0 = locked)
// and, when locked, says "no circle" whatever the stick did. The Spin key must
// obey the same lock: without it, Q spun Crash during cutscenes (2026-10-15).
// The controller = the behaviour controller's vtable slot 23 (+92), as the
// question itself gets it (sub_8221E200).
extern "C" REX_FUNC(__imp__sub_82272D68);
static bool ControllerLocked(PPCContext& ctx, uint8_t* base, uint32_t behaviours) {
  auto read32 = [&](uint32_t a) {
    const uint8_t* p = GuestPtr(base, a);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  PPCFunc* slot = rex::runtime::ResolveIndirectFunction(read32(read32(behaviours) + 92));
  if (!slot) return false;
  const PPCContext saved = ctx;
  ctx.r3.u64 = behaviours;
  slot(ctx, base);
  const uint32_t controller = ctx.r3.u32;
  bool locked = false;
  if (controller) {
    ctx.r3.u64 = controller;
    __imp__sub_82272D68(ctx, base);
    locked = (ctx.r3.u32 & 0xFF) != 0;
  }
  ctx = saved;
  return locked;
}

extern "C" REX_FUNC(sub_8221E200) {
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8221E200(ctx, base);
  // Whose character is asking: player p's Crash on foot, or the titan p rides
  // (the fight tree asks for whichever is being controlled).
  int player = -1;
  bool on_foot = false;
  for (int p = 0; p < 4 && player < 0; ++p) {
    const uint32_t crash = more_players::CharacterOfPlayer(p), titan = more_players::TitanOfPlayer(p);
    if (crash && character == crash + kActorBehaviours) {
      player = p;
      on_foot = true;
    } else if (titan && character == titan + kActorBehaviours) {
      player = p;
    }
  }
  if (ctx.r3.u32 & 0xFF) {  // the stick (a controller) did a circle itself
    if (player >= 0 && on_foot) game_mods::SpinStarted(player);
    return;
  }
  if (player < 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("KBM: HasControlDone360 asked for {:08X}, no player's character "
                  "(P1 Crash {:08X})", character, more_players::CharacterOfPlayer(0));
    }
    return;
  }
  // Keep spinning holds the current spin (the wrapper below): a new circle
  // now would start a fresh spin and cut it (gameplay/game_mods.h).
  if (game_mods::Holding(player)) return;
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  const bool keyboard = driver && assignment && assignment->KeyboardAnswersFor(uint32_t(player));
  // Our circles (the keyboard's Spin key, simple spinning's LB) obey the
  // game's own lock, like the stick's: no spins in cutscenes.
  if (keyboard && !REXCVAR_GET(simple_spinning)) driver->TakeSpinCircle();  // off: dropped
  if (ControllerLocked(ctx, base, character)) {
    // Drop what's waiting (it would start a spin the moment the scene ends).
    const bool dropped = (keyboard && driver->TakeSpinCircle()) | game_mods::TakeButtonSpin(player);
    if (dropped && REXCVAR_GET(debug_kbm_trace)) {
      REXLOG_INFO("KBM: Spin: player {}'s controller is locked (a cutscene): no spin", player + 1);
    }
    return;
  }
  const char* from = nullptr;
  // The keyboard's Spin key is part of Simple Spinning too (Options ->
  // Gameplay): off = the key does nothing, like LB (2026-10-15).
  if (keyboard && REXCVAR_GET(simple_spinning) && driver->TakeSpinCircle()) {
    from = "the Spin key";
  } else if (game_mods::TakeButtonSpin(player)) {
    from = "simple spinning's button";
  }
  if (!from) return;
  ctx.r3.u64 = 1;
  if (on_foot) game_mods::SpinStarted(player);
  if (REXCVAR_GET(debug_kbm_trace)) REXLOG_INFO("KBM: Spin: a full circle for player {} ({})", player + 1, from);
}

// -----------------------------------------------------------------------------
// KEEP SPINNING, the game's way (findings/40 s.4.3). Crash's spin state
// (Crash.bfig SpinAttack) decides "spin again" in compiled decisions, one per
// spin upgrade: sub_821A3578 (exit 609), ..3628 (611), ..36D8 (613), ..3788
// (615), ..3838 (617), ..38E8 (619), ..3998 (621), ..3A48 (623), plus the
// spinner ones ..3AF8 (627) and ..3C20 (629). Each asks, once the spin has
// run a moment: CheckInputEvent(PLAYER, ATTACK_LIGHT) (sub_8221F1E8, a
// PRESS = the Old Skool "Repeatedly Press Light Attack") OR HasLookDone360
// (sub_8221E1C8: did the RIGHT stick make a circle?); either = spin again
// (the disc's Lua copy, fighttrees/Crashlua lines 4735-5080, reads the same).
// While the player holds the spin (game_mods::Holding), the second question
// is answered "yes" for THOSE call sites only (by return address): no button
// is faked, so a Light attack the player holds stays theirs, and the right
// stick's other uses (the co-op power shot) are untouched. The game's own
// limit still applies: after the last upgrade the spin goes to Alert ->
// OnePressTooMany -> dizzy.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(__imp__sub_8221E1C8);
static bool FromSpinAgainDecision(uint32_t return_address) {
  // The instruction after each decision's `bl 0x8221E1C8` (xexdis).
  static constexpr uint32_t kSites[] = {0x821A35E8, 0x821A3698, 0x821A3748, 0x821A37F8, 0x821A38A8,
                                        0x821A3958, 0x821A3A08, 0x821A3AB8, 0x821A3B64, 0x821A3CB0};
  for (uint32_t site : kSites) {
    if (return_address == site) return true;
  }
  return false;
}

extern "C" REX_FUNC(sub_8221E1C8) {
  const uint32_t character = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8221E1C8(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !FromSpinAgainDecision(caller)) return;
  int player = -1;
  for (int p = 0; p < 4 && player < 0; ++p) {
    const uint32_t crash = more_players::CharacterOfPlayer(p);
    if (crash && character == crash + kActorBehaviours) player = p;
  }
  if (player < 0 || !game_mods::Holding(player)) return;
  if (ControllerLocked(ctx, base, character)) return;  // a cutscene took control
  ctx.r3.u64 = 1;
  if (REXCVAR_GET(debug_kbm_trace)) {
    REXLOG_INFO("KBM: keep spinning: player {}'s spin goes on (decision {:08X})", player + 1, caller - 4);
  }
}
