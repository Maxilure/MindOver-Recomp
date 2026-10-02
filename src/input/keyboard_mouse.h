// =============================================================================
// input/keyboard_mouse.h -- play with keyboard and mouse (a virtual 360 pad)
// =============================================================================
//
// WHAT: an input driver that turns keys, mouse buttons and the mouse wheel
// into the Xbox 360 controller the game expects, using the player's bindings
// (input/bindings.h: WASD to move, Space to jump, mouse buttons to attack...;
// F6 opens the Controls menu to change them). On by default; a real
// controller keeps working at the same time (--keyboard_mouse=false turns
// this driver off).
//
// WHY OUR OWN (the SDK has --mnk_mode): the SDK's keyboard driver is a
// generic "keys -> buttons" table. This game needs a few things it can't do:
//   * SPIN is a stick gesture, not a button ("Rotate the Left Analog Stick"):
//     the Spin key draws full circles with the virtual stick.
//   * Modifier keys as game keys: the SDK matches Shift/Ctrl/Alt exactly, so
//     holding Shift (to block) would silence W/A/S/D. Here every key is just
//     a key; any combination can be held.
//   * Mouse side buttons and the wheel as bindable keys.
//   * Both windows of dual mode (F8) take keys, and a Controls menu can grab
//     "the next key pressed" to rebind.
//
// HOW IT FITS IN: ReXGlue's InputSystem asks every driver for its devices
// and a "device assignment" which devices feed which player; ours
// (input/players.h) puts this one on player 1 unless the Players tab says
// otherwise, together with the first real pad. Devices on one player are
// merged: buttons OR'ed, sticks take the bigger push. The game polls XInputGetState once per frame (Radical's input
// manager, sub_8235D188); each poll lands in GetDeviceState, which builds
// the pad state from the keys held RIGHT THEN.
//
// THE STICK FROM KEYS:
//   * Opposite keys (A and D): the one pressed last wins, so a quick change
//     of direction never stops Crash dead.
//   * Diagonals are scaled to the stick's circle (a real stick can't reach
//     a corner); Walk shortens the push (Settings::walk_tilt).
//   * Spin, while held: angle = start - 2 pi x turns/s x time (clockwise),
//     starting from the direction Crash was being pushed, so he doesn't
//     jerk. On release the circle in progress is finished, plus 0.15 of a
//     turn (a quick tap = one whole circle, ending past where it began, like
//     a thumb), then the movement keys take over again. The game
//     counts a circle at a time and drops the count if the next takes over
//     1.0 s (findings/23), so turns/s anywhere from ~1.5 works.
//   * The mouse wheel has no "held": each notch = a short press (60 ms) and
//     a short gap (40 ms), queued (at most 4), so fast scrolling still gives
//     separate presses the game can count.
//
// WHEN THE GAME GETS NOTHING (a neutral pad): no game window focused, an
// ImGui overlay is using the keyboard (F4 settings, console), or the
// Controls menu is open. While ImGui only wants the MOUSE (the pointer is
// over an overlay or a pop-up), just the mouse buttons and wheel stop
// counting: clicking an overlay never attacks, and the keys keep playing.
// Keys are forgotten when a window loses focus (its key-up would never
// arrive: no stuck keys).
//
// Threads: key / mouse events and Attach/Capture on the UI thread;
// GetDeviceState on the game's threads. One mutex guards the shared state.
// Debug: --debug_kbm_trace logs every change of the pad state the game sees;
// the debug FIFO's "key <Name> [ms]" presses a key as if typed (for tests).
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "bindings.h"

REXCVAR_DECLARE(bool, keyboard_mouse);

