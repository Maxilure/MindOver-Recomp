// =============================================================================
// players/dummy_players.cpp -- see dummy_players.h
// =============================================================================
#include "dummy_players.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../language/texts.h"
#include "../debug_input_script.h"
#include "../input/players.h"
#include "../input_record.h"
#include "../pddi/intercept.h"
#include "more_players.h"

namespace dummy_players {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kGameGlobal = 0x8259B190;   // the game ("uber")
constexpr uint32_t kPlayerStates = 0x8259B11C;  // u32 per player: 1 mask, 2 on foot, 3 turning into a mask
// The game camera (findings/27): *(*(uber + 76) + 444) = the VectorCamera,
// direction at +228.
constexpr uint32_t kCameraHolder = 76, kCameraOffset = 444, kCamDirection = 228;

// FOLLOW: stop within kStopUnits, full tilt beyond kFullUnits, and never less
// than kMinTilt (the game ignores a stick under ~0.65: input/keyboard_mouse
// walk_tilt). Stuck = moved less than kStuckUnits in kStuckSeconds while still
// far: a jump if the leader is higher (a ledge), at most every kJumpGap s.
constexpr float kStopUnits = 3.0f, kFullUnits = 7.0f, kMinTilt = 0.75f;
constexpr float kStuckUnits = 0.6f, kStuckSeconds = 0.6f, kJumpGap = 1.0f, kJumpHigher = 1.0f;

struct Dummy {
  std::atomic<int> mode{int(Mode::kIdle)};
  std::atomic<int> target{0};
  std::atomic<float> held_x{0.0f}, held_y{0.0f};
  // Follow's stuck check (game thread).
  float check_x = 0, check_z = 0;
  Clock::time_point check_since{};
  Clock::time_point last_jump{};
};
Dummy g_dummies[kMaxDummies];

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float ReadFloat(uint32_t a) {
  const uint32_t b = Read32(a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// Player p's body position (their titan if they ride one, else their Crash).
bool PositionOf(int p, float out[3]) {
  const uint32_t t = more_players::TitanOfPlayer(p);
  const uint32_t actor = t ? t : more_players::CharacterOfPlayer(p);
  if (!actor) return false;
  const uint32_t matrix = Read32(actor + 44);  // world matrix, position at +48
  if (matrix < 0x40000000) return false;
  for (int i = 0; i < 3; ++i) out[i] = ReadFloat(matrix + 48 + 4 * i);
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// The game camera's direction, flattened (x, z), normalised. False if none.
bool CameraForward(float& fx, float& fz) {
  const uint32_t uber = Read32(kGameGlobal);
  const uint32_t holder = uber ? Read32(uber + kCameraHolder) : 0;
  const uint32_t camera = holder >= 0x40000000 ? Read32(holder + kCameraOffset) : 0;
  if (camera < 0x40000000) return false;
  fx = ReadFloat(camera + kCamDirection);
  fz = ReadFloat(camera + kCamDirection + 8);
  const float l = std::sqrt(fx * fx + fz * fz);
  if (!(l > 1e-3f) || !std::isfinite(l)) return false;
  fx /= l;
  fz /= l;
  return true;
}

int16_t ToAxis(float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

// FOLLOW for dummy d (player me) after `target`: the left stick and a jump.
void Follow(Dummy& d, int me, int target, ScriptedInputDriver::LiveState& out) {
  if (target == me || Read32(kPlayerStates + 4 * me) != 2) return;  // on foot only
  const uint32_t target_state = Read32(kPlayerStates + 4 * target);
  if (target_state != 2 && target_state != 1) return;  // nobody to follow
  float a[3], b[3], fx, fz;
  if (!PositionOf(me, a) || !PositionOf(target, b) || !CameraForward(fx, fz)) return;
  const float dx = b[0] - a[0], dz = b[2] - a[2];
  const float distance = std::sqrt(dx * dx + dz * dz);
  const Clock::time_point now = Clock::now();
  if (distance < kStopUnits) {
    d.check_since = {};
    return;
  }
  // Stick in the camera's frame: up = away from the camera (its forward),
  // right = screen right = up x forward = (fz, -fx) (coop_camera.cpp's rule).
  // The game reads the left stick relative to the camera, like a person
  // pushing it.
  const float rx = fz, rz = -fx;
  const float ux = dx / distance, uz = dz / distance;
  float sx = ux * rx + uz * rz, sy = ux * fx + uz * fz;
  const float tilt = std::clamp((distance - kStopUnits) / (kFullUnits - kStopUnits), kMinTilt, 1.0f);
  const float len = std::sqrt(sx * sx + sy * sy);
  if (len > 1e-3f) {
    sx = sx / len * tilt;
    sy = sy / len * tilt;
  }
  out.lx = ToAxis(sx);
  out.ly = ToAxis(sy);
  // Stuck below a ledge: barely moved for a while, and the leader is higher.
  if (d.check_since == Clock::time_point{}) {
    d.check_since = now;
    d.check_x = a[0];
    d.check_z = a[2];
  } else if (std::chrono::duration<float>(now - d.check_since).count() > kStuckSeconds) {
    const float moved = std::hypot(a[0] - d.check_x, a[2] - d.check_z);
    if (moved < kStuckUnits && b[1] > a[1] + kJumpHigher &&
        std::chrono::duration<float>(now - d.last_jump).count() > kJumpGap) {
      out.buttons |= 0x1000;  // A (this frame; the taps keep it held long enough below)
      d.last_jump = now;
    }
    d.check_since = now;
    d.check_x = a[0];
    d.check_z = a[2];
  }
}

// COPY for dummy d: `target`'s controller as the game read it, minus START / BACK.
void Copy(int target, ScriptedInputDriver::LiveState& out) {
  uint8_t g[12];
  if (!input_record::LastRead(uint32_t(target), g)) return;
  out.buttons = uint16_t((g[0] << 8 | g[1]) & ~(0x0010 | 0x0020));
  out.lt = g[2];
  out.rt = g[3];
  out.lx = int16_t(g[4] << 8 | g[5]);
  out.ly = int16_t(g[6] << 8 | g[7]);
  out.rx = int16_t(g[8] << 8 | g[9]);
  out.ry = int16_t(g[10] << 8 | g[11]);
}

void OnFrameEnd(void*) {
  ScriptedInputDriver* driver = ScriptedInputDriver::Live();
  if (!driver) return;
  const int count = std::min(kMaxDummies, driver->PadCount() - 1);
  for (int i = 0; i < count; ++i) {
    Dummy& d = g_dummies[i];
    ScriptedInputDriver::LiveState out;
    const int me = PlayerOf(i);
    const Mode mode = Mode(d.mode.load());
    const int target = d.target.load();
    const bool playing = more_players::InPlay() && !more_players::Loading();
    if (me >= 0 && playing && target >= 0 && target < 4) {
      if (mode == Mode::kFollow) Follow(d, me, target, out);
      if (mode == Mode::kCopy && target != me) Copy(target, out);
    }
    // The menu's stick pad wins while held.
    const float hx = d.held_x.load(), hy = d.held_y.load();
    if (hx != 0.0f || hy != 0.0f) {
      out.lx = ToAxis(hx);
      out.ly = ToAxis(hy);
    }
    // A follow jump: held for this frame only would be too short at high
    // frame rates; tap it through the driver's timeline instead.
    if (out.buttons & 0x1000 && mode == Mode::kFollow) {
      out.buttons &= uint16_t(~0x1000);
      Tap(i, "a", 150);
    }
    driver->SetLiveState(i + 1, out);
  }
}

}  // namespace

int Count() {
  ScriptedInputDriver* driver = ScriptedInputDriver::Live();
  return driver ? std::min(kMaxDummies, driver->PadCount() - 1) : 0;
}

int PlayerOf(int d) {
  kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  if (!assignment || d < 0 || d >= kMaxDummies) return -1;
  const std::string key = "debug-pad-" + std::to_string(d + 2);  // (fake pads: the bare guid, players.cpp)
  for (const auto& device : assignment->Devices()) {
    if (device.key == key) return device.player;
  }
  return -1;
}

void SetMode(int d, Mode mode, int target) {
  if (d < 0 || d >= kMaxDummies) return;
  g_dummies[d].mode = int(mode);
  g_dummies[d].target = std::clamp(target, 0, 3);
  g_dummies[d].check_since = {};
  REXLOG_INFO("Dummy players: dummy controller {} -> {}{}", d + 2,
              mode == Mode::kIdle ? "idle" : mode == Mode::kFollow ? "follow player " : "copy player ",
              mode == Mode::kIdle ? std::string() : std::to_string(target + 1));
}
Mode GetMode(int d) { return d >= 0 && d < kMaxDummies ? Mode(g_dummies[d].mode.load()) : Mode::kIdle; }
int GetTarget(int d) { return d >= 0 && d < kMaxDummies ? g_dummies[d].target.load() : 0; }

void SetHeldStick(int d, float x, float y) {
  if (d < 0 || d >= kMaxDummies) return;
  g_dummies[d].held_x = x;
  g_dummies[d].held_y = y;
}

bool Tap(int d, const std::string& input, int hold_ms) {
  ScriptedInputDriver* driver = ScriptedInputDriver::Live();
  if (!driver || d < 0 || d >= Count()) return false;
  return driver->HandleLine("p" + std::to_string(d + 2) + "." + input + " " + std::to_string(hold_ms));
}

// Models (SDL GUIDs: bytes 0-1 bus 0x0003 USB, 4-5 vendor, 8-9 product,
// little-endian; the detector reads vendor + product, prompt_icons.cpp
// StyleOfPad). The ids are the makers' real USB ones.
struct Model {
  const char* name;
  const char* guid;  // "" = none
};
constexpr Model kModels[] = {
    {trsn("Test pad (no model)"), ""},
    {trsn("Xbox 360 controller"), "030000005e0400008e02000000000000"},       // 045E:028E
    {trsn("Xbox Series controller"), "030000005e040000120b000000000000"},    // 045E:0B12
    {trsn("PlayStation 4 (DualShock 4)"), "030000004c050000cc09000000000000"},  // 054C:09CC
    {trsn("PlayStation 5 (DualSense)"), "030000004c050000e60c000000000000"},    // 054C:0CE6
    {trsn("Switch Pro controller"), "030000007e0500000920000000000000"},       // 057E:2009
    {trsn("Another maker (8BitDo)"), "03000000c82d00000160000000000000"},      // 2DC8:6001
};
std::atomic<int> g_models[kMaxDummies] = {};

int ModelCount() { return int(std::size(kModels)); }
const char* ModelName(int model) { return model >= 0 && model < ModelCount() ? kModels[model].name : "?"; }
void SetModel(int d, int model) {
  if (d < 0 || d >= kMaxDummies || model < 0 || model >= ModelCount()) return;
  g_models[d] = model;
  REXLOG_INFO("Dummy players: dummy controller {} looks like: {}", d + 2, kModels[model].name);
}
int GetModel(int d) { return d >= 0 && d < kMaxDummies ? g_models[d].load() : 0; }
std::string DetectorKey(int d) {
  const int model = GetModel(d);
  return model > 0 ? std::string("pad:") + kModels[model].guid : std::string();
}

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

}  // namespace dummy_players
