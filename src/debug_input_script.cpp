// =============================================================================
// debug_input_script.cpp -- see debug_input_script.h for the why and how
// =============================================================================

#include "debug_input_script.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

#if defined(__unix__)
#include <fcntl.h>     // open
#include <poll.h>      // poll: wait for FIFO data with a timeout
#include <sys/stat.h>  // mkfifo
#include <unistd.h>    // read, close
#endif

#include <rex/logging.h>

REXCVAR_DEFINE_STRING(debug_input_script, "", "CrashMoM",
                      "Debug: timed input taps, e.g. \"8000:start,30000:lsright:2000\" "
                      "(ms since launch[:hold ms, default 150]; empty = off)");
REXCVAR_DEFINE_STRING(debug_input_fifo, "", "CrashMoM",
                      "Debug: named pipe to read live input commands from, one per "
                      "line: \"<inputs> [hold ms]\" (Linux; empty = off)");

// Xbox types (X_RESULT, X_ERROR_*, X_INPUT_STATE...) live in these.
using namespace rex;
using namespace rex::input;

namespace {

// Any value no other driver uses. The SDK's own synthetic devices use ASCII
// tags ("NOP\0", "MNK\0"); this is "SCR\0". SDL pads count up from small ints.
constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x53435200);

// Stick directions, above the 16 XInput button bits.
constexpr uint32_t kStickLUp = 1u << 16, kStickLDown = 1u << 17;
constexpr uint32_t kStickLLeft = 1u << 18, kStickLRight = 1u << 19;
constexpr uint32_t kStickRUp = 1u << 20, kStickRDown = 1u << 21;
constexpr uint32_t kStickRLeft = 1u << 22, kStickRRight = 1u << 23;

// Input name -> bit (buttons are X_INPUT_GAMEPAD_* from rex/input/input.h).
uint32_t InputFromName(std::string_view name) {
  struct Entry {
    std::string_view name;
    uint32_t bit;
  };
  static constexpr Entry kInputs[] = {
      {"up", 0x0001},         {"down", 0x0002},        {"left", 0x0004},
      {"right", 0x0008},      {"start", 0x0010},       {"back", 0x0020},
      {"lb", 0x0100},         {"rb", 0x0200},          {"a", 0x1000},
      {"b", 0x2000},          {"x", 0x4000},           {"y", 0x8000},
      {"lsup", kStickLUp},    {"lsdown", kStickLDown}, {"lsleft", kStickLLeft},
      {"lsright", kStickLRight}, {"rsup", kStickRUp},  {"rsdown", kStickRDown},
      {"rsleft", kStickRLeft},   {"rsright", kStickRRight},
  };
  for (const Entry& e : kInputs) {
    if (e.name == name) {
      return e.bit;
    }
  }
  return 0;
}

// "a+lsright" -> bits. 0 if empty or any name is unknown.
uint32_t ParseInputs(std::string_view names) {
  uint32_t inputs = 0;
  while (!names.empty()) {
    size_t plus = names.find('+');
    uint32_t bit = InputFromName(names.substr(0, plus));
    if (!bit) {
      return 0;
    }
    inputs |= bit;
    names = plus == std::string_view::npos ? std::string_view() : names.substr(plus + 1);
  }
  return inputs;
}

// Stick axis from two opposite directions: full tilt one way, the other, or 0.
int16_t Axis(uint32_t inputs, uint32_t negative, uint32_t positive) {
  if (inputs & positive) return 32767;
  if (inputs & negative) return -32768;
  return 0;
}

}  // namespace

std::unique_ptr<ScriptedInputDriver> ScriptedInputDriver::CreateFromCvars() {
  std::string_view rest = REXCVAR_GET(debug_input_script);
  const std::string& fifo = REXCVAR_GET(debug_input_fifo);
  if (rest.empty() && fifo.empty()) {
    return nullptr;
  }
  std::vector<Tap> taps;
  while (!rest.empty()) {
    // Split off one "<time>:<inputs>[:<hold>]" token.
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);

    size_t colon1 = token.find(':');
    size_t colon2 = colon1 == std::string_view::npos ? colon1 : token.find(':', colon1 + 1);
    int64_t time_ms = -1, hold_ms = kDefaultHoldMs;
    uint32_t inputs = 0;
    if (colon1 != std::string_view::npos) {
      std::from_chars(token.data(), token.data() + colon1, time_ms);
      inputs = ParseInputs(token.substr(colon1 + 1, colon2 - colon1 - 1));
      if (colon2 != std::string_view::npos) {
        std::from_chars(token.data() + colon2 + 1, token.data() + token.size(), hold_ms);
      }
    }
    if (time_ms < 0 || !inputs || hold_ms <= 0) {
      REXLOG_ERROR("debug_input_script: can't parse \"{}\", script ignored", token);
      return nullptr;
    }
    taps.push_back({time_ms, hold_ms, inputs});
  }
  REXLOG_INFO("debug_input_script: {} scripted taps{}", taps.size(),
              fifo.empty() ? std::string() : ", live commands from " + fifo);
  std::unique_ptr<ScriptedInputDriver> driver(new ScriptedInputDriver(std::move(taps)));
  if (!fifo.empty()) {
    driver->fifo_thread_ = std::thread([d = driver.get(), fifo] { d->FifoThread(fifo); });
  }
  return driver;
}

