// =============================================================================
// native/frame_blend.cpp -- pictures in between two logic frames (frame_blend.h)
// =============================================================================
#include "frame_blend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(debug_frame_blend_trace, false, "CrashMoM",
                    "Log why draws aren't blended between logic frames (frame_blend.h)");

namespace native::frame_blend {
namespace {

// A draw's matrices changed by more than this share of their size between
// A and B: a cut for that draw (drawn as in B). A camera turning fast moves
// its matrix ~0.1-0.2 per 1/30 s; a camera cut changes it completely.
constexpr float kSnapMatrix = 0.5f;
// More than this share of the matched 3D draws snapped: the whole frame is a cut.
constexpr float kCutShare = 0.6f;
// An immediate draw whose vertex moved further than this (game units; world
// units for the skinned characters: Crash runs ~0.3 per 1/30 s): drawn as in B.
constexpr float kSnapUnits = 4.0f;
// A follower (frame_blend.h FOLLOWERS) follows a body whose centre is this
// close to its own (a shadow volume reaches down from the body).
constexpr float kFollowUnits = 2.5f;
// A particle pairs with A's nearest particle within this (game units).
constexpr float kParticleUnits = 1.0f;
constexpr uint32_t kParticleVertices = 6;  // two triangles per particle

// What identifies "the same object" in two frames (frame_blend.h MATCHING).
struct Key {
  uint8_t material, topology, mesh, stride;
  uint32_t vertex_buffer, index_buffer, vertex_count, index_count, texture, colour_target;
  bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(Key) == 28);
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(k); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return size_t(h);
  }
};

Key KeyOf(const DrawCommand& d) {
  Key k{};
  k.material = uint8_t(d.material);
  k.topology = uint8_t(d.topology);
  k.mesh = d.mesh;
  k.stride = d.layout.stride;
  // (Not the meshes' buffer ADDRESSES: some meshes are rewritten into new
  // buffers every frame and never paired, feel test 3. Copies are told apart
  // by the closest matrix, Pair.)
  k.vertex_buffer = 0;
  k.index_buffer = 0;
  k.vertex_count = d.vertex_count;
  k.index_count = d.index_count;
  k.texture = d.texture.fetch[1];
  k.colour_target = d.targets.colour;
  return k;
}

// How many floats at the start of a draw's material block are matrices /
// positions that move with the object or camera (frame.h): mvp, world,
// normal rotation, camera position. 0 = leave the block as B's.
size_t MovingBlockFloats(const DrawCommand& d) {
  if (d.block == DrawCommand::kNoBlock) return 0;
  switch (d.material) {
    case Material::kSimple:     // LitParams (proximity lights / Fx shadows on)
    case Material::kLitSimple:
    case Material::kCharacter:
    case Material::kReflect:    // LitParams too
      return 16 * 3 + 4;        // mvp, world, rotation, eye
    case Material::kBumpMega:
      return 16 * 3 + 4;        // mvp, world, rotation, camera
    case Material::kWater:
      return 16 * 2 + 4;        // mvp, world, camera
    case Material::kShadow:
    case Material::kParticle:
      return 16;                // mvp
    default:
      return 0;                 // depth of field: a full-screen quad
  }
}

// Relative size of the change between two matrices.
float MatrixChange(const float* a, const float* b) {
  float diff = 0, size = 0;
  for (int i = 0; i < 16; ++i) {
    diff += std::fabs(a[i] - b[i]);
    size += std::fabs(b[i]);
  }
  return size > 0 ? diff / size : (diff > 0 ? 1.0f : 0.0f);
}

void Lerp(float* out, const float* a, const float* b, size_t n, float t) {
  for (size_t i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

// The immediate draw's vertices as floats (host order words, frame.h).
const float* VertexFloats(const Frame& f, const DrawCommand& d) {
  return reinterpret_cast<const float*>(f.vertex_data.data()) + d.vertex_offset / 4;
}

bool VerticesInRange(const Frame& f, const DrawCommand& d) {
  return d.layout.stride != 0 && d.layout.stride % 4 == 0 &&
         size_t(d.vertex_offset) + size_t(d.vertex_count) * d.layout.stride <= f.vertex_data.size() * 4;
}

// Did any vertex move further than kSnapUnits?
// A FLAT draw (menus, HUD, texts, full-screen passes): its matrix has no
// perspective (the w row is 0 0 0 1; column-major: elements 3, 7, 11, 15).
// The menus' glass panels draw WITH the depth test, so "no depth test" alone
// missed them: the pause menu's glass and the Options glass paired up and
// slid into each other (feel test 2).
bool IsFlat(const DrawCommand& d) {
  return d.mvp[3] == 0.0f && d.mvp[7] == 0.0f && d.mvp[11] == 0.0f && d.mvp[15] == 1.0f;
}

// 4x4 column-major helpers (THE OLDER MATRIX).
bool Invert(const float* m, double* out) {
  double a[16];
  for (int i = 0; i < 16; ++i) a[i] = m[i];
  double inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (!(std::fabs(det) > 1e-20)) return false;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
  return true;
}
// out = x * y (column-major: element [col*4+row]).
template <typename Y>
void Multiply(const double* x, const Y* y, double* out) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += x[k * 4 + r] * double(y[c * 4 + k]);
      out[c * 4 + r] = sum;
    }
  }
}

