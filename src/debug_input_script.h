// =============================================================================
// debug_input_script.h -- a fake controller for test runs (timed or live)
// =============================================================================
//
// A debugging aid, OFF unless you pass --debug_input_script and/or
// --debug_input_fifo.
//
// Why it exists: every test run starts with ~100 s of intro movies that only
// a button press skips. Scripted presses make test runs short and repeatable
// without anyone at the controller (skip the intros), and the live FIFO lets
// a script or a terminal drive the menus step by step: press a button, look
// at the screenshot (--debug_capture_dir), decide.
//
// How it works: ReXGlue's InputSystem asks every "input driver" for its
// devices and merges the ones flagged `synthetic` (the keyboard driver, and
// this one) into guest player 1, together with the first real pad. So the
// game sees one controller whose buttons are (real pad OR this driver). It
// keeps working when the window doesn't have focus.
//
// Inputs: buttons  a b x y start back lb rb up down left right (d-pad)
//         sticks   lsup lsdown lsleft lsright (left stick, full tilt)
//                  rsup rsdown rsleft rsright (right stick)
//         combine with "+", e.g. lsright+a (run right and jump)
//
// 1) Timeline: --debug_input_script="<ms>:<inputs>[:<hold_ms>],..."
//    ms since launch; hold defaults to 150 ms.
//      --debug_input_script="8000:start,9500:start,30000:lsright:2000"
//
// 2) Live (Linux): --debug_input_fifo=/path/to/fifo
//    The game creates the named pipe (FIFO) if needed and reads one command
//    per line, applied the moment it arrives: "<inputs> [<hold_ms>]".
//      echo "down" > /path/to/fifo
//      echo "lsright 2000" > /path/to/fifo
//    "key <Name> [<hold_ms>]" presses a KEYBOARD / MOUSE key instead, through
//    the keyboard driver and the player's bindings (input/keyboard_mouse.h),
//    exactly as if typed (names as in controls.toml: Space, LMB, WheelUp...):
//      echo "key Q 1000" > /path/to/fifo     (hold Spin for a second)
//      echo "cheat god on" > /path/to/fifo   (a cheat: cheats/cheats.h)
//
// 3) MORE FAKE CONTROLLERS (local multiplayer tests): --debug_fake_pads=N
//    (1-3) adds N more fake controllers, "Debug fake controller 2..N+1", which
//    play as players 2..N+1 by default (input/players.h; the Players tab can
//    move them like any controller). A "p<N>." prefix sends a command to fake
//    controller N instead of the first one (p1 = the first, the default),
//    in the timeline and the FIFO alike:
//      --debug_input_script="30000:p2.start,32000:p3.lsup+a:1000"
//      echo "p3.lsup 2000" > /path/to/fifo
//    Why: testing 3-4 players needs 3-4 controllers; nobody has to own them.
//    FIFO "p<N>.unplug" / "p<N>.plug" (N = 1-4): that fake controller is
//    pulled out / plugged back in (a lost controller, players/lost_controller.h).
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>

REXCVAR_DECLARE(std::string, debug_input_script);
REXCVAR_DECLARE(std::string, debug_input_fifo);
REXCVAR_DECLARE(int32_t, debug_fake_pads);

class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  // Builds the driver from --debug_input_script / --debug_input_fifo.
  // Returns nullptr when both are empty (feature off) or the script doesn't
  // parse (logged).
  static std::unique_ptr<ScriptedInputDriver> CreateFromCvars();
  ~ScriptedInputDriver() override;

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

 private:
  // Low 16 bits: X_INPUT_GAMEPAD_* buttons. Bits 16-23: stick directions
  // (see kStick* in the .cpp), turned into full-tilt thumbstick values.
  using Inputs = uint32_t;
  struct Tap {
    int64_t start_ms;
    int64_t hold_ms;
    Inputs inputs;
    int pad;  // 0 = the first fake controller, 1-3 = --debug_fake_pads' extra ones
  };
  static constexpr int64_t kDefaultHoldMs = 150;
  static constexpr int kMaxPads = 4;  // the first + up to 3 extra

  ScriptedInputDriver(std::vector<Tap> taps, int pads);
  // Which of our pads a device id is (-1: not ours).
  int PadOf(rex::input::DeviceId id) const;
  int64_t NowMs() const;
  void FifoThread(std::string path);

  const std::chrono::steady_clock::time_point start_;

  std::mutex mutex_;       // guards everything below
  const int pads_;          // how many fake controllers (1 + --debug_fake_pads)
  std::vector<Tap> taps_;  // timeline taps + live taps (appended by the FIFO)
  Inputs last_inputs_[kMaxPads] = {};
  uint32_t packet_number_[kMaxPads] = {1, 1, 1, 1};  // must change whenever the state changes
  // "p<N>.unplug" / "p<N>.plug": a fake controller that is unplugged
  // isn't listed any more (the SDK re-lists devices at every poll), exactly
  // like a real pad pulled out. For the lost-controller handling's tests.
  std::atomic<bool> unplugged_[kMaxPads] = {};

  std::thread fifo_thread_;
  std::atomic<bool> stop_{false};
};