// InputDriver's constructor wants a window + z-order for drivers that read
// window events. We read none, so null / 0.
ScriptedInputDriver::ScriptedInputDriver(std::vector<Tap> taps)
    : InputDriver(nullptr, 0), start_(std::chrono::steady_clock::now()), taps_(std::move(taps)) {}

ScriptedInputDriver::~ScriptedInputDriver() {
  stop_ = true;
  if (fifo_thread_.joinable()) {
    fifo_thread_.join();  // it polls with a timeout, so this is quick
  }
}

int64_t ScriptedInputDriver::NowMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start_)
      .count();
}

void ScriptedInputDriver::FifoThread(std::string path) {
#if defined(__unix__)
  // Create the pipe if it isn't there yet (EEXIST is fine). Opening it
  // read+write means the open doesn't block waiting for a writer, and we
  // never see end-of-file when one `echo` finishes.
  mkfifo(path.c_str(), 0600);
  int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    REXLOG_ERROR("debug_input_fifo: can't open {}: {}", path, std::strerror(errno));
    return;
  }
  std::string pending;  // bytes read but not yet a complete line
  char buf[256];
  while (!stop_) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 200) <= 0) {
      continue;  // timeout: re-check stop_
    }
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      continue;
    }
    pending.append(buf, size_t(n));
    size_t eol;
    while ((eol = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, eol);
      pending.erase(0, eol + 1);
      // "<inputs> [<hold_ms>]"
      std::string_view view(line);
      size_t space = view.find(' ');
      uint32_t inputs = ParseInputs(view.substr(0, space));
      int64_t hold_ms = kDefaultHoldMs;
      if (space != std::string_view::npos) {
        std::from_chars(view.data() + space + 1, view.data() + view.size(), hold_ms);
      }
      if (!inputs || hold_ms <= 0) {
        REXLOG_WARN("debug_input_fifo: ignoring \"{}\"", line);
        continue;
      }
      int64_t now = NowMs();
      REXLOG_INFO("debug_input_fifo: t={} ms \"{}\"", now, line);
      std::lock_guard<std::mutex> lock(mutex_);
      taps_.push_back({now, hold_ms, inputs});
    }
  }
  close(fd);
#else
  REXLOG_ERROR("debug_input_fifo: only supported on Linux ({} ignored)", path);
#endif
}

X_STATUS ScriptedInputDriver::Setup() {
  return X_STATUS_SUCCESS;  // nothing to open
}

void ScriptedInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kScriptDevice;
  info.name = "Debug input script";
  info.synthetic = true;  // -> merged into guest player 1 (SlotAssignment)
  out.push_back(info);
}

X_RESULT ScriptedInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const int64_t now_ms = NowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  Inputs inputs = 0;
  for (const Tap& tap : taps_) {
    if (now_ms >= tap.start_ms && now_ms < tap.start_ms + tap.hold_ms) {
      inputs |= tap.inputs;
    }
  }
  if (inputs != last_inputs_) {
    // XInput games compare packet_number to spot a new state.
    ++packet_number_;
    if (inputs) {
      REXLOG_INFO("debug_input_script: t={} ms inputs={:06X}", now_ms, inputs);
    }
    last_inputs_ = inputs;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = uint16_t(inputs & 0xFFFF);
    // XInput sticks: +y is up, +x is right.
    out_state->gamepad.thumb_lx = Axis(inputs, kStickLLeft, kStickLRight);
    out_state->gamepad.thumb_ly = Axis(inputs, kStickLDown, kStickLUp);
    out_state->gamepad.thumb_rx = Axis(inputs, kStickRLeft, kStickRRight);
    out_state->gamepad.thumb_ry = Axis(inputs, kStickRDown, kStickRUp);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptedInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                    X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    // Same answer as the SDK's keyboard driver: a standard gamepad
    // (type 1 = gamepad, sub_type 1 = standard controller) with every
    // button and axis available.
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

X_RESULT ScriptedInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) {
  return id == kScriptDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptedInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) {
  // No keystroke events: games that poll XInputGetKeystroke see "empty".
  return id == kScriptDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}