// THE CAMERA, TAKEN APART (frame_blend.h). A camera matrix (column-major,
// projection x view) -> lens, position, rotation; false if it isn't a plain
// perspective camera (checked by putting it back together).
bool SplitCamera(const float* m, Pairing::CameraParts& c) {
  auto row = [&](int r, double v[4]) {
    for (int k = 0; k < 4; ++k) v[k] = m[k * 4 + r];
  };
  double r0[4], r1[4], r2[4], r3[4];
  row(0, r0); row(1, r1); row(2, r2); row(3, r3);
  // Row 3 (w) = n x forward: the view's forward axis and the scale.
  c.n = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
  if (!(c.n > 1e-8)) return false;
  double f[3] = {r3[0] / c.n, r3[1] / c.n, r3[2] / c.n};
  auto split = [&](const double* r, double& scale, double& centre, double axis[3]) {
    centre = r[0] * f[0] + r[1] * f[1] + r[2] * f[2];
    double rest[3] = {r[0] - centre * f[0], r[1] - centre * f[1], r[2] - centre * f[2]};
    scale = std::sqrt(rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2]);
    if (!(scale > 1e-8)) return false;
    for (int k = 0; k < 3; ++k) axis[k] = rest[k] / scale;
    return true;
  };
  double right[3], up[3];
  if (!split(r0, c.sx, c.c0, right) || !split(r1, c.sy, c.c1, up)) return false;
  if (std::fabs(right[0] * up[0] + right[1] * up[1] + right[2] * up[2]) > 1e-3) return false;
  c.a = r2[0] * f[0] + r2[1] * f[1] + r2[2] * f[2];
  // Translations: tz from w, then tx, ty, and the depth offset b.
  const double tz = r3[3] / c.n;
  const double tx = (r0[3] - c.c0 * tz) / c.sx;
  const double ty = (r1[3] - c.c1 * tz) / c.sy;
  c.b = r2[3] - c.a * tz;
  for (int k = 0; k < 3; ++k) c.eye[k] = -(tx * right[k] + ty * up[k] + tz * f[k]);
  // Rotation rows right / up / forward -> quaternion (M[row][col]). The
  // three may form a left-handed set (the game's camera convention): keep
  // the sign in `n`'s partner and quaternion-ise a proper rotation.
  const double cross[3] = {right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
                           right[0] * up[1] - right[1] * up[0]};
  const double hand = cross[0] * f[0] + cross[1] * f[1] + cross[2] * f[2];
  if (std::fabs(std::fabs(hand) - 1) > 1e-3) return false;
  if (hand < 0) {  // flip forward into a right-handed set; fold the sign into the lens
    for (double& v : f) v = -v;
    c.n = -c.n;
    c.c0 = -c.c0;
    c.c1 = -c.c1;
    c.a = -c.a;
  }
  const double M[3][3] = {{right[0], right[1], right[2]}, {up[0], up[1], up[2]}, {f[0], f[1], f[2]}};
  const double tr = M[0][0] + M[1][1] + M[2][2];
  double x, y, z, w;
  if (tr > 0) {
    const double s = std::sqrt(tr + 1.0) * 2;
    w = 0.25 * s; x = (M[2][1] - M[1][2]) / s; y = (M[0][2] - M[2][0]) / s; z = (M[1][0] - M[0][1]) / s;
  } else if (M[0][0] > M[1][1] && M[0][0] > M[2][2]) {
    const double s = std::sqrt(1.0 + M[0][0] - M[1][1] - M[2][2]) * 2;
    w = (M[2][1] - M[1][2]) / s; x = 0.25 * s; y = (M[0][1] + M[1][0]) / s; z = (M[0][2] + M[2][0]) / s;
  } else if (M[1][1] > M[2][2]) {
    const double s = std::sqrt(1.0 + M[1][1] - M[0][0] - M[2][2]) * 2;
    w = (M[0][2] - M[2][0]) / s; x = (M[0][1] + M[1][0]) / s; y = 0.25 * s; z = (M[1][2] + M[2][1]) / s;
  } else {
    const double s = std::sqrt(1.0 + M[2][2] - M[0][0] - M[1][1]) * 2;
    w = (M[1][0] - M[0][1]) / s; x = (M[0][2] + M[2][0]) / s; y = (M[1][2] + M[2][1]) / s; z = 0.25 * s;
  }
  const double len = std::sqrt(x * x + y * y + z * z + w * w);
  c.q[0] = x / len; c.q[1] = y / len; c.q[2] = z / len; c.q[3] = w / len;
  // Check: put it back together, compare.
  double back[16];
  void JoinCamera(const Pairing::CameraParts&, const Pairing::CameraParts&, double, double*);
  JoinCamera(c, c, 0.0, back);
  double err = 0, size = 0;
  for (int k = 0; k < 16; ++k) err += std::fabs(back[k] - m[k]), size += std::fabs(m[k]);
  return err <= 1e-4 * size;
}

