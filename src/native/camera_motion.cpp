// =============================================================================
// native/camera_motion.cpp -- see camera_motion.h
// =============================================================================
#include "camera_motion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../players/more_players.h"
#include "guest.h"

namespace native {
namespace camera_motion {
namespace {

using Matrix = std::array<float, 16>;  // column-major: element (row r, column c) at [c * 4 + r]
using Clock = std::chrono::steady_clock;

// a x b (both column-major).
Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
  return out;
}

// The inverse (cofactors / determinant), false if the matrix has none.
bool Invert(const Matrix& m, Matrix& out) {
  Matrix o;
  o[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  o[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  o[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  o[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  o[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  o[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  o[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  o[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  o[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  o[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  o[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  o[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  o[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  o[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  o[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  o[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * o[0] + m[1] * o[4] + m[2] * o[8] + m[3] * o[12];
  if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) return false;
  for (int i = 0; i < 16; ++i) out[i] = o[i] / det;
  return true;
}

// M x (x, y, z, 1), divided by w: false if w is ~0 or behind.
bool Project(const Matrix& m, float x, float y, float z, float out[3]) {
  float v[4];
  for (int r = 0; r < 4; ++r) v[r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
  if (!(v[3] > 1e-6f)) return false;
  for (int r = 0; r < 3; ++r) out[r] = v[r] / v[3];
  return true;
}

bool Same(const Matrix& a, const Matrix& b) {
  for (int i = 0; i < 16; ++i) {
    if (std::fabs(a[i] - b[i]) > 1e-3f * (1.0f + std::fabs(a[i]))) return false;
  }
  return true;
}

// The vote: each distinct view-projection seen this frame (they're few: the
// main camera, the reflection's, sometimes a cutscene's second view) and how
// many draws used it.
struct Candidate {
  Matrix vp;
  uint32_t draws = 0;
};
constexpr size_t kMaxCandidates = 8;

struct State {
  std::array<Candidate, kMaxCandidates> candidates;
  size_t count = 0;
  bool decided = false;  // AtPostPass ran this frame
  // This frame's winner (valid once decided) and last frame's.
  bool have_current = false, have_previous = false;
  Matrix current{}, previous{};
  // Real frame length, smoothed (seconds), and when the last post pass ran.
  Clock::time_point last_pass{};
  float frame_seconds = 1.0f / 30.0f;
} g;

// The setting's share of 1/30 s (one original frame) the shutter stays open:
// Low, Medium, High. High = the whole movement of an original frame.
constexpr float kShutter[4] = {0.0f, 0.35f, 0.7f, 1.0f};

// A frame whose camera jumps further than this (screen widths, D3D clip
// units / 2) at any of the test points is a CUT (a cutscene changing shots,
// a respawn, a teleport): no blur, it would smear the whole picture.
constexpr float kCutDistance = 0.3f;

// Player bodies, a CAPSULE each (a vertical line with a radius: a standing
// shape): where its line starts above the actor's position (its feet), how
// long it is and the radius. Fully sharp within 3/4 of the radius, fading
// out to the radius (shaders/dof.frag). Crash is about 1.7 units tall (his
// ears smeared with a 1-unit ball, 2026-10-11); titans 3-4.
struct Body {
  float bottom, length, radius;
};
constexpr Body kCrash = {0.5f, 0.8f, 0.75f};
constexpr Body kTitan = {1.4f, 2.2f, 2.2f};

}  // namespace

void NoteDraw(const float mvp[16], const float world[16]) {
  if (g.decided) return;  // after the post pass: HUD / overlays, not the scene
  Matrix m, w, w_inverse;
  std::memcpy(m.data(), mvp, sizeof(m));
  std::memcpy(w.data(), world, sizeof(w));
  if (!Invert(w, w_inverse)) return;
  const Matrix vp = Multiply(m, w_inverse);
  for (size_t i = 0; i < g.count; ++i) {
    if (Same(g.candidates[i].vp, vp)) {
      ++g.candidates[i].draws;
      return;
    }
  }
  if (g.count < kMaxCandidates) {
    g.candidates[g.count++] = {vp, 1};
  }
}

Motion AtPostPass(const uint8_t* base, int strength) {
  Motion out;
  if (g.decided) return out;  // a second pass this frame: once is enough
  g.decided = true;

  // This frame's camera = the matrix most draws used (at least 3 of them).
  const Candidate* best = nullptr;
  for (size_t i = 0; i < g.count; ++i) {
    if (!best || g.candidates[i].draws > best->draws) best = &g.candidates[i];
  }
  g.have_current = best && best->draws >= 3;
  if (g.have_current) g.current = best->vp;

  // How long this frame was (for the shutter): the time since the last post
  // pass, smoothed so a single slow frame doesn't flash a longer smear.
  const Clock::time_point now = Clock::now();
  const float seconds =
      g.last_pass.time_since_epoch().count() ? std::chrono::duration<float>(now - g.last_pass).count() : 0.0f;
  g.last_pass = now;
  const bool hitch = !(seconds > 0.0f) || seconds > 0.2f;  // a load / pause of the whole game
  if (!hitch) g.frame_seconds += (std::clamp(seconds, 1.0f / 1000, 1.0f / 15) - g.frame_seconds) * 0.2f;

  strength = std::clamp(strength, 0, 3);
  if (!strength || hitch || !g.have_current || !g.have_previous) return out;

  Matrix unproject;
  if (!Invert(g.current, unproject)) return out;
  const Matrix reproject = Multiply(g.previous, unproject);

  // Camera cut? Five test points (centre, corners) at two depths: a distance
  // of ~10 units and the far plane. (Clip z here is D3D's 0 near .. 1 far.)
  static constexpr float kTest[5][2] = {{0, 0}, {-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, 0.8f}};
  for (const auto& t : kTest) {
    for (float z : {0.95f, 0.9999f}) {
      float p[3];
      if (!Project(reproject, t[0], t[1], z, p)) return out;
      if (std::fabs(p[0] - t[0]) * 0.5f > kCutDistance || std::fabs(p[1] - t[1]) * 0.5f > kCutDistance) {
        return out;
      }
    }
  }

  out.valid = true;
  std::memcpy(out.reproject, reproject.data(), sizeof(out.reproject));
  std::memcpy(out.unproject, unproject.data(), sizeof(out.unproject));
  out.scale = kShutter[strength] * (1.0f / 30.0f) / g.frame_seconds;

  // The players' bodies (the co-op camera reads them the same way): the
  // titan they ride, else their Crash; the actor's world matrix (actor +44)
  // holds the position at +48.
  if (more_players::InPlay()) {
    for (int p = 0; p < 4 && out.spheres < 4; ++p) {
      const uint32_t titan = more_players::TitanOfPlayer(p);
      const uint32_t actor = titan ? titan : more_players::CharacterOfPlayer(p);
      if (!actor) continue;
      const uint32_t matrix = guest::Load32(base, actor + 44);
      if (matrix < 0x40000000) continue;  // not a heap pointer: skip it
      float pos[3];
      for (int i = 0; i < 3; ++i) pos[i] = guest::LoadFloat(base, matrix + 48 + 4 * i);
      if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) continue;
      const Body& body = titan ? kTitan : kCrash;
      float* s = out.sphere[out.spheres];
      s[0] = pos[0];
      s[1] = pos[1] + body.bottom;  // the game's y is up
      s[2] = pos[2];
      s[3] = body.radius;
      out.height[out.spheres++] = body.length;
    }
  }
  return out;
}

void EndFrame(bool recorded) {
  // A frame without a post pass (loading screens draw no depth of field)
  // decides here, so its camera still counts as "last frame's".
  if (!g.decided) {
    const Candidate* best = nullptr;
    for (size_t i = 0; i < g.count; ++i) {
      if (!best || g.candidates[i].draws > best->draws) best = &g.candidates[i];
    }
    g.have_current = best && best->draws >= 3;
    if (g.have_current) g.current = best->vp;
  }
  g.have_previous = recorded && g.have_current;
  g.previous = g.current;
  g.have_current = false;
  g.count = 0;
  g.decided = false;
}

}  // namespace camera_motion
}  // namespace native
