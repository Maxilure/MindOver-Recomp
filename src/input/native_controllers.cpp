// SDL gamepads owned by the port. The original Xbox wrappers define the
// buffer layouts at the guest boundary (findings/38 s.8).
#include "native_controllers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "players.h"

REXCVAR_DEFINE_BOOL(native_controller_state, true, "CrashMoM",
                    "Use the port's SDL controller backend (false = original ReXGlue input)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, guide_button);

extern "C" REX_FUNC(__imp__sub_824742E8);
extern "C" REX_FUNC(__imp__sub_82474300);

namespace native_controllers {
namespace {

using namespace rex;
using namespace rex::input;
using Clock = std::chrono::steady_clock;

// Player 1 keeps a neutral socket when no physical pad is connected. This
// lets the front end start and lets the keyboard merge run on that socket.
constexpr auto kIdle = static_cast<DeviceId>(1);
constexpr uint32_t kSyntheticOrder = UINT32_MAX;

struct Pad {
  DeviceInfo info;
  std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)> handle{nullptr, SDL_CloseGamepad};
  X_INPUT_STATE last{};
};

std::mutex g_mutex;
std::unique_ptr<kbm::PlayerAssignment> g_assignment;
std::unique_ptr<InputDriver> g_keyboard, g_scripted;
std::map<SDL_JoystickID, Pad> g_pads;
std::vector<DeviceInfo> g_devices;
std::map<DeviceId, InputDriver*> g_synthetic;
std::array<DeviceId, 4> g_active_devices{};
std::function<bool()> g_active_check;
Clock::time_point g_next_poll;
bool g_sdl = false;

memory::PageAccess GuestAccess(uint32_t address, uint32_t size) {
  if (!address || !size) return memory::PageAccess::kNoAccess;
  auto* runtime = Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  return heap ? heap->QueryRangeAccess(address, address + size - 1)
              : memory::PageAccess::kNoAccess;
}

DeviceId PhysicalId(SDL_JoystickID id) {
  // Keep SDL instance IDs separate from the keyboard and scripted IDs.
  return static_cast<DeviceId>((uint64_t{1} << 32) | id);
}

Pad* FindPad(DeviceId id) {
  if (uint64_t(id) >> 32 != 1) return nullptr;
  auto found = g_pads.find(SDL_JoystickID(uint64_t(id)));
  return found == g_pads.end() ? nullptr : &found->second;
}

void Refresh() {
  if (!g_assignment) return;
  const auto now = Clock::now();
  if (now < g_next_poll) return;
  // Cap SDL sampling independently of how many guest sockets are polled.
  g_next_poll = now + std::chrono::milliseconds(4);
  if (g_sdl) {
    SDL_UpdateGamepads();
    int count = 0;
    std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> ids(SDL_GetGamepads(&count), SDL_free);
    if (ids) {
      auto* end = ids.get() + count;
      std::erase_if(g_pads, [&](const auto& entry) {
        return std::find(ids.get(), end, entry.first) == end;
      });
      for (const auto* id = ids.get(); id != end; ++id) {
        if (g_pads.contains(*id)) continue;
        Pad pad;
        pad.handle.reset(SDL_OpenGamepad(*id));
        if (!pad.handle) continue;
        pad.info.id = PhysicalId(*id);
        if (const char* name = SDL_GetGamepadName(pad.handle.get())) pad.info.name = name;
        char guid[33]{};
        SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad.handle.get())), guid,
                         sizeof(guid));
        pad.info.guid = guid;
        // Preserve live pads' ordinals when another pad disconnects.
        uint32_t ordinal = 0;
        while (std::any_of(g_pads.begin(), g_pads.end(), [&](const auto& entry) {
          return entry.second.info.ordinal == ordinal;
        })) ++ordinal;
        pad.info.ordinal = ordinal;
        g_pads.emplace(*id, std::move(pad));
      }
    }
  }

  std::vector<DeviceInfo> devices;
  for (const auto& [id, pad] : g_pads) devices.push_back(pad.info);
  std::sort(devices.begin(), devices.end(),
            [](const auto& a, const auto& b) { return a.ordinal < b.ordinal; });
  devices.push_back({kIdle, kSyntheticOrder, "None", {}, true});
  g_synthetic.clear();
  for (auto* driver : {g_scripted.get(), g_keyboard.get()}) {
    if (!driver) continue;
    const auto first = devices.size();
    driver->EnumerateDevices(devices);
    for (size_t i = first; i < devices.size(); ++i) {
      devices[i].ordinal = kSyntheticOrder;
      g_synthetic.emplace(devices[i].id, driver);
    }
  }
  if (devices.size() != g_devices.size() ||
      !std::equal(devices.begin(), devices.end(), g_devices.begin(),
                  [](const auto& a, const auto& b) { return a.id == b.id; })) {
    g_devices = std::move(devices);
    g_assignment->OnDevicesChanged(g_devices);
  }
}