// The camera at blend t between A's and B's parts (column-major).
void JoinCamera(const Pairing::CameraParts& pa, const Pairing::CameraParts& pb, double t, double* out) {
  auto mix = [t](double x, double y) { return x + (y - x) * t; };
  // Rotation: the shortest arc.
  double qa[4] = {pa.q[0], pa.q[1], pa.q[2], pa.q[3]};
  double dot = qa[0] * pb.q[0] + qa[1] * pb.q[1] + qa[2] * pb.q[2] + qa[3] * pb.q[3];
  if (dot < 0) {
    for (double& v : qa) v = -v;
    dot = -dot;
  }
  double q[4];
  if (dot > 0.9995) {
    for (int k = 0; k < 4; ++k) q[k] = mix(qa[k], pb.q[k]);
  } else {
    const double angle = std::acos(std::min(dot, 1.0));
    const double wa = std::sin((1 - t) * angle) / std::sin(angle), wb = std::sin(t * angle) / std::sin(angle);
    for (int k = 0; k < 4; ++k) q[k] = qa[k] * wa + pb.q[k] * wb;
  }
  const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  const double x = q[0] / len, y = q[1] / len, z = q[2] / len, w = q[3] / len;
  const double M[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                          {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                          {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
  double eye[3];
  for (int k = 0; k < 3; ++k) eye[k] = mix(pa.eye[k], pb.eye[k]);
  double tv[3];  // view translation = -M x eye
  for (int r = 0; r < 3; ++r) tv[r] = -(M[r][0] * eye[0] + M[r][1] * eye[1] + M[r][2] * eye[2]);
  const double sx = mix(pa.sx, pb.sx), sy = mix(pa.sy, pb.sy), c0 = mix(pa.c0, pb.c0), c1 = mix(pa.c1, pb.c1);
  const double a = mix(pa.a, pb.a), b = mix(pa.b, pb.b), n = mix(pa.n, pb.n);
  double rows[4][4];
  for (int k = 0; k < 3; ++k) {
    rows[0][k] = sx * M[0][k] + c0 * M[2][k];
    rows[1][k] = sy * M[1][k] + c1 * M[2][k];
    rows[2][k] = a * M[2][k];
    rows[3][k] = n * M[2][k];
  }
  rows[0][3] = sx * tv[0] + c0 * tv[2];
  rows[1][3] = sy * tv[1] + c1 * tv[2];
  rows[2][3] = a * tv[2] + b;
  rows[3][3] = n * tv[2];
  for (int r = 0; r < 4; ++r) {
    for (int k = 0; k < 4; ++k) out[k * 4 + r] = rows[r][k];
  }
}

// A shadow volume: immediate geometry drawn only into the stencil.
bool IsVolume(const DrawCommand& d) {
  return !d.mesh && d.stencil.enable && d.colour_mask == 0;
}

// A particle draw whose particles we can pair one by one.
bool IsParticleList(const Frame& f, const DrawCommand& d) {
  return d.material == Material::kParticle && !d.mesh && d.topology == Topology::kTriangleList &&
         d.index_count == 0 && d.vertex_count && d.vertex_count % kParticleVertices == 0 &&
         VerticesInRange(f, d);
}

// The centre of particle q of draw d.
void QuadCentre(const Frame& f, const DrawCommand& d, uint32_t q, float c[3]) {
  const float* v = VertexFloats(f, d) + size_t(q) * kParticleVertices * (d.layout.stride / 4);
  const size_t stride = d.layout.stride / 4;
  c[0] = c[1] = c[2] = 0;
  for (uint32_t i = 0; i < kParticleVertices; ++i) {
    for (int k = 0; k < 3; ++k) c[k] += v[i * stride + k];
  }
  for (int k = 0; k < 3; ++k) c[k] /= float(kParticleVertices);
}

// The average position of an immediate draw's vertices.
bool Centre(const Frame& f, const DrawCommand& d, float c[3]) {
  if (d.mesh || !d.vertex_count || !VerticesInRange(f, d)) return false;
  const float* v = VertexFloats(f, d);
  const size_t stride = d.layout.stride / 4;
  double sum[3] = {};
  for (uint32_t i = 0; i < d.vertex_count; ++i) {
    for (int k = 0; k < 3; ++k) sum[k] += v[i * stride + k];
  }
  for (int k = 0; k < 3; ++k) c[k] = float(sum[k] / d.vertex_count);
  return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}

bool VerticesJumped(const Frame& a, const DrawCommand& da, const Frame& b, const DrawCommand& db) {
  const float* va = VertexFloats(a, da);
  const float* vb = VertexFloats(b, db);
  const size_t stride = db.layout.stride / 4;
  for (uint32_t v = 0; v < db.vertex_count; ++v) {
    const float* pa = va + v * stride;
    const float* pb = vb + v * stride;
    const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    if (!(dx * dx + dy * dy + dz * dz <= kSnapUnits * kSnapUnits)) return true;  // (NaN too)
  }
  return false;
}

}  // namespace

void Pair(const Frame& a, const Frame& b, Pairing& out) {
  out.match.assign(b.draws.size(), -1);
  out.cut = false;
  out.matched = out.snapped = 0;
  out.notes.clear();
  const bool trace = REXCVAR_GET(debug_frame_blend_trace);
  auto note = [&](size_t i, const char* why, float change) {
    if (!trace) return;
    const DrawCommand& d = b.draws[i];
    out.notes.push_back(fmt::format("draw {} material {} {} verts {} idx {} vb {:08X} tex {:08X} target {:08X}: {} ({:.3f})",
                                    i, int(d.material), d.mesh ? "mesh" : "immediate", d.vertex_count,
                                    d.index_count, d.vertex_buffer.address, d.texture.fetch[1],
                                    d.targets.colour, why, change));
  };
  if (!a.recorded || !b.recorded) return;
  // A's draws by key.
  std::unordered_map<Key, std::vector<int32_t>, KeyHash> by_key;
  by_key.reserve(a.draws.size());
  for (size_t i = 0; i < a.draws.size(); ++i) by_key[KeyOf(a.draws[i])].push_back(int32_t(i));
  // WHICH A DRAW EACH B DRAW PAIRS WITH. Copies of one mesh (plants, rocks,
  // the mojo) share a key. History of the rule:
  //  * the n-th of a key = the n-th: one copy culled in or out paired every
  //    later copy with its neighbour (feel test 1);
  //  * then each B draw, in draw order, took the closest UNUSED A draw by
  //    whole matrix: the floating, spinning mojo around a plant changed
  //    rotation more between two logic frames than the distance to a
  //    neighbour, and the first one took its neighbour's partner, which
  //    cascaded (mojo slid into each other's places and new ones popped up:
  //    "moving, then going back", feel test 5).
  // Now: the cost is mainly WHERE THE OBJECT IS ON SCREEN (its origin, or an
  // immediate draw's first vertex, through its matrix: NDC units), the
  // matrix change only breaks ties; and pairs are assigned GLOBALLY, best
  // first (every candidate pair of a key sorted by cost), not in draw order.
  auto screen_point = [](const Frame& f, const DrawCommand& d, float& x, float& y) {
    float p[3] = {0, 0, 0};
    if (!d.mesh) {
      if (!d.vertex_count || !VerticesInRange(f, d)) return false;
      std::memcpy(p, VertexFloats(f, d), sizeof(p));
    }
    const float* m = d.mvp;
    const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (!(w > 1e-4f)) return false;
    x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w;
    y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
    return std::isfinite(x) && std::isfinite(y);
  };
  auto cost_of = [&](const DrawCommand& da, const DrawCommand& db) {
    const float change = MatrixChange(da.mvp, db.mvp);
    float ax, ay, bx, by;
    if (screen_point(a, da, ax, ay) && screen_point(b, db, bx, by)) {
      return std::hypot(ax - bx, ay - by) + 0.05f * change;
    }
    return 2.0f + change;  // (behind the camera: matrices only, after the rest)
  };
  // B's eligible draws by key.
  std::unordered_map<Key, std::vector<int32_t>, KeyHash> b_by_key;
  for (size_t i = 0; i < b.draws.size(); ++i) {
    const DrawCommand& db = b.draws[i];
    if (IsVolume(db)) continue;  // (frame_blend.h SHADOW VOLUMES)
    // 2D (no depth test, or a flat matrix: menus, HUD, texts, full-screen
    // passes) is drawn as the game drew it: two different texts in one font
    // with as many letters, or the pause and Options glass panels, paired up
    // and slid into each other when a menu changed (feel test 2).
    if (!db.depth_test || IsFlat(db)) continue;
    // Particle lists pair particle by particle (PARTICLES below), never as a
    // whole draw: with the same count in both frames, vertex n of A and of B
    // are different particles once the game reorders them (feel test 3).
    if (IsParticleList(b, db)) continue;
    b_by_key[KeyOf(db)].push_back(int32_t(i));
  }
  std::vector<uint8_t> used(a.draws.size(), 0);
  std::vector<int32_t> partner(b.draws.size(), -1);
  struct Candidate {
    float cost;
    int32_t bi, ai;
  };
  std::vector<Candidate> candidates;
  for (auto& [key, bs] : b_by_key) {
    auto it = by_key.find(key);
    if (it == by_key.end()) {
      for (int32_t bi : bs) note(size_t(bi), "no draw with its key in A", 0);
      continue;
    }
    const std::vector<int32_t>& as = it->second;
    candidates.clear();
    if (bs.size() * as.size() > 200000) {  // (huge key: B draw order, as before)
      for (size_t k = 0; k < bs.size() && k < as.size(); ++k) candidates.push_back({0, bs[k], as[k]});
    } else {
      for (int32_t bi : bs) {
        for (int32_t ai : as) candidates.push_back({cost_of(a.draws[size_t(ai)], b.draws[size_t(bi)]), bi, ai});
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const Candidate& x, const Candidate& y) { return x.cost < y.cost; });
    }
    for (const Candidate& c : candidates) {
      if (partner[size_t(c.bi)] >= 0 || used[size_t(c.ai)]) continue;
      partner[size_t(c.bi)] = c.ai;
      used[size_t(c.ai)] = 1;
    }
    for (int32_t bi : bs) {
      if (partner[size_t(bi)] < 0) note(size_t(bi), "its key's draws in A all taken", 0);
    }
  }
  uint32_t moving_3d = 0, snapped_3d = 0;
  for (size_t i = 0; i < b.draws.size(); ++i) {
    const int32_t ia = partner[i];
    if (ia < 0) continue;
    const DrawCommand& db = b.draws[i];
    const DrawCommand& da = a.draws[size_t(ia)];
    const float mchange = MatrixChange(da.mvp, db.mvp);
    bool snap = mchange > kSnapMatrix;
    if (snap) note(i, "matrix jumped", mchange);
    if (!snap && !db.mesh && VerticesInRange(a, da) && VerticesInRange(b, db)) {
      snap = VerticesJumped(a, da, b, db);
      if (snap) note(i, "a vertex jumped", mchange);
    }
    if (db.depth_test) {
      ++moving_3d;
      if (snap) ++snapped_3d;
    }
    if (snap) {
      ++out.snapped;
      continue;
    }
    out.match[i] = ia;
    ++out.matched;
  }
  if (moving_3d >= 8 && float(snapped_3d) > kCutShare * float(moving_3d)) {
    out.cut = true;
  }

  // THE OLDER MATRIX (frame_blend.h): groups of one matrix in B, the camera
  // pair, then every 3D draw's older matrix.
  out.a_mvp.assign(b.draws.size(), {});
  out.has_a_mvp.assign(b.draws.size(), 0);
  {
    struct MatrixKey {
      float m[16];
      bool operator==(const MatrixKey& o) const { return std::memcmp(m, o.m, sizeof(m)) == 0; }
    };
    struct MatrixHash {
      size_t operator()(const MatrixKey& k) const {
        uint64_t h = 1469598103934665603ull;
        const auto* p = reinterpret_cast<const uint8_t*>(k.m);
        for (size_t i = 0; i < sizeof(k.m); ++i) h = (h ^ p[i]) * 1099511628211ull;
        return size_t(h);
      }
    };
    auto key_of = [](const float* m) {
      MatrixKey k;
      std::memcpy(k.m, m, sizeof(k.m));
      return k;
    };
    // B matrix -> the A matrix its first paired draw came from.
    std::unordered_map<MatrixKey, int32_t, MatrixHash> group;
    std::unordered_map<MatrixKey, std::unordered_map<MatrixKey, uint32_t, MatrixHash>, MatrixHash> group_votes;
    std::unordered_map<MatrixKey, int32_t, MatrixHash> group_member;  // an A draw with that A matrix
    // The CAMERA: the matrix most paired world-space (immediate, 3D) draws share.
    std::unordered_map<MatrixKey, uint32_t, MatrixHash> world_votes;
    for (size_t i = 0; i < b.draws.size(); ++i) {
      if (out.match[i] < 0) continue;
      const DrawCommand& db = b.draws[i];
      // (Every member votes with its partner's matrix; the group takes the
      // one most agree on. Taking the FIRST member's partner let one odd
      // draw blend the whole world through a wrong camera for a logic
      // frame: springs, Crash, mojo jumped together, feel test 7.)
      ++group_votes[key_of(db.mvp)][key_of(a.draws[size_t(out.match[i])].mvp)];
      group_member.emplace(key_of(a.draws[size_t(out.match[i])].mvp), out.match[i]);
      if (!db.mesh && db.depth_test && !IsFlat(db)) ++world_votes[key_of(db.mvp)];
    }
    for (const auto& [b_key, votes] : group_votes) {
      const MatrixKey* winner = nullptr;
      uint32_t most = 0;
      for (const auto& [a_key, n] : votes) {
        if (n > most) {
          most = n;
          winner = &a_key;
        }
      }
      if (winner) group[b_key] = group_member[*winner];
    }
    bool camera = false;
    double cam_b_inverse[16];
    const float* cam_a = nullptr;
    const float* cam_b_matrix = nullptr;  // (points into world_votes: alive in this block)
    {
      const MatrixKey* best = nullptr;
      uint32_t votes = 0;
      for (const auto& [k, n] : world_votes) {
        if (n > votes) {
          votes = n;
          best = &k;
        }
      }
      if (best && Invert(best->m, cam_b_inverse)) {
        camera = true;
        cam_a = a.draws[size_t(group[*best])].mvp;
        cam_b_matrix = best->m;
      }
    }
    uint32_t grouped = 0, by_camera = 0;
    for (size_t i = 0; i < b.draws.size(); ++i) {
      const DrawCommand& db = b.draws[i];
      if (!db.depth_test || IsFlat(db)) continue;
      auto it = group.find(key_of(db.mvp));
      if (it != group.end()) {
        std::memcpy(out.a_mvp[i].data(), a.draws[size_t(it->second)].mvp, 16 * sizeof(float));
        out.has_a_mvp[i] = 1;
        if (out.match[i] < 0) ++grouped;
        continue;
      }
      if (!camera || out.match[i] >= 0) continue;  // (paired: always in a group)
      // older = camera_A x camera_B^-1 x B's matrix (column-major).
      double placement[16];
      Multiply(cam_b_inverse, db.mvp, placement);
      double older[16];
      double ca[16];
      for (int k = 0; k < 16; ++k) ca[k] = cam_a[k];
      Multiply(ca, placement, older);
      bool finite = true;
      for (int k = 0; k < 16; ++k) {
        out.a_mvp[i][size_t(k)] = float(older[k]);
        finite = finite && std::isfinite(older[k]);
      }
      out.has_a_mvp[i] = finite;
      by_camera += finite;
    }
    // THE CAMERA, TAKEN APART (frame_blend.h): both cameras split, then
    // every 3D draw with an older matrix gets its two placements.
    out.cam_split = false;
    out.placement.assign(b.draws.size(), {});
    out.has_placement.assign(b.draws.size(), 0);
    double cam_a_inverse[16];
    if (camera && cam_b_matrix && SplitCamera(cam_a, out.cam_pa) && SplitCamera(cam_b_matrix, out.cam_pb) &&
        Invert(cam_a, cam_a_inverse)) {
      out.cam_split = true;
      uint32_t placed = 0;
      for (size_t i = 0; i < b.draws.size(); ++i) {
        if (!out.has_a_mvp[i]) continue;
        double wa[16], wb[16];
        Multiply(cam_a_inverse, out.a_mvp[i].data(), wa);
        Multiply(cam_b_inverse, b.draws[i].mvp, wb);
        bool finite = true;
        for (int k = 0; k < 16; ++k) {
          out.placement[i][size_t(k)] = float(wa[k]);
          out.placement[i][size_t(16 + k)] = float(wb[k]);
          finite = finite && std::isfinite(wa[k]) && std::isfinite(wb[k]);
        }
        out.has_placement[i] = finite;
        placed += finite;
      }
      if (trace) out.notes.push_back(fmt::format("  camera taken apart; {} draws blend as camera x placement", placed));
    } else if (trace) {
      out.notes.push_back("  camera NOT taken apart (old number-by-number blend)");
    }
    if (trace) {
      out.notes.push_back(fmt::format("  unpaired: {} take their group's older matrix, {} the camera's blend{}",
                                      grouped, by_camera, camera ? "" : " (no camera found)"));
    }
  }

  // FOLLOWERS (frame_blend.h): the blended CHARACTERS' centres in B and how
  // they moved (A - B); each shadow volume takes the move of the character
  // nearest to ANY of its vertices (a volume's sides reach far down from the
  // body: their centre is no guide).
  out.follow.assign(b.draws.size(), {});
  struct Body {
    float centre[3], move[3];
  };
  std::vector<Body> bodies;
  // One body per CHARACTER, not per draw: a character is several draws (body,
  // head, hair...) whose own centres swing with the animation; volume pieces
  // following different parts drifted apart (shadows twitched, feel test 2).
  // Draws whose centres are within kGroupUnits of a group's first join it;
  // a group's move = the move of all its vertices' centre.
  constexpr float kGroupUnits = 2.0f;
  struct Group {
    float first[3];
    double sum_b[3] = {}, sum_a[3] = {};
    double weight = 0;
  };
  std::vector<Group> groups;
  for (size_t i = 0; i < b.draws.size(); ++i) {
    const int32_t ia = out.match[i];
    const DrawCommand& db = b.draws[i];
    if (ia < 0 || db.mesh || db.material != Material::kCharacter) continue;
    float cb[3], ca[3];
    if (!Centre(b, db, cb) || !Centre(a, a.draws[size_t(ia)], ca)) continue;
    Group* group = nullptr;
    for (Group& g : groups) {
      const float dx = g.first[0] - cb[0], dy = g.first[1] - cb[1], dz = g.first[2] - cb[2];
      if (dx * dx + dy * dy + dz * dz < kGroupUnits * kGroupUnits) {
        group = &g;
        break;
      }
    }
    if (!group) {
      groups.push_back({});
      group = &groups.back();
      for (int k = 0; k < 3; ++k) group->first[k] = cb[k];
    }
    const double w = db.vertex_count;
    for (int k = 0; k < 3; ++k) {
      group->sum_b[k] += cb[k] * w;
      group->sum_a[k] += ca[k] * w;
    }
    group->weight += w;
  }
  for (const Group& g : groups) {
    Body body;
    for (int k = 0; k < 3; ++k) {
      body.centre[k] = float(g.sum_b[k] / g.weight);
      body.move[k] = float((g.sum_a[k] - g.sum_b[k]) / g.weight);
    }
    bodies.push_back(body);
  }
  for (size_t i = 0; i < b.draws.size() && !bodies.empty(); ++i) {
    const DrawCommand& db = b.draws[i];
    if (!IsVolume(db) || !VerticesInRange(b, db)) continue;
    const float* v = VertexFloats(b, db);
    const size_t stride = db.layout.stride / 4;
    const Body* nearest = nullptr;
    float best = kFollowUnits * kFollowUnits;
    for (const Body& body : bodies) {
      for (uint32_t k = 0; k < db.vertex_count; ++k) {
        const float* p = v + k * stride;
        const float dx = body.centre[0] - p[0], dy = body.centre[1] - p[1], dz = body.centre[2] - p[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) {
          best = d2;
          nearest = &body;
        }
      }
    }
    if (!nearest) continue;
    Pairing::Follow& f = out.follow[i];
    f.on = true;
    for (int k = 0; k < 3; ++k) f.move[k] = nearest->move[k];
    if (trace) {
      out.notes.push_back(fmt::format("  volume {} follows a character {:.2f} units away (move {:.3f} {:.3f} {:.3f})",
                                      i, std::sqrt(best), f.move[0], f.move[1], f.move[2]));
    }
  }

  // PARTICLES (frame_blend.h): A's particles by texture, in a grid of
  // kParticleUnits cells; each particle of an unpaired B particle draw takes
  // the nearest unused one within kParticleUnits.
  out.quads.clear();
  struct APart {
    float c[3];
    uint32_t draw, quad;
    bool used;
  };
  struct Cell {
    int32_t x, y, z;
    uint32_t texture;
    bool operator==(const Cell& o) const {
      return x == o.x && y == o.y && z == o.z && texture == o.texture;
    }
  };
  struct CellHash {
    size_t operator()(const Cell& c) const {
      return size_t((uint64_t(uint32_t(c.x)) * 73856093u) ^ (uint64_t(uint32_t(c.y)) * 19349663u) ^
                    (uint64_t(uint32_t(c.z)) * 83492791u) ^ (uint64_t(c.texture) * 2654435761u));
    }
  };
  std::vector<APart> parts;
  std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
  auto cell_of = [](const float c[3], uint32_t texture) {
    return Cell{int32_t(std::floor(c[0] / kParticleUnits)), int32_t(std::floor(c[1] / kParticleUnits)),
                int32_t(std::floor(c[2] / kParticleUnits)), texture};
  };
  bool any_b = false;
  for (size_t i = 0; i < b.draws.size() && !any_b; ++i) {
    any_b = IsParticleList(b, b.draws[i]);
  }
  if (!any_b) return;
  for (size_t i = 0; i < a.draws.size(); ++i) {
    const DrawCommand& da = a.draws[i];
    if (!IsParticleList(a, da)) continue;
    for (uint32_t q = 0; q < da.vertex_count / kParticleVertices; ++q) {
      APart part{};
      QuadCentre(a, da, q, part.c);
      if (!std::isfinite(part.c[0] + part.c[1] + part.c[2])) continue;
      part.draw = uint32_t(i);
      part.quad = q;
      grid[cell_of(part.c, da.texture.fetch[1])].push_back(uint32_t(parts.size()));
      parts.push_back(part);
    }
  }
  uint32_t linked = 0, total = 0;
  for (size_t i = 0; i < b.draws.size(); ++i) {
    const DrawCommand& db = b.draws[i];
    if (!IsParticleList(b, db)) continue;
    for (uint32_t q = 0; q < db.vertex_count / kParticleVertices; ++q) {
      ++total;
      float c[3];
      QuadCentre(b, db, q, c);
      if (!std::isfinite(c[0] + c[1] + c[2])) continue;
      const Cell home = cell_of(c, db.texture.fetch[1]);
      APart* nearest = nullptr;
      float best = kParticleUnits * kParticleUnits;
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dz = -1; dz <= 1; ++dz) {
            auto it = grid.find(Cell{home.x + dx, home.y + dy, home.z + dz, home.texture});
            if (it == grid.end()) continue;
            for (uint32_t pi : it->second) {
              APart& p = parts[pi];
              if (p.used) continue;
              const float ex = p.c[0] - c[0], ey = p.c[1] - c[1], ez = p.c[2] - c[2];
              const float d2 = ex * ex + ey * ey + ez * ez;
              if (d2 < best) {
                best = d2;
                nearest = &p;
              }
            }
          }
        }
      }
      if (!nearest) continue;
      nearest->used = true;
      out.quads.push_back({uint32_t(i), q, nearest->draw, nearest->quad});
      ++linked;
    }
  }
  if (trace && total) out.notes.push_back(fmt::format("  particles: {} of {} paired one by one", linked, total));
}

