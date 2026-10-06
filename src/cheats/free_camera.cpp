// =============================================================================
// cheats/free_camera.cpp -- see free_camera.h
// =============================================================================
#include "free_camera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "../input/keyboard_mouse.h"
#include "../players/coop_camera.h"

namespace free_camera {
namespace {

// Pure3D VectorCamera (findings/27).
constexpr uint32_t kVectorCameraVtable = 0x8200BE84;
constexpr uint32_t kPosition = 216, kDirection = 228, kUp = 240;  // 3 floats each
constexpr uint32_t kMatricesCurrent = 208;                         // byte

constexpr float kPi = 3.14159265f;
constexpr float kMaxPitch = 1.55f;  // just short of straight up / down (89 degrees)

uint8_t* Guest(uint32_t address) {
  return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(address);
}
float ReadFloat(uint32_t address) {
  const uint8_t* p = Guest(address);
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void WriteFloat(uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint8_t* p = Guest(address);
  p[0] = uint8_t(bits >> 24); p[1] = uint8_t(bits >> 16);
  p[2] = uint8_t(bits >> 8); p[3] = uint8_t(bits);
}
uint32_t Read32(uint32_t address) {
  const uint8_t* p = Guest(address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

using Clock = std::chrono::steady_clock;

struct State {
  bool enabled = false;
  bool keys_move = true;
  bool need_start = false;    // take the game camera's view at the next rebuild
  float speed = 10.0f;     // about Crash running
  float look_speed = 1.0f;
  float position[3] = {};
  float yaw = 0.0f, pitch = 0.0f;  // radians; yaw 0 = looking along +z
  Clock::time_point last_move{};
  // Mouse movement not yet turned into yaw / pitch (pixels).
  float mouse_dx = 0.0f, mouse_dy = 0.0f;
  // The game camera's last view (for the menu while we're off).
  float game_position[3] = {}, game_direction[3] = {};
};
std::mutex g_mutex;
State g_state;
std::atomic<bool> g_enabled{false};  // copy of g_state.enabled for the cheap checks
std::atomic<bool> g_flying{false};   // enabled && keys_move

void UpdateFlags() {  // g_mutex held
  g_enabled = g_state.enabled;
  g_flying = g_state.enabled && g_state.keys_move;
}

bool Held(kbm::KeyboardMouseDriver* driver, rex::ui::VirtualKey key) {
  return driver->InputHeld(static_cast<kbm::Input>(key));
}

// The direction for a yaw / pitch (the game: y up, yaw 0 = +z, yaw grows
// toward +x = to the right on screen).
void DirectionOf(float yaw, float pitch, float out[3]) {
  out[0] = std::sin(yaw) * std::cos(pitch);
  out[1] = std::sin(pitch);
  out[2] = std::cos(yaw) * std::cos(pitch);
}

// Moves / turns the camera by the keys held and the mouse moved since the
// last call (g_mutex held). Real time, so it flies at the same speed whatever
// the frame rate or the game speed, and while the game is frozen.
void Move() {
  const Clock::time_point now = Clock::now();
  float dt = g_state.last_move == Clock::time_point{}
                 ? 0.0f
                 : std::chrono::duration<float>(now - g_state.last_move).count();
  g_state.last_move = now;
  dt = std::min(dt, 0.1f);  // a hitch doesn't throw the camera away

  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  using VK = rex::ui::VirtualKey;
  // Looking: mouse with the right button held (0.003 rad per pixel), arrows
  // (1.5 rad/s).
  float turn_x = 0.0f, turn_y = 0.0f;
  if (g_state.keys_move && driver) {
    if (Held(driver, VK::kRButton)) {
      turn_x += g_state.mouse_dx * 0.003f;
      turn_y -= g_state.mouse_dy * 0.003f;
    }
    const float arrows = 1.5f * dt;
    if (Held(driver, VK::kLeft)) turn_x -= arrows;
    if (Held(driver, VK::kRight)) turn_x += arrows;
    if (Held(driver, VK::kUp)) turn_y += arrows;
    if (Held(driver, VK::kDown)) turn_y -= arrows;
  }
  g_state.mouse_dx = g_state.mouse_dy = 0.0f;
  g_state.yaw += turn_x * g_state.look_speed;
  g_state.pitch = std::clamp(g_state.pitch + turn_y * g_state.look_speed, -kMaxPitch, kMaxPitch);
  if (g_state.yaw > kPi) g_state.yaw -= 2.0f * kPi;
  if (g_state.yaw < -kPi) g_state.yaw += 2.0f * kPi;

  if (!g_state.keys_move || !driver || dt <= 0.0f) return;
  // Flying: forward = where we look, right = level with the ground, up = world up.
  float forward[3];
  DirectionOf(g_state.yaw, g_state.pitch, forward);
  const float right[3] = {std::cos(g_state.yaw), 0.0f, -std::sin(g_state.yaw)};
  float move[3] = {};
  auto add = [&](const float v[3], float amount) {
    for (int i = 0; i < 3; ++i) move[i] += v[i] * amount;
  };
  const float world_up[3] = {0.0f, 1.0f, 0.0f};
  if (Held(driver, VK::kW)) add(forward, 1.0f);
  if (Held(driver, VK::kS)) add(forward, -1.0f);
  if (Held(driver, VK::kD)) add(right, 1.0f);
  if (Held(driver, VK::kA)) add(right, -1.0f);
  if (Held(driver, VK::kE)) add(world_up, 1.0f);
  if (Held(driver, VK::kQ)) add(world_up, -1.0f);
  float speed = g_state.speed;
  if (Held(driver, VK::kShift) || Held(driver, VK::kLShift) || Held(driver, VK::kRShift)) speed *= 4.0f;
  if (Held(driver, VK::kControl) || Held(driver, VK::kLControl) || Held(driver, VK::kRControl)) {
    speed *= 0.25f;
  }
  for (int i = 0; i < 3; ++i) g_state.position[i] += move[i] * speed * dt;
}

// Mouse movement of the main window -> g_state (UI thread). Never consumes
// the event: the keyboard driver and ImGui still see it.
class MouseListener final : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (have_last_ && g_state.enabled) {
      g_state.mouse_dx += float(e.x() - last_x_);
      g_state.mouse_dy += float(e.y() - last_y_);
    }
    last_x_ = e.x();
    last_y_ = e.y();
    have_last_ = true;
  }

 private:
  int32_t last_x_ = 0, last_y_ = 0;
  bool have_last_ = false;
};
MouseListener g_mouse_listener;

}  // namespace

// --- API ----------------------------------------------------------------------

void SetEnabled(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (on && !g_state.enabled) g_state.need_start = true;
  g_state.enabled = on;
  g_state.last_move = Clock::time_point{};
  UpdateFlags();
  REXLOG_INFO("Cheats: free camera {}", on ? "on" : "off");
}
bool Enabled() { return g_enabled.load(); }
void SetKeysMoveCamera(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.keys_move = on;
  UpdateFlags();
}
bool KeysMoveCamera() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.keys_move;
}
bool Flying() { return g_flying.load(); }
void SetSpeed(float units_per_second) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.speed = std::clamp(units_per_second, 1.0f, 200.0f);
}
float Speed() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.speed;
}
void SetLookSpeed(float scale) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.look_speed = std::clamp(scale, 0.1f, 4.0f);
}
float LookSpeed() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.look_speed;
}
void ResetToGameCamera() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.need_start = true;
}
Pose CurrentPose() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pose pose;
  if (g_state.enabled) {
    std::copy(g_state.position, g_state.position + 3, pose.position);
    DirectionOf(g_state.yaw, g_state.pitch, pose.direction);
  } else {
    std::copy(g_state.game_position, g_state.game_position + 3, pose.position);
    std::copy(g_state.game_direction, g_state.game_direction + 3, pose.direction);
  }
  return pose;
}
Pose GameCameraPose() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pose pose;
  std::copy(g_state.game_position, g_state.game_position + 3, pose.position);
  std::copy(g_state.game_direction, g_state.game_direction + 3, pose.direction);
  return pose;
}
void AttachWindow(rex::ui::Window* window) {
  if (window) window->AddInputListener(&g_mouse_listener, 0);
}

}  // namespace free_camera