// SDL reports triggers as 0..32767 and positive Y down. The game's
// triggers are bytes and its signed stick Y points up.
X_INPUT_GAMEPAD Sample(SDL_Gamepad* pad) {
  constexpr std::pair<SDL_GamepadButton, uint16_t> buttons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, X_INPUT_GAMEPAD_A},
      {SDL_GAMEPAD_BUTTON_EAST, X_INPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, X_INPUT_GAMEPAD_X},
      {SDL_GAMEPAD_BUTTON_NORTH, X_INPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_BACK, X_INPUT_GAMEPAD_BACK},
      {SDL_GAMEPAD_BUTTON_START, X_INPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, X_INPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, X_INPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, X_INPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, X_INPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  uint16_t mask = 0;
  for (auto [button, bit] : buttons) {
    if (SDL_GetGamepadButton(pad, button)) mask |= bit;
  }
  if (REXCVAR_GET(guide_button) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE)) {
    mask |= X_INPUT_GAMEPAD_GUIDE;
  }
  auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a); };
  auto trigger = [&](SDL_GamepadAxis a) { return uint8_t(std::max(0, int(axis(a))) >> 7); };
  X_INPUT_GAMEPAD state{};
  state.buttons = mask;
  state.left_trigger = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  state.right_trigger = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
  state.thumb_lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
  state.thumb_ly = int16_t(~axis(SDL_GAMEPAD_AXIS_LEFTY));
  state.thumb_rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
  state.thumb_ry = int16_t(~axis(SDL_GAMEPAD_AXIS_RIGHTY));
  return state;
}

bool ReadOne(DeviceId id, X_INPUT_STATE& out) {
  if (id == kIdle) return true;
  if (auto* pad = FindPad(id)) {
    const bool active = !g_active_check || g_active_check();
    SDL_LockJoysticks();
    const auto state = active ? Sample(pad->handle.get()) : X_INPUT_GAMEPAD{};
    SDL_UnlockJoysticks();
    if (std::memcmp(&state, &pad->last.gamepad, sizeof(state)) != 0) {
      pad->last.packet_number = uint32_t(pad->last.packet_number) + 1;
      pad->last.gamepad = state;
    }
    out = pad->last;
    return true;
  }
  const auto driver = g_synthetic.find(id);
  return driver != g_synthetic.end() && driver->second->GetDeviceState(id, &out) == X_ERROR_SUCCESS;
}

int16_t Stronger(int16_t a, int16_t b) {
  return std::abs(int(a)) >= std::abs(int(b)) ? a : b;
}

void Merge(X_INPUT_STATE& a, const X_INPUT_STATE& b) {
  a.packet_number = std::max(uint32_t(a.packet_number), uint32_t(b.packet_number));
  a.gamepad.buttons = uint16_t(a.gamepad.buttons) | uint16_t(b.gamepad.buttons);
  a.gamepad.left_trigger = std::max(a.gamepad.left_trigger, b.gamepad.left_trigger);
  a.gamepad.right_trigger = std::max(a.gamepad.right_trigger, b.gamepad.right_trigger);
  a.gamepad.thumb_lx = Stronger(a.gamepad.thumb_lx, b.gamepad.thumb_lx);
  a.gamepad.thumb_ly = Stronger(a.gamepad.thumb_ly, b.gamepad.thumb_ly);
  a.gamepad.thumb_rx = Stronger(a.gamepad.thumb_rx, b.gamepad.thumb_rx);
  a.gamepad.thumb_ry = Stronger(a.gamepad.thumb_ry, b.gamepad.thumb_ry);
}