// B copied into `out`, the draws' matrices and vertices blended (below).
void BlendDraws(const Frame& a, const Frame& b, const Pairing& pairing, float t, Frame& out);

void Blend(const Frame& a, const Frame& b, const Pairing& pairing, float t, Frame& out) {
  BlendDraws(a, b, pairing, t, out);
  if (pairing.cut || t >= 1.0f || pairing.match.size() != b.draws.size()) return;
  if (t < 0.0f) t = 0.0f;
  // PARTICLES one by one (frame_blend.h): their 6 vertices' positions.
  for (const Pairing::QuadLink& link : pairing.quads) {
    const DrawCommand& db = b.draws[link.b_draw];
    const DrawCommand& da = a.draws[link.a_draw];
    const size_t sb = db.layout.stride / 4, sa = da.layout.stride / 4;
    const float* vb = VertexFloats(b, db) + size_t(link.b_quad) * kParticleVertices * sb;
    const float* va = VertexFloats(a, da) + size_t(link.a_quad) * kParticleVertices * sa;
    float* vo = reinterpret_cast<float*>(out.vertex_data.data()) + db.vertex_offset / 4 +
                size_t(link.b_quad) * kParticleVertices * sb;
    for (uint32_t k = 0; k < kParticleVertices; ++k) Lerp(vo + k * sb, va + k * sa, vb + k * sb, 3, t);
  }
}

