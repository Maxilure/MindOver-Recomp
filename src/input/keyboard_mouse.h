// =============================================================================
// input/keyboard_mouse.h -- play with keyboard and mouse (a virtual 360 pad)
// =============================================================================
//
// WHAT: turns keys, mouse buttons and the mouse wheel into the controller
// input the game reads, using the player's bindings
// (input/bindings.h: WASD to move, Space to jump, mouse buttons to attack...;
// F6 opens the Controls menu to change them). On by default; a real
// controller keeps working at the same time (--keyboard_mouse=false turns
// this driver off).
//
// WHY OUR OWN (the SDK has --mnk_mode): the SDK's keyboard driver is a
// generic "keys -> buttons" table. This game needs a few things it can't do:
//   * SPIN is a stick gesture, not a button ("Rotate the Left Analog Stick"):
//     the Spin key answers the game's own "a full circle done?" directly
//     (TakeSpinCircle; findings/38).
//   * Menus and play want different keys (Enter / Escape vs Space / E): each
//     key does the job of the context it was pressed in (bindings.h).
//   * Modifier keys as game keys: the SDK matches Shift/Ctrl/Alt exactly, so
//     holding Shift (to block) would silence W/A/S/D. Here every key is just
//     a key; any combination can be held.
//   * Mouse side buttons and the wheel as bindable keys.
//   * Both windows of dual mode (F8) take keys, and a Controls menu can grab
//     "the next key pressed" to rebind.
//
// HOW IT FITS IN (since 2026-10-11, findings/38): NOT as a pretend controller
// of the SDK's input system. The SDK still lists this device (connected and
// SILENT), so the player assignment (input/players.h) knows a keyboard plays
// and which player it is. The game polls XInputGetState once per frame
// (Radical's input manager, sub_8235D188, whose button objects read their
// values from that state); our wrapper of it (saves/save_library.cpp) calls
// MergeInto for the sockets the keyboard answers on, which builds the state
// from the keys held RIGHT THEN, in their contexts, and merges it with the
// controller's (buttons together, a stick the keys push replaces the pad's).
//
// THE STICK FROM KEYS:
//   * Opposite keys (A and D): the one pressed last wins, so a quick change
//     of direction never stops Crash dead.
//   * Diagonals are scaled to the stick's circle (a real stick can't reach
//     a corner); Walk shortens the push (Settings::walk_tilt).
//   * Spin: a press = one "full circle", then one every 1 / turns-per-second
//     while held, each kept 0.4 s for the fight trees to take (the game's
//     own detector reports a circle once too). The stick stays with the
//     movement keys: Crash can be steered while spinning.
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
#include <deque>
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
  // "Is one of the game's menus on screen?" (the front end, or the pause /
  // totem / save menus in a level): asked when a key goes down. Any thread.
  void SetMenuCheck(std::function<bool()> check);

  // The game's XInputGetState for a socket the keyboard answers on (players.h
  // KeyboardAnswersFor): adds the keys' state to `gamepad` (guest
  // X_INPUT_GAMEPAD, 12 big-endian bytes). Game threads.
  void MergeInto(uint8_t* gamepad);
  // One "full circle of the left stick" from the Spin key, if one is waiting
  // (consumed). For the fight trees' HasControlDone360 (players/... wrapper
  // in keyboard_mouse.cpp). Game threads.
  bool TakeSpinCircle();

  // --- The mouse in menus (input/mouse_menus.h) --------------------------
  // Where the pointer is over the game's picture: 0..1 across its 16:9
  // picture (the presenter's centred fit), and a counter that goes up at
  // every move (hover only follows a pointer that moved). False if no
  // pointer has been seen over a game window. Any thread.
  bool Pointer(float* x, float* y, uint32_t* moves) const;
  // A left click made while a menu was on screen, waiting for the menu code
  // to say what it hit (dropped after 0.2 s). Taking it consumes it.
  bool TakeMenuClick();
  // The same without taking it: is a fresh one waiting? `x` / `y` (if given)
  // = where the pointer was when the button went down (0..1 over the
  // picture, as Pointer): a click counts where it was MADE, not where the
  // pointer went in the frame or two before a menu looks at it.
  bool PeekMenuClick(float* x = nullptr, float* y = nullptr) const;
  // Presses `buttons` (X_INPUT_GAMEPAD_*) for a moment (~80 ms), queued after
  // any earlier ones: a click's A, a right click's B, a wheel notch's D-pad.
  void Pulse(uint16_t buttons);
  // Is a pulse being pressed or waiting? (The click lock in mouse_menus ends
  // once the click's press has reached the game.)
  bool PulseBusy() const;
  // Debug FIFO "mouse <x> <y>" (0..1 over the picture): moves the pointer.
  static bool DebugPointer(float x, float y);

  // Thread-safe copy / replace of the bindings (the menu edits a copy).
  Bindings bindings() const;
  void SetBindings(const Bindings& bindings);
  // Goes up by one at every SetBindings (any thread): the prompt pictures
  // (prompt_icons.h) redraw the keys when it changes.
  uint32_t bindings_revision() const { return bindings_revision_.load(); }

  // For the prompt pictures (prompt_icons.h), any thread:
  //   last_key_down(): when a key, mouse button or wheel notch last went down
  //   (steady_clock nanoseconds; 0 = never), debug FIFO taps included;
  //   last_pad(): the pad this driver built at its last poll (what it added to
  //   its player's state), to tell the keyboard's part from a controller's.
  int64_t last_key_down() const { return last_key_down_.load(); }
  rex::input::X_INPUT_GAMEPAD last_pad() const;

  // UI thread, for the Controls menu: the next key / mouse button / wheel
  // notch in any attached window is handed to `done` instead of the game
  // (Esc hands kNoInput = cancelled). Tool keys (F6, F9...) keep working.
  void Capture(std::function<void(Input)> done);
  void CancelCapture();
  bool capturing() const;

  // Is this key / mouse button held right now (any thread)? Raw keys, not
  // actions, and also while the game's input is paused: the cheat menu's free
  // camera flies with them (cheats/free_camera.h). Debug FIFO taps count.
  bool InputHeld(Input input) const;

  // Debug FIFO (debug_input_script.h): press `key_name` for `hold_ms` as if
  // typed (in the context of that moment). Works without focus. False if the
  // name is unknown / no driver.
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
  void OnMouseMove(rex::ui::MouseEvent& e) override;
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
  bool MenuOnScreen() const;                     // lock held
  Clock::duration SpinPeriod() const;            // lock held
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
  // What a key going down does in a menu beyond its bindings (the mouse:
  // left click queued for the menu, right click = B, wheel = D-pad). Lock held.
  void MenuMouseDown(Input input, Clock::time_point now);
  // Pointer (Pointer()): 0..1 over the picture, moves counter.
  float pointer_x_ = 0.5f, pointer_y_ = 0.5f;
  uint32_t pointer_moves_ = 0;
  bool pointer_seen_ = false;
  // Waiting left click (MenuMouseDown), time it was made; pulses.
  Clock::time_point menu_click_{};
  float menu_click_x_ = 0, menu_click_y_ = 0;
  std::deque<uint16_t> pulses_;
  Clock::time_point pulse_until_{}, pulse_gap_until_{};
  uint16_t pulse_now_ = 0;
  // Per input: pressed while a menu was on screen (its context, bindings.h).
  std::array<bool, kInputCount> in_menu_{};
  // Spin: held at the last poll, when the next circle is due, circles waiting.
  bool spin_held_ = false;
  Clock::time_point next_spin_{};
  std::deque<Clock::time_point> spin_circles_;
  // What the keys added last (prompt_icons.h, --debug_kbm_trace).
  rex::input::X_INPUT_GAMEPAD last_pad_{};

  std::atomic<uint32_t> bindings_revision_{0};
  std::atomic<int64_t> last_key_down_{0};

  std::function<uint32_t()> paused_check_;
  std::function<bool()> menu_check_;
  bool mouse_blocked_ = false;  // this poll: mouse buttons / wheel don't count
  std::atomic<bool> menu_open_{false};

  // UI thread only.
  std::vector<rex::ui::Window*> windows_;
  std::function<void(Input)> capture_done_;
  Input swallow_mouse_up_ = kNoInput;  // the button that ended a capture
};

}  // namespace kbm