bool HasInput(const X_INPUT_GAMEPAD& pad) {
  return pad.buttons || pad.left_trigger >= 30 || pad.right_trigger >= 30 ||
         std::max({std::abs(int16_t(pad.thumb_lx)), std::abs(int16_t(pad.thumb_ly)),
                   std::abs(int16_t(pad.thumb_rx)), std::abs(int16_t(pad.thumb_ry))}) >= 7849;
}

std::vector<DeviceId> DevicesFor(uint32_t user) {
  Refresh();
  std::vector<DeviceId> devices;
  if (g_assignment && user < g_active_devices.size()) g_assignment->DevicesForUser(user, devices);
  return devices;
}

X_RESULT Capabilities(uint32_t user, uint32_t flags, X_INPUT_CAPABILITIES& caps) {
  std::lock_guard lock(g_mutex);
  if ((flags & 0xff) && !(flags & X_INPUT_FLAG_GAMEPAD)) return X_ERROR_DEVICE_NOT_CONNECTED;
  // The title's import accepts the any-user marker and resolves it to socket 0.
  if ((user & 0xff) == 0xff || (flags & (1u << 30))) user = 0;
  const auto ids = DevicesFor(user);
  if (ids.empty()) return X_ERROR_DEVICE_NOT_CONNECTED;
  const auto active = g_active_devices[user];
  const auto id = std::find(ids.begin(), ids.end(), active) == ids.end() ? ids.front() : active;
  if (const auto driver = g_synthetic.find(id); driver != g_synthetic.end()) {
    return driver->second->GetDeviceCapabilities(id, flags, &caps);
  }
  // Gamepad ABI: type/subtype 1, flags and motor limits describe the host pad.
  caps.type = caps.sub_type = 1;
  caps.gamepad.buttons = 0xf3ff | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0);
  caps.gamepad.left_trigger = caps.gamepad.right_trigger = 255;
  caps.gamepad.thumb_lx = caps.gamepad.thumb_ly = caps.gamepad.thumb_rx = caps.gamepad.thumb_ry = -1;
  if (const auto* pad = FindPad(id)) {
    auto* handle = pad->handle.get();
    uint16_t features = 0;
    if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(handle)) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
      features |= X_INPUT_CAPS_WIRELESS;
    }
    if (SDL_GetBooleanProperty(SDL_GetGamepadProperties(handle), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
      features |= X_INPUT_CAPS_FFB_SUPPORTED;
      caps.vibration.left_motor_speed = caps.vibration.right_motor_speed = 65535;
    }
    caps.flags = features;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT Vibrate(uint32_t user, const X_INPUT_VIBRATION& vibration) {
  std::lock_guard lock(g_mutex);
  if ((user & 0xff) == 0xff) user = 0;
  const auto ids = DevicesFor(user);
  if (ids.empty()) return X_ERROR_DEVICE_NOT_CONNECTED;
  bool physical = false;
  bool sent = false;
  for (const auto id : ids) {
    if (auto* pad = FindPad(id)) {
      physical = true;
      // Xbox motors persist until the next command. SDL accepts a duration
      // and replaces the old effect on each call, including the zero command.
      sent |= SDL_RumbleGamepad(pad->handle.get(), vibration.left_motor_speed,
                                vibration.right_motor_speed, 65535);
    }
  }
  return !physical || sent ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}

}  // namespace

bool Enabled() { return REXCVAR_GET(native_controller_state); }