void BlendDraws(const Frame& a, const Frame& b, const Pairing& pairing, float t, Frame& out) {
  // B as it is (assign = reuses out's storage).
  out.number = b.number;
  out.recorded = b.recorded;
  out.vertex_data.assign(b.vertex_data.begin(), b.vertex_data.end());
  out.indices.assign(b.indices.begin(), b.indices.end());
  out.snapshots = b.snapshots;
  out.blocks = b.blocks;
  out.draws = b.draws;
  out.clears = b.clears;
  out.resolves = b.resolves;
  out.commands = b.commands;
  out.surfaces = b.surfaces;
  if (pairing.cut || t >= 1.0f || pairing.match.size() != b.draws.size()) return;
  if (t < 0.0f) t = 0.0f;
  // THE CAMERA, TAKEN APART: this picture's camera, once.
  double camera_t[16];
  const bool split = pairing.cam_split && pairing.has_placement.size() == b.draws.size();
  if (split) JoinCamera(pairing.cam_pa, pairing.cam_pb, t, camera_t);
  // A draw's matrix at t: camera_t x its blended placement.
  auto camera_times_placement = [&](size_t i, float* mvp) {
    double place[16], m[16];
    for (int k = 0; k < 16; ++k) {
      place[k] = pairing.placement[i][size_t(k)] +
                 (pairing.placement[i][size_t(16 + k)] - pairing.placement[i][size_t(k)]) * t;
    }
    Multiply(camera_t, place, m);
    for (int k = 0; k < 16; ++k) mvp[k] = float(m[k]);
  };

  for (size_t i = 0; i < out.draws.size(); ++i) {
    const int32_t ia = pairing.match[i];
    if (ia < 0) {
      // Unpaired: its older matrix (group / camera, THE OLDER MATRIX), and
      // the material block's copy of it.
      if (i < pairing.has_a_mvp.size() && pairing.has_a_mvp[i]) {
        const DrawCommand& db = b.draws[i];
        float blended[16];
        if (split && pairing.has_placement[i]) {
          camera_times_placement(i, blended);
        } else {
          Lerp(blended, pairing.a_mvp[i].data(), db.mvp, 16, t);
        }
        if (db.block != DrawCommand::kNoBlock && db.block < out.blocks.size()) {
          float* block = reinterpret_cast<float*>(&out.blocks[db.block]);
          if (std::memcmp(block, db.mvp, sizeof(db.mvp)) == 0) std::memcpy(block, blended, sizeof(blended));
        }
        std::memcpy(out.draws[i].mvp, blended, sizeof(blended));
      }
      // A follower: shifted with its body (frame_blend.h FOLLOWERS).
      if (i < pairing.follow.size() && pairing.follow[i].on) {
        const DrawCommand& db = b.draws[i];
        const float* m = pairing.follow[i].move;
        const float s = 1.0f - t;
        float* vo = reinterpret_cast<float*>(out.vertex_data.data()) + db.vertex_offset / 4;
        const size_t stride = db.layout.stride / 4;
        for (uint32_t v = 0; v < db.vertex_count; ++v) {
          for (int k = 0; k < 3; ++k) vo[v * stride + k] += m[k] * s;
        }
      }
      continue;
    }
    const DrawCommand& da = a.draws[size_t(ia)];
    const DrawCommand& db = b.draws[i];
    DrawCommand& d = out.draws[i];
    // The draw's own matrix (the simple material's shaders read this one):
    // from its group's older matrix (one object, one blend).
    const float* older = i < pairing.has_a_mvp.size() && pairing.has_a_mvp[i] ? pairing.a_mvp[i].data() : da.mvp;
    if (split && pairing.has_placement[i]) {
      camera_times_placement(i, d.mvp);
    } else {
      Lerp(d.mvp, older, db.mvp, 16, t);
    }
    // Its material block's matrices and camera.
    const size_t n = MovingBlockFloats(db);
    if (n && da.block != DrawCommand::kNoBlock && da.block < a.blocks.size() &&
        db.block < out.blocks.size()) {
      Lerp(reinterpret_cast<float*>(&out.blocks[db.block]),
           reinterpret_cast<const float*>(&a.blocks[da.block]),
           reinterpret_cast<const float*>(&b.blocks[db.block]), n, t);
      // The block's matrix = the draw's (same group blend).
      float* block = reinterpret_cast<float*>(&out.blocks[db.block]);
      if (std::memcmp(&b.blocks[db.block], db.mvp, sizeof(db.mvp)) == 0) std::memcpy(block, d.mvp, sizeof(d.mvp));
    }
    // Immediate geometry: positions + normal, binormal, tangent per vertex.
    if (!db.mesh && VerticesInRange(a, da) && VerticesInRange(b, db)) {
      const float* va = VertexFloats(a, da);
      const float* vb = VertexFloats(b, db);
      float* vo = reinterpret_cast<float*>(out.vertex_data.data()) + db.vertex_offset / 4;
      const size_t stride = db.layout.stride / 4;
      const uint8_t parts[3] = {db.layout.normal, db.layout.binormal, db.layout.tangent};
      for (uint32_t v = 0; v < db.vertex_count; ++v) {
        const size_t at = v * stride;
        Lerp(vo + at, va + at, vb + at, 3, t);
        for (uint8_t offset : parts) {
          if (offset == VertexLayout::kAbsent || offset % 4 || offset / 4 + 3 > stride) continue;
          Lerp(vo + at + offset / 4, va + at + offset / 4, vb + at + offset / 4, 3, t);
        }
      }
    }
  }
}

}  // namespace native::frame_blend

