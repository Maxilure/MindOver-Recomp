// =============================================================================
// debug_input_script.h -- a fake controller for test runs (timed or live)
// =============================================================================
//
// A debugging aid, OFF unless you pass --debug_input_script and/or
// --debug_input_fifo.
//
// Why it exists: every test run starts with ~100 s of intro movies that only
// a button press skips, and Claude can't press buttons on the user's
// controller. Scripted presses make test runs short and repeatable (skip the
// intros), and the live FIFO lets Claude navigate menus it has never seen:
// press a button, look at the screenshot (--debug_capture_dir), decide.
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
  };
  static constexpr int64_t kDefaultHoldMs = 150;

  explicit ScriptedInputDriver(std::vector<Tap> taps);
  int64_t NowMs() const;
  void FifoThread(std::string path);

  const std::chrono::steady_clock::time_point start_;

  std::mutex mutex_;       // guards everything below
  std::vector<Tap> taps_;  // timeline taps + live taps (appended by the FIFO)
  Inputs last_inputs_ = 0;
  uint32_t packet_number_ = 1;  // must change whenever the state changes

  std::thread fifo_thread_;
  std::atomic<bool> stop_{false};
};