namespace kbm {

class KeyboardMouseDriver final : public rex::input::InputDriver,
                                  public rex::ui::WindowInputListener,
                                  public rex::ui::WindowListener {
 public:
  // Our device's handle in the input system ("KBM\0": any id no other driver
  // uses; the SDK's are "NOP\0" and "MNK\0", the debug script's "SCR\0", SDL
  // pads count up from small numbers). players.h tells it from the pads.
  static constexpr rex::input::DeviceId kDeviceId = static_cast<rex::input::DeviceId>(0x4B424D00);

  // Loads the controls file (bindings.h). Null when --keyboard_mouse=false.
  static std::unique_ptr<KeyboardMouseDriver> Create();
  // The one driver (null if none): for the Controls menu and the debug FIFO.
  // Lives until the input system shuts down, after every window is gone.
  static KeyboardMouseDriver* Get();
  ~KeyboardMouseDriver() override;

  // UI thread. Start / stop taking keys and mouse from a window (the main
  // window; the dual-mode window while it's open). Attach twice = once.
  // A window closing detaches itself.
  void Attach(rex::ui::Window* window);
  void Detach(rex::ui::Window* window);

  // Extra "keep input from the game" check, asked at every poll (the app's:
  // no game window focused / an ImGui overlay uses the keys or the mouse).
  // Returns kPauseAll and/or kPauseMouse (mouse buttons and wheel only, so a
  // pop-up under the pointer doesn't freeze the keyboard player).
  static constexpr uint32_t kPauseAll = 1, kPauseMouse = 2;
  void SetPausedCheck(std::function<uint32_t()> paused);
  // The Controls menu is open: game input paused, cursor shown.
  void SetMenuOpen(bool open);

  // Thread-safe copy / replace of the bindings (the menu edits a copy).
  Bindings bindings() const;
  void SetBindings(const Bindings& bindings);

  // UI thread, for the Controls menu: the next key / mouse button / wheel
  // notch in any attached window is handed to `done` instead of the game
  // (Esc hands kNoInput = cancelled). Tool keys (F6, F9...) keep working.
  void Capture(std::function<void(Input)> done);
  void CancelCapture();
  bool capturing() const;

  // Debug FIFO (debug_input_script.h): press `key_name` for `hold_ms` as if
  // typed. Works without focus. False if the name is unknown / no driver.
  static bool DebugTap(std::string_view key_name, int64_t hold_ms);

  // InputDriver (game threads).
  rex::X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,
                               rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                                   rex::input::X_INPUT_VIBRATION* vibration) override;
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

  // WindowInputListener / WindowListener (UI thread).
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnClosing(rex::ui::UIEvent& e) override;

 private:
  using Clock = std::chrono::steady_clock;

  explicit KeyboardMouseDriver(Bindings bindings);

  // Key / button down or up, from any source (UI thread or debug FIFO).
  // Takes the lock.
  void Press(Input input, bool down);
  // Everything a capture or the game might want from a press; returns true
  // if the event was consumed (capture).
  bool HandleDown(Input input);
  void ReleaseAll();  // lock held
  // Mouse event button -> Input (kNoInput for none).
  static Input MouseInput(const rex::ui::MouseEvent& e);
  // Builds the pad from the held inputs (lock held).
  void BuildState(Clock::time_point now, rex::input::X_INPUT_GAMEPAD& pad);
  bool ActionHeld(Action action) const;          // lock held
  uint32_t ActionPressOrder(Action action) const;  // lock held: 0 = not held
  // One stick axis from two opposite actions: +1, -1 or 0 (last pressed wins).
  float Axis(Action negative, Action positive) const;

  mutable std::mutex mutex_;  // guards everything below
  Bindings bindings_;
  // Per input: held right now, and when it was pressed (a counter, so "which
  // was pressed last" is a comparison).
  std::array<bool, kInputCount> held_{};
  std::array<uint32_t, kInputCount> pressed_at_{};
  uint32_t press_counter_ = 0;
  // Debug taps: release time per input (Clock::time_point{} = none).
  std::array<Clock::time_point, kInputCount> debug_release_{};
  // Mouse wheel pulses, [0] = up, [1] = down.
  struct WheelPulse {
    int queued = 0;
    Clock::time_point down_until{}, gap_until{};
  };
  std::array<WheelPulse, 2> wheel_{};
  // Spin gesture.
  struct Spin {
    bool active = false;
    Clock::time_point start{};
    float start_angle = 0.0f;  // radians, 0 = right, pi/2 = up
    int end_turns = 0;         // > 0: finishing; stop after this many turns
  } spin_;
  // Last pushed direction of the left stick (where a spin starts).
  float last_move_angle_ = 1.5707964f;
  // What the game saw last (for packet numbers and --debug_kbm_trace).
  rex::input::X_INPUT_GAMEPAD last_pad_{};
  uint32_t packet_number_ = 1;

  std::function<uint32_t()> paused_check_;
  bool mouse_blocked_ = false;  // this poll: mouse buttons / wheel don't count
  std::atomic<bool> menu_open_{false};

  // UI thread only.
  std::vector<rex::ui::Window*> windows_;
  std::function<void(Input)> capture_done_;
  Input swallow_mouse_up_ = kNoInput;  // the button that ended a capture
};

}  // namespace kbm