// -----------------------------------------------------------------------------
// The console's `blendrec` (frame_blend.h RecordCommand)
// -----------------------------------------------------------------------------
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

REXCVAR_DECLARE(int32_t, logic_rate);
REXCVAR_DECLARE(bool, frame_blending);
REXCVAR_DECLARE(std::string, debug_frame_blend_points);

namespace native::frame_blend {
namespace {

// One object in one picture (a line of the points file).
struct Point {
  std::string kind;  // "material/mesh/count"
  float x, y;
};

// The nearest point of the same kind within `radius` pixels, or null.
const Point* Near(const std::vector<Point>& picture, const Point& p, float radius) {
  const Point* best = nullptr;
  float best_d2 = radius * radius;
  for (const Point& q : picture) {
    if (q.kind != p.kind) continue;
    const float d2 = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
    if (d2 < best_d2) {
      best_d2 = d2;
      best = &q;
    }
  }
  return best;
}

std::string Analyse(const std::string& path) {
  std::ifstream in(path);
  std::vector<std::vector<Point>> pictures(1);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line[0] == 'P') {
      pictures.emplace_back();
      continue;
    }
    std::istringstream words(line);
    double wall;
    uint32_t id, count;
    int material, mesh;
    float x, y;
    if (!(words >> wall >> id >> material >> mesh >> count >> x >> y)) continue;
    if (material == int(Material::kParticle)) continue;  // (particles pair one by one: own story)
    if (x < 0 || x >= 1280 || y < 0 || y >= 720) continue;
    pictures.back().push_back({fmt::format("material {} {} {}", material, mesh ? "mesh" : "immediate", count), x, y});
  }
  if (pictures.back().empty()) pictures.pop_back();
  struct Tally {
    uint32_t vanished = 0, went_back = 0;
    float x = 0, y = 0;  // a sample spot
  };
  std::map<std::string, Tally> kinds;
  uint64_t seen = 0;
  for (size_t p = 1; p + 1 < pictures.size(); ++p) {
    const auto& before = pictures[p - 1];
    const auto& now = pictures[p];
    const auto& after = pictures[p + 1];
    for (const Point& a : before) {
      if (Near(after, a, 12) && !Near(now, a, 25)) {
        Tally& t = kinds[a.kind];
        if (!t.vanished && !t.went_back) t.x = a.x, t.y = a.y;
        ++t.vanished;
      }
    }
    for (const Point& b : now) {
      ++seen;
      const Point* a = Near(before, b, 12);
      const Point* c = Near(after, b, 12);
      if (!a || !c) continue;
      const float d1x = b.x - a->x, d1y = b.y - a->y, d2x = c->x - b.x, d2y = c->y - b.y;
      const float l1 = std::hypot(d1x, d1y), l2 = std::hypot(d2x, d2y);
      if (l1 > 0.4f && l2 > 0.4f && d1x * d2x + d1y * d2y < -0.5f * l1 * l2) {
        Tally& t = kinds[b.kind];
        if (!t.vanished && !t.went_back) t.x = b.x, t.y = b.y;
        ++t.went_back;
      }
    }
  }
  std::vector<std::pair<std::string, Tally>> list(kinds.begin(), kinds.end());
  std::sort(list.begin(), list.end(), [](const auto& l, const auto& r) {
    return l.second.vanished + l.second.went_back > r.second.vanished + r.second.went_back;
  });
  uint64_t vanished = 0, went_back = 0;
  for (const auto& [k, t] : list) vanished += t.vanished, went_back += t.went_back;
  std::string out = fmt::format(
      "{} pictures, {} object positions: {} vanished for one picture, {} went back for one picture\n",
      pictures.size(), seen, vanished, went_back);
  for (size_t i = 0; i < list.size() && i < 10; ++i) {
    out += fmt::format("  {:3} vanished {:3} went back  {}  (e.g. at {:.0f},{:.0f} of 1280x720)\n",
                       list[i].second.vanished, list[i].second.went_back, list[i].first, list[i].second.x,
                       list[i].second.y);
  }
  out += "(Small numbers on Crash's own parts = his animation, e.g. feet landing.)\n";
  return out;
}

}  // namespace

std::string RecordCommand(double seconds) {
  if (!(seconds > 0) || seconds > 60) return "blendrec <seconds> (0-60)\n";
  if (REXCVAR_GET(logic_rate) <= 0) return "blendrec: needs fixed-rate logic (Game Logic 30 or 60)\n";
  if (!REXCVAR_GET(debug_frame_blend_points).empty()) return "blendrec: a recording is already running\n";
  const bool was_blending = REXCVAR_GET(frame_blending);
  const std::string path =
      (std::filesystem::temp_directory_path() /
       fmt::format("crashmom-blendrec-{}.txt", std::chrono::steady_clock::now().time_since_epoch().count()))
          .string();
  rex::cvar::SetFlagByName("debug_frame_blend_points", path);
  if (!was_blending) rex::cvar::SetFlagByName("frame_blending", "true");
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  // Stop: the renderer closes the file at its next blended picture, so
  // blending stays on a moment longer when it was off before.
  rex::cvar::SetFlagByName("debug_frame_blend_points", "");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  if (!was_blending) rex::cvar::SetFlagByName("frame_blending", "false");
  std::string report = Analyse(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return report + (was_blending ? "" : "(blending was off: switched on for the recording, off again)\n");
}

}  // namespace native::frame_blend