// -----------------------------------------------------------------------------
// The wrapped game functions (strong definitions of the generated weak ones)
// -----------------------------------------------------------------------------

// VectorCamera: rebuild the matrices from position / direction / up (r3 =
// camera, slot 26). Ours go in first while the free camera is on.
extern "C" REX_FUNC(__imp__sub_82373D18);
extern "C" REX_FUNC(sub_82373D18) {
  using namespace free_camera;
  const uint32_t camera = ctx.r3.u32;
  if (!g_enabled.load()) {
    coop_camera::OnGameCameraRebuilt(camera);
    __imp__sub_82373D18(ctx, base);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < 3; ++i) {
      g_state.game_position[i] = ReadFloat(camera + kPosition + 4 * i);
      g_state.game_direction[i] = ReadFloat(camera + kDirection + 4 * i);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < 3; ++i) {  // the game's own view, before ours goes in
      g_state.game_position[i] = ReadFloat(camera + kPosition + 4 * i);
      g_state.game_direction[i] = ReadFloat(camera + kDirection + 4 * i);
    }
    if (g_state.need_start) {
      // Start (or reset) where the game's camera is.
      float d[3];
      for (int i = 0; i < 3; ++i) {
        g_state.position[i] = ReadFloat(camera + kPosition + 4 * i);
        d[i] = ReadFloat(camera + kDirection + 4 * i);
      }
      g_state.yaw = std::atan2(d[0], d[2]);
      g_state.pitch = std::clamp(std::asin(std::clamp(d[1], -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
      g_state.need_start = false;
      g_state.last_move = Clock::time_point{};
    }
    Move();
    float d[3];
    DirectionOf(g_state.yaw, g_state.pitch, d);
    for (int i = 0; i < 3; ++i) {
      WriteFloat(camera + kPosition + 4 * i, g_state.position[i]);
      WriteFloat(camera + kDirection + 4 * i, d[i]);
      WriteFloat(camera + kUp + 4 * i, i == 1 ? 1.0f : 0.0f);
    }
  }
  __imp__sub_82373D18(ctx, base);
}

// The camera's getters (Camera vtable slots 13 world-to-camera, 14
// camera-to-world, 17 / 18 transforms, 20 position, 22 set up the view, 23 a
// projection helper): each rebuilds the matrices when +208 is 0. While the
// free camera is on we clear it first, so they always see our latest view.
#define CRASHMOM_CAMERA_GETTER(hex)                                                    \
  extern "C" REX_FUNC(__imp__sub_##hex);                                                \
  extern "C" REX_FUNC(sub_##hex) {                                                      \
    using namespace free_camera;                                                        \
    if (g_enabled.load() && Read32(ctx.r3.u32) == kVectorCameraVtable) {                \
      *Guest(ctx.r3.u32 + kMatricesCurrent) = 0;                                        \
    }                                                                                   \
    __imp__sub_##hex(ctx, base);                                                        \
  }
CRASHMOM_CAMERA_GETTER(823817D8)
CRASHMOM_CAMERA_GETTER(82381820)
CRASHMOM_CAMERA_GETTER(82381940)
CRASHMOM_CAMERA_GETTER(823819B8)
CRASHMOM_CAMERA_GETTER(82381AD0)
CRASHMOM_CAMERA_GETTER(82381B38)
CRASHMOM_CAMERA_GETTER(82381D18)
#undef CRASHMOM_CAMERA_GETTER