void Start(std::unique_ptr<kbm::PlayerAssignment> assignment, std::unique_ptr<InputDriver> keyboard,
           std::unique_ptr<InputDriver> scripted) {
  std::lock_guard lock(g_mutex);
  g_assignment = std::move(assignment);
  g_keyboard = std::move(keyboard);
  g_scripted = std::move(scripted);
  g_sdl = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (g_sdl) {
    // Read snapshots directly and avoid queuing input events nobody consumes.
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
    REXLOG_INFO("Native controllers: polling SDL gamepads directly");
  } else {
    REXLOG_WARN("Native controllers: SDL gamepad initialization failed: {}", SDL_GetError());
  }
  g_next_poll = {};
  Refresh();
}

void Stop() {
  std::lock_guard lock(g_mutex);
  g_active_check = {};
  g_devices.clear();
  g_synthetic.clear();
  g_active_devices = {};
  g_assignment.reset();
  g_keyboard.reset();
  g_scripted.reset();
  g_pads.clear();
  if (g_sdl) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  g_sdl = false;
}

void SetActiveCheck(std::function<bool()> check) {
  std::lock_guard lock(g_mutex);
  g_active_check = std::move(check);
}

rex::X_RESULT Read(uint32_t user, X_INPUT_STATE* state) {
  if (!Enabled()) {
    auto* runtime = rex::Runtime::instance();
    auto* input = runtime ? static_cast<InputSystem*>(runtime->input_system()) : nullptr;
    return input ? input->GetState(user, state) : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(g_mutex);
  if ((user & 0xff) == 0xff) user = 0;
  X_INPUT_STATE merged{};
  bool connected = false;
  for (const auto id : DevicesFor(user)) {
    X_INPUT_STATE sample{};
    if (!ReadOne(id, sample)) continue;
    if (HasInput(sample.gamepad)) g_active_devices[user] = id;
    Merge(merged, sample);
    connected = true;
  }
  if (connected && state) *state = merged;
  return connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

bool ReadDevice(DeviceId id, X_INPUT_STATE* state) {
  std::lock_guard lock(g_mutex);
  Refresh();
  X_INPUT_STATE sample{};
  if (!ReadOne(id, sample)) return false;
  if (state) *state = sample;
  return true;
}

bool Writable(uint32_t address, uint32_t size) {
  return GuestAccess(address, size) == memory::PageAccess::kReadWrite;
}

}  // namespace native_controllers

// Capabilities is a tail call with user, flags and a 20-byte output in r3–r5.
extern "C" REX_FUNC(sub_824742E8) {
  using namespace rex;
  using namespace native_controllers;
  if (!Enabled()) {
    __imp__sub_824742E8(ctx, base);
    return;
  }
  const uint32_t address = ctx.r5.u32;
  rex::input::X_INPUT_CAPABILITIES caps{};
  if (!Writable(address, sizeof(caps))) {
    ctx.r3.u64 = X_ERROR_BAD_ARGUMENTS;
    return;
  }
  ctx.r3.u64 = Capabilities(ctx.r3.u32, ctx.r4.u32, caps);
  if (ctx.r3.u32 == X_ERROR_SUCCESS) std::memcpy(GuestPtr(base, address), &caps, sizeof(caps));
}

// The game's vibration wrapper takes user in r3 and two big-endian motor
// speeds in r4. Its special case for an old console wheel does not apply
// to the standard SDL gamepad subtype exposed here.
extern "C" REX_FUNC(sub_82474300) {
  using namespace rex;
  using namespace native_controllers;
  if (!Enabled()) {
    __imp__sub_82474300(ctx, base);
    return;
  }
  const uint32_t address = ctx.r4.u32;
  rex::input::X_INPUT_VIBRATION vibration{};
  if (GuestAccess(address, sizeof(vibration)) == memory::PageAccess::kNoAccess) {
    ctx.r3.u64 = X_ERROR_BAD_ARGUMENTS;
    return;
  }
  std::memcpy(&vibration, GuestPtr(base, address), sizeof(vibration));
  ctx.r3.u64 = Vibrate(ctx.r3.u32, vibration);
}
