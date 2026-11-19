// =============================================================================
// world/test_course.cpp -- the test course's geometry and its collision data patch (see test_course.h)
// =============================================================================
//
// THE COLLISION OBJECT (findings/69 s.2-3). Chunk 07010025 "room_collision" ->
// 07010001 -> mesh 0701000A: u32 5, 2 u32, box min + max (6 floats), and its
// children, all u32 count + entries:
//   07000003 corners      x y z floats
//   07000006 triangles    u16 corner a, b, c + u16 flags: bit k = edge k (a-b, b-c, c-a) is OPEN or a RIDGE
//   07000748 entries      u32 triangle across edge 0/1/2 (0xFFFFFFFF none), f32 1.9 x3, f32 normal x y z
//   07000008 search tree  u16 words (format: header of notes/scratch-tools/maps/coltree.py; walker sub_8244E760)
//   07016000 streams      07016012 "SurfaceMaterialData" = u32 n, n u16 triangles, n u16 materials
// The normal is cross(b - a, c - a) (the game's winding). The tree's grid:
// trunc((p - box min) x 65535 / (box max - box min)) per axis, clamped. The
// box grows south (kBoxMinZ) for the copy of a real spot; the tree is built
// again for every triangle on that grid (the old one doesn't know the new ones).
//
// THE TREE BUILDER: a kd-tree split at the middle of each node's grid box
// along its widest axis; a triangle reaching <= split goes left, >= split
// right (crossing ones to both, as on the disc). Type-0 nodes (head = axis
// << 14, then the split and two child offsets counted in words from the
// node's head) and leaves (head = 3 << 14 | count, then the triangles).
// Leaves: <= 4 triangles, depth 22, or no side got smaller. Over 65,536 words
// (u16 offsets) -> again with bigger leaves and less depth. Checked against
// brute force on the game's own meshes (no triangle ever missed); 2-3x
// looser than the disc's trees on big meshes (they also pack short lists
// into the nodes), exact on small ones.
// =============================================================================

#include "test_course.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"
#include "../cheats/spawn.h"
#include "../debug_console.h"
#include "../players/more_players.h"

REXCVAR_DEFINE_BOOL(test_course, false, "CrashMoM",
                    "Debug: an invisible test course (ramps, stairs, a bumpy slope) in empty air on Wumpa "
                    "Island, for frame-rate probes; the log lists each lane's start spot "
                    "(src/world/test_course.cpp)");
REXCVAR_DEFINE_STRING(test_course_start, "", "CrashMoM",
                      "Debug: with --test_course, put the players at this lane's start (e.g. ropegap) when "
                      "the course's objects are placed, on a fixed game frame (online: both PCs alike)");
REXCVAR_DEFINE_STRING(debug_test_course_dump, "", "CrashMoM",
                      "Debug: also write the patched Wumpa Island package to this file (to compare with "
                      "notes/scratch-tools/maps/p3d_patch.py) (src/world/test_course.cpp)");

namespace test_course {
namespace {

using pure3d::Bytes;
using pure3d::Node;

bool g_course_built = false;  // the patch ran this session (Wumpa Island's piece was loaded)
constexpr uint32_t kCategoryRoom = 15;            // "room" packages (LevelPackages type 14 + 1)
constexpr const char* kPackage = "L1/9db06066";   // the Wumpa Island piece the course goes into
constexpr uint16_t kGroundMaterial = 19;          // the island's usual ground (2,128 of its 3,987 triangles)

constexpr uint32_t kChunkCollision = 0x07010025, kChunkMesh = 0x0701000A, kChunkCorners = 0x07000003,
                   kChunkTriangles = 0x07000006, kChunkTree = 0x07000008, kChunkEntries = 0x07000748,
                   kChunkStreams = 0x07016000, kChunkMaterials = 0x07016012;

// ---------------------------------------------------------------------------
// The course (same numbers and order as notes/scratch-tools/maps/course.py)
// ---------------------------------------------------------------------------
constexpr double kX0 = 312.0, kZ0 = -420.0, kPitch = 9.0, kWidth = 8.0;   // lanes along +x, side by side in z
constexpr double kBase = 20.0, kFloor = 12.0, kPad = 6.0;                // end pads at kBase, catch floor below

enum class Kind { kRamp, kStairs, kBumpy, kGap };
struct Lane {
  const char* name;
  Kind kind;
  double p;  // ramp / bumpy: angle in degrees; stairs: step height; gap: its length
};
constexpr Lane kLanes[] = {
    {"ramp10", Kind::kRamp, 10},      {"ramp20", Kind::kRamp, 20},      {"ramp30", Kind::kRamp, 30},
    {"ramp40", Kind::kRamp, 40},      {"ramp50", Kind::kRamp, 50},      {"step010", Kind::kStairs, 0.10},
    {"step025", Kind::kStairs, 0.25}, {"step050", Kind::kStairs, 0.50}, {"step100", Kind::kStairs, 1.00},
    {"bumpy15", Kind::kBumpy, 15},
    // GAPS (2026-11-19): two pads with nothing between them; the course's
    // OBJECTS (kObjects below) go into them: a rope to swing across, a
    // platform to land on. A miss falls onto the catch floor.
    {"ropegap", Kind::kGap, 6},       {"platgap", Kind::kGap, 8},
};
constexpr int kStairCount = 10;
// Where lane k starts (its -x, -z corner). The first ten side by side from
// (kX0, kZ0); the GAP lanes in their own clear spot, east of the copy of the
// courtyard descent and south of Wumpa Island's own pieces (2026-11-19: first
// put next to the others, at z -330..-313, they ran into a Wumpa structure;
// a scan of every L1 piece's collision found x 340-370, z -470..-440 clear).
constexpr double kGapX0 = 344.0, kGapZ0 = -470.0;
constexpr int kFirstGapLane = 10;  // kLanes[10..] are the gap lanes
double LaneX0(int k) { return k >= kFirstGapLane ? kGapX0 : kX0; }
double LaneZ0(int k) { return k >= kFirstGapLane ? kGapZ0 + (k - kFirstGapLane) * kPitch : kZ0 + k * kPitch; }

constexpr double kPi = 3.14159265358979323846;
double Radians(double deg) { return deg * kPi / 180.0; }

// A lane's feature: its length along x and the height above kBase at t (0 .. run).
struct Profile {
  double run;
  double drop;  // ramp / bumpy
  double top;   // stairs
};
Profile ProfileOf(const Lane& l) {
  switch (l.kind) {
    case Kind::kRamp: return {12.0, 12.0 * std::tan(Radians(l.p)), 0};
    case Kind::kStairs: return {double(kStairCount), 0, kStairCount * l.p};
    case Kind::kBumpy: return {16.0, 16.0 * std::tan(Radians(l.p)), 0};
    case Kind::kGap: return {l.p, 0, 0};
  }
  return {};
}
double HeightAt(const Lane& l, const Profile& pr, double t) {
  if (l.kind == Kind::kStairs) {
    return pr.top - l.p * std::min<double>(kStairCount, std::floor(t + 1e-9));
  }
  if (l.kind == Kind::kGap) return 0;
  return pr.drop * (1 - t / pr.run);
}

// The bumpy lane's +-0.12 per grid corner (course.py's jitter()).
double Jitter(int lane, int i, int j) {
  uint64_t h = (uint64_t(lane) * 83492791u ^ uint64_t(i) * 73856093u ^ uint64_t(j) * 19349663u) & 0xFFFFFFFFu;
  h = ((h ^ (h >> 13)) * 0x5BD1E995u) & 0xFFFFFFFFu;
  h ^= h >> 15;
  return (double(h % 2001) / 1000.0 - 1.0) * 0.12;
}

using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;

std::vector<Triangle> CourseTriangles() {
  std::vector<Triangle> tris;
  auto quad = [&](const Point& a, const Point& b, const Point& c, const Point& d) {
    tris.push_back({a, b, c});  // counter-clockwise seen from where the normal points
    tris.push_back({a, c, d});
  };
  for (int k = 0; k < int(std::size(kLanes)); ++k) {
    const Lane& l = kLanes[k];
    const Profile pr = ProfileOf(l);
    const double z0 = LaneZ0(k), x0 = LaneX0(k);
    auto y_at = [&](double xl, double zl) {  // xl = 0 .. pad + run + pad along the lane
      if (xl <= kPad) return kBase + HeightAt(l, pr, 0.0);
      if (xl >= kPad + pr.run) return kBase + HeightAt(l, pr, pr.run);
      double y = kBase + HeightAt(l, pr, xl - kPad);
      if (l.kind == Kind::kBumpy) y += Jitter(k, int(std::lround(xl)), int(std::lround(zl - z0)));
      return y;
    };
    const int cells_x = int(kPad + pr.run + kPad), cells_z = int(kWidth);
    for (int i = 0; i < cells_x; ++i) {
      const double xa = i, xb = i + 1;
      if (l.kind == Kind::kGap && xa >= kPad && xb <= kPad + pr.run) continue;  // the gap itself
      for (int j = 0; j < cells_z; ++j) {
        const double za = z0 + j, zb = z0 + j + 1;
        if (l.kind == Kind::kStairs) {  // treads: flat cells at the height of the step under the cell's middle
          const double ym = xa + 0.5 < kPad + pr.run ? y_at(xa + 0.5, za) : kBase;
          quad({x0 + xa, ym, za}, {x0 + xa, ym, zb}, {x0 + xb, ym, zb}, {x0 + xb, ym, za});
        } else {
          quad({x0 + xa, y_at(xa, za), za}, {x0 + xa, y_at(xa, zb), zb}, {x0 + xb, y_at(xb, zb), zb},
               {x0 + xb, y_at(xb, za), za});
        }
      }
    }
    if (l.kind == Kind::kStairs) {  // risers at each step line, facing +x (down the stairs)
      for (int s = 1; s <= kStairCount; ++s) {
        const double xc = kPad + s;
        const double hi = y_at(xc - 0.5, z0);
        const double lo = xc + 0.5 > kPad + pr.run ? kBase : y_at(xc + 0.5, z0);
        for (int j = 0; j < cells_z; ++j) {
          const double za = z0 + j, zb = z0 + j + 1;
          quad({x0 + xc, lo, za}, {x0 + xc, hi, za}, {x0 + xc, hi, zb}, {x0 + xc, lo, zb});
        }
      }
    }
  }
  const double zmax = kZ0 + double(kFirstGapLane) * kPitch;  // the catch floor, 4-unit cells
  for (double x = kX0; x < kX0 + 32; x += 4) {  // from the lanes on: the descent's copy is west of them
    for (double z = kZ0 - 4; z < zmax; z += 4) {
      quad({x, kFloor, z}, {x, kFloor, z + 4}, {x + 4, kFloor, z + 4}, {x + 4, kFloor, z});
    }
  }
  // ... and one under the gap lanes (a missed rope / platform lands here).
  for (double x = kGapX0 - 4; x < kGapX0 + 28; x += 4) {
    for (double z = kGapZ0 - 4; z < kGapZ0 + (int(std::size(kLanes)) - kFirstGapLane) * kPitch + 4; z += 4) {
      quad({x, kFloor, z}, {x, kFloor, z + 4}, {x + 4, kFloor, z + 4}, {x + 4, kFloor, z});
    }
  }
  return tris;
}

// ---------------------------------------------------------------------------
// THE COPY OF A REAL SPOT: the Ratcicle Kingdom courtyard's descent (findings/22 s.5-11, findings/69 s.7)
// ---------------------------------------------------------------------------
// The course's ramps are smooth planes; the real descent is rough (a new
// triangle every ~0.45 units, nearly every join bent, up to 30-50 degrees:
// findings/69 s.7). So its own triangles are copied, from the player's disc,
// out of Ice Kingdom's piece L2/8514dde0: every triangle whose middle is in
// x 252..284, z -148..-104 (the reported ground losses and the slope probe's
// path, with room around them), with its own edge flags and surface material,
// moved by kCopyShift into free air south of the lanes. The Wumpa piece's box
// is grown for it (kBoxMinZ; kBoxMinY..kBoxMaxY in y, so the descent's walls,
// y -14 to 56, come along whole); a triangle still reaching outside it is
// left out, and edges cut at the copy's border become open edges. A catch
// floor at y -15.5 lies under it. Same heights as the original: its slope probe's
// spot (262.329, 31.5, -126.734) is (310.329, 31.5, -456.734) here.
constexpr const char* kCopyArchiveName = "package\\L2\\8514dde0.p3d";
constexpr double kCopyX0 = 252, kCopyX1 = 284, kCopyZ0 = -148, kCopyZ1 = -104;
constexpr std::array<double, 3> kCopyShift{48.0, 0.0, -330.0};
// The Wumpa piece's box grows south to here, so the copy fits (findings/69 s.7: a first, tighter copy beside the
// lanes gave extra ground losses at its cut edge, 2 units from the probe's start). No other piece reaches it.
constexpr double kBoxMinZ = -480.0;
// ... and in y, so the descent's walls (y -14 to 56) come along whole: without them the probes ran off the copy
// where the original stops at a wall (findings/69 s.7).
constexpr double kBoxMinY = -16.0, kBoxMaxY = 60.0;
constexpr double kCopyFloor = -15.5;  // under the copy's lowest ground (y -14)

struct CopiedTriangle {
  Triangle t;
  int disc_flags;     // the original's edge flags; -1 = work them out (the copy's catch floor)
  uint16_t material;  // the original's surface material
};

uint32_t U32(const Bytes& b, size_t at);
uint16_t U16(const Bytes& b, size_t at);
float F32(const Bytes& b, size_t at);
Node* FindNode(Node& n, uint32_t id);

std::vector<CopiedTriangle> CopiedSpot(const std::array<double, 3>& box_lo, const std::array<double, 3>& box_hi) {
  std::vector<CopiedTriangle> out;
  Bytes file;
  std::string error;
  if (!data_patcher::ReadArchiveFile(kCopyArchiveName, &file, &error)) {
    REXLOG_WARN("Test course: no copy of the Ice Kingdom descent ({})", error);
    return out;
  }
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(file, &root, &top)) return out;
  Node n = pure3d::ToNode(file, root);
  Node* mesh = FindNode(n, 0x0701000A);
  if (!mesh) return out;
  const Node *corners = nullptr, *triangles = nullptr;
  Node* streams = nullptr;
  for (Node& k : mesh->children) {
    if (k.id == 0x07000003) corners = &k;
    if (k.id == 0x07000006) triangles = &k;
    if (k.id == 0x07016000) streams = &k;
  }
  const Node* materials = streams ? FindNode(*streams, 0x07016012) : nullptr;
  if (!corners || !triangles || !materials) return out;
  const uint32_t nv = U32(corners->data, 0), nt = U32(triangles->data, 0);
  if (corners->data.size() != 4 + 12 * size_t(nv) || triangles->data.size() != 4 + 8 * size_t(nt)) return out;
  std::vector<uint16_t> material_of(nt, kGroundMaterial);
  {
    const Bytes& m = materials->data;
    const size_t count_at = 4 + 1 + m[4];
    const uint32_t cnt = U32(m, count_at);
    for (uint32_t i = 0; i < cnt && count_at + 4 + 4 * size_t(cnt) <= m.size(); ++i) {
      const uint16_t tri = U16(m, count_at + 4 + 2 * i);
      if (tri < nt) material_of[tri] = U16(m, count_at + 4 + 2 * cnt + 2 * i);
    }
  }
  auto corner_at = [&](uint16_t i) {
    Point p;
    for (int k = 0; k < 3; ++k) p[k] = F32(corners->data, 4 + 12 * size_t(i) + 4 * k) + kCopyShift[k];
    return p;
  };
  for (uint32_t i = 0; i < nt; ++i) {
    const size_t o = 4 + 8 * size_t(i);
    const uint16_t c[3] = {U16(triangles->data, o), U16(triangles->data, o + 2), U16(triangles->data, o + 4)};
    if (c[0] >= nv || c[1] >= nv || c[2] >= nv) continue;
    const Triangle t{corner_at(c[0]), corner_at(c[1]), corner_at(c[2])};
    const double mx = (t[0][0] + t[1][0] + t[2][0]) / 3 - kCopyShift[0];
    const double mz = (t[0][2] + t[1][2] + t[2][2]) / 3 - kCopyShift[2];
    if (mx < kCopyX0 || mx > kCopyX1 || mz < kCopyZ0 || mz > kCopyZ1) continue;
    bool inside = true;
    for (const Point& p : t) {
      for (int k = 0; k < 3; ++k) inside = inside && p[k] > box_lo[k] + 0.25 && p[k] < box_hi[k] - 0.25;
    }
    if (!inside) continue;
    out.push_back({t, U16(triangles->data, o + 6), material_of[i]});
  }
  const size_t copied = out.size();
  // Its catch floor, 4-unit cells under the copy (x 296..336, z -476..-428).
  for (double x = 296; x < 336; x += 4) {
    for (double z = -476; z < -428; z += 4) {
      const Point a{x, kCopyFloor, z}, b{x, kCopyFloor, z + 4}, c{x + 4, kCopyFloor, z + 4}, d{x + 4, kCopyFloor, z};
      out.push_back({{a, b, c}, -1, kGroundMaterial});
      out.push_back({{a, c, d}, -1, kGroundMaterial});
    }
  }
  REXLOG_INFO("Test course: copy of the Ice Kingdom descent = {} triangles from L2/8514dde0; its slope probe "
              "starts at {:.3f} {:.3f} {:.3f}", copied, 262.329 + kCopyShift[0], 31.5 + kCopyShift[1],
              -126.734 + kCopyShift[2]);
  return out;
}

// ---------------------------------------------------------------------------
// Little-endian helpers over a chunk's data
// ---------------------------------------------------------------------------
uint32_t U32(const Bytes& b, size_t at) { return pure3d::Le32(&b[at]); }
uint16_t U16(const Bytes& b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }
float F32(const Bytes& b, size_t at) {
  const uint32_t v = U32(b, at);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void Put16(Bytes& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void PutF(Bytes& out, double d) {
  const float f = float(d);
  uint32_t v;
  std::memcpy(&v, &f, 4);
  pure3d::PutLe32(out, v);
}

Node* FindNode(Node& n, uint32_t id) {
  if (n.id == id) return &n;
  for (Node& k : n.children) {
    if (Node* f = FindNode(k, id)) return f;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The search tree (coltree_build.py's build())
// ---------------------------------------------------------------------------
struct GridBox {
  std::array<int, 3> lo, hi;
};

bool BuildTree(const std::vector<GridBox>& boxes, std::vector<uint16_t>* words) {
  int leaf = 4, max_depth = 22;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<uint32_t> out;
    struct Builder {
      const std::vector<GridBox>& boxes;
      std::vector<uint32_t>& out;
      int leaf, max_depth;
      size_t Node(const std::vector<int>& ids, std::array<int, 3> c0, std::array<int, 3> c1, int depth) {
        const size_t start = out.size();
        int axis = 0;  // the widest axis of this node's cell (the first of equals)
        for (int k = 1; k < 3; ++k) {
          if (c1[k] - c0[k] > c1[axis] - c0[axis]) axis = k;
        }
        const int split = (c0[axis] + c1[axis]) / 2;
        std::vector<int> left, right;
        for (int i : ids) {
          if (boxes[i].lo[axis] <= split) left.push_back(i);
          if (boxes[i].hi[axis] >= split) right.push_back(i);
        }
        if (int(ids.size()) <= leaf || depth >= max_depth || c1[axis] - c0[axis] < 2 ||
            (left.size() == ids.size() && right.size() == ids.size())) {
          out.push_back((3u << 14) | uint32_t(ids.size()));  // a leaf (ids <= 0xFFF: checked by the caller)
          for (int i : ids) out.push_back(uint32_t(i));
          return start;
        }
        out.push_back(uint32_t(axis) << 14);  // type 0: both sides are child offsets
        out.push_back(uint32_t(split));
        const size_t lw = out.size();
        out.push_back(0);
        const size_t rw = out.size();
        out.push_back(0);
        std::array<int, 3> l1 = c1, r0 = c0;
        l1[axis] = split;
        r0[axis] = split;
        out[lw] = uint32_t(Node(left, c0, l1, depth + 1) - start);
        out[rw] = uint32_t(Node(right, r0, c1, depth + 1) - start);
        return start;
      }
    } b{boxes, out, leaf, max_depth};
    std::vector<int> all(boxes.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = int(i);
    b.Node(all, {0, 0, 0}, {65535, 65535, 65535}, 0);
    const bool fits = out.size() < 65536 &&
                      std::all_of(out.begin(), out.end(), [](uint32_t w) { return w < 65536; });
    if (fits) {
      words->assign(out.begin(), out.end());
      return true;
    }
    leaf *= 2;
    max_depth -= 2;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The patch (p3d_patch.py's patch())
// ---------------------------------------------------------------------------
bool PatchPackage(const Bytes& original, Bytes* patched) {
  g_course_built = true;  // (the objects' Tick: this session's Wumpa Island has the course)
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  Node file = pure3d::ToNode(original, root);
  Node* collision = FindNode(file, kChunkCollision);
  Node* mesh = collision ? FindNode(*collision, kChunkMesh) : nullptr;
  if (!mesh || mesh->data.size() < 36) {
    REXLOG_WARN("Test course: {} has no collision mesh where expected: left alone", kPackage);
    return false;
  }
  Node *corners = nullptr, *triangles = nullptr, *tree = nullptr, *entries = nullptr, *streams = nullptr;
  for (Node& k : mesh->children) {
    if (k.id == kChunkCorners) corners = &k;
    if (k.id == kChunkTriangles) triangles = &k;
    if (k.id == kChunkTree) tree = &k;
    if (k.id == kChunkEntries) entries = &k;
    if (k.id == kChunkStreams) streams = &k;
  }
  Node* materials = streams ? FindNode(*streams, kChunkMaterials) : nullptr;
  if (!corners || !triangles || !tree || !entries || !materials) {
    REXLOG_WARN("Test course: {}'s collision mesh lacks a part: left alone", kPackage);
    return false;
  }
  // The mesh as it is.
  std::array<double, 3> lo, hi;
  for (int k = 0; k < 3; ++k) {
    lo[k] = F32(mesh->data, 12 + 4 * k);
    hi[k] = F32(mesh->data, 24 + 4 * k);
  }
  // The box grows south for the copy of the real spot (the tree below is built on the new grid).
  auto grow = [&](double& v, double to, bool lower, size_t at) {
    if (lower ? v <= to : v >= to) return;
    v = to;
    Bytes f;
    PutF(f, v);
    std::copy(f.begin(), f.end(), mesh->data.begin() + at);
  };
  grow(lo[2], kBoxMinZ, true, 12 + 8);    // box min z
  grow(lo[1], kBoxMinY, true, 12 + 4);    // box min y
  grow(hi[1], kBoxMaxY, false, 24 + 4);   // box max y
  const uint32_t nv = U32(corners->data, 0), nt = U32(triangles->data, 0);
  if (corners->data.size() != 4 + 12 * size_t(nv) || triangles->data.size() != 4 + 8 * size_t(nt) ||
      entries->data.size() != 4 + 36 * size_t(nt) || U32(entries->data, 0) != nt) {
    REXLOG_WARN("Test course: {}'s collision mesh isn't the expected shape: left alone", kPackage);
    return false;
  }
  std::vector<Point> V(nv);
  for (uint32_t i = 0; i < nv; ++i) {
    for (int k = 0; k < 3; ++k) V[i][k] = F32(corners->data, 4 + 12 * i + 4 * k);
  }
  struct Tri {
    uint16_t a, b, c, flags;
  };
  std::vector<Tri> T(nt);
  for (uint32_t i = 0; i < nt; ++i) {
    const size_t o = 4 + 8 * size_t(i);
    T[i] = {U16(triangles->data, o), U16(triangles->data, o + 2), U16(triangles->data, o + 4),
            U16(triangles->data, o + 6)};
  }
  // The course's corners (shared ones merged; values as the game will read them: 32-bit floats).
  std::map<std::array<long long, 3>, uint16_t> index;
  auto corner = [&](const Point& p) {
    const std::array<long long, 3> key{std::llround(p[0] * 1e4), std::llround(p[1] * 1e4),
                                       std::llround(p[2] * 1e4)};
    const auto it = index.find(key);
    if (it != index.end()) return it->second;
    const uint16_t n = uint16_t(V.size());
    V.push_back({double(float(p[0])), double(float(p[1])), double(float(p[2]))});
    index.emplace(key, n);
    return n;
  };
  const size_t first = T.size();
  std::vector<std::array<uint16_t, 3>> added;
  std::vector<int> disc_flags;            // per added triangle: the copied original's flags, -1 = work out
  std::vector<uint16_t> added_material;
  for (const Triangle& t : CourseTriangles()) {
    added.push_back({corner(t[0]), corner(t[1]), corner(t[2])});
    disc_flags.push_back(-1);
    added_material.push_back(kGroundMaterial);
  }
  for (const CopiedTriangle& c : CopiedSpot(lo, hi)) {
    added.push_back({corner(c.t[0]), corner(c.t[1]), corner(c.t[2])});
    disc_flags.push_back(c.disc_flags);
    added_material.push_back(c.material);
  }
  if (V.size() >= 65536 || first + added.size() >= 65536) {
    REXLOG_WARN("Test course: too many corners or triangles for 16-bit numbers: left alone");
    return false;
  }
  auto normal = [&](const std::array<uint16_t, 3>& t) {
    Point u, w, n;
    for (int k = 0; k < 3; ++k) {
      u[k] = V[t[1]][k] - V[t[0]][k];
      w[k] = V[t[2]][k] - V[t[0]][k];
    }
    n = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (double& x : n) x /= len;
    return n;
  };
  // Edges -> the course triangles on them (in order), for neighbours and the ridge flags.
  std::map<std::pair<uint16_t, uint16_t>, std::vector<size_t>> edges;
  auto edge_key = [](uint16_t u, uint16_t v) { return std::make_pair(std::min(u, v), std::max(u, v)); };
  for (size_t i = 0; i < added.size(); ++i) {
    for (int k = 0; k < 3; ++k) edges[edge_key(added[i][k], added[i][(k + 1) % 3])].push_back(first + i);
  }
  Bytes new_entries;
  for (size_t i = 0; i < added.size(); ++i) {
    const auto& t = added[i];
    const size_t ti = first + i;
    const Point n = normal(t);
    std::array<uint32_t, 3> nb{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    const bool copied = disc_flags[i] >= 0;   // a copied triangle keeps its own flags (+ open edges at the cut)
    uint16_t flags = copied ? uint16_t(disc_flags[i]) : 0;
    for (int k = 0; k < 3; ++k) {
      size_t other = SIZE_MAX;
      for (size_t j : edges[edge_key(t[k], t[(k + 1) % 3])]) {
        if (j != ti) {
          other = j;
          break;
        }
      }
      if (other == SIZE_MAX) {  // an open edge
        flags |= uint16_t(1u << k);
        continue;
      }
      nb[k] = uint32_t(other);
      const auto& tj = added[other - first];
      for (uint16_t x : tj) {  // the neighbour's third corner below this plane = a ridge
        if (x == t[0] || x == t[1] || x == t[2]) continue;
        double s = 0;
        for (int q = 0; q < 3; ++q) s += (V[x][q] - V[t[0]][q]) * n[q];
        if (s < -1e-4 && !copied) flags |= uint16_t(1u << k);
        break;
      }
    }
    T.push_back({t[0], t[1], t[2], flags});
    for (uint32_t v : nb) pure3d::PutLe32(new_entries, v);
    for (int q = 0; q < 3; ++q) PutF(new_entries, 1.9);
    for (int q = 0; q < 3; ++q) PutF(new_entries, n[q]);
  }
  // Chunks back: corners, triangles, entries.
  Bytes data;
  pure3d::PutLe32(data, uint32_t(V.size()));
  for (const Point& p : V) {
    for (int k = 0; k < 3; ++k) PutF(data, p[k]);
  }
  corners->data = std::move(data);
  data.clear();
  pure3d::PutLe32(data, uint32_t(T.size()));
  for (const Tri& t : T) {
    Put16(data, t.a);
    Put16(data, t.b);
    Put16(data, t.c);
    Put16(data, t.flags);
  }
  triangles->data = std::move(data);
  data.assign(entries->data.begin(), entries->data.end());
  for (int i = 0; i < 4; ++i) data[i] = uint8_t(T.size() >> (8 * i));
  data.insert(data.end(), new_entries.begin(), new_entries.end());
  entries->data = std::move(data);
  // The tree, for every triangle, on the mesh's own grid.
  std::array<double, 3> scale;
  for (int k = 0; k < 3; ++k) scale[k] = hi[k] > lo[k] ? 65535.0 / (hi[k] - lo[k]) : 0.0;
  auto grid = [&](const Point& p, int k) {
    const double g = (p[k] - lo[k]) * scale[k];
    return std::clamp(int(g), 0, 65535);   // trunc toward zero, then 0..65535 (sub_8244BA60)
  };
  std::vector<GridBox> boxes;
  boxes.reserve(T.size());
  for (const Tri& t : T) {
    GridBox b;
    for (int k = 0; k < 3; ++k) {
      const int ga = grid(V[t.a], k), gb = grid(V[t.b], k), gc = grid(V[t.c], k);
      b.lo[k] = std::min({ga, gb, gc});
      b.hi[k] = std::max({ga, gb, gc});
    }
    boxes.push_back(b);
  }
  std::vector<uint16_t> words;
  if (!BuildTree(boxes, &words)) {
    REXLOG_WARN("Test course: the collision tree doesn't fit 65,536 words: left alone");
    return false;
  }
  data.clear();
  pure3d::PutLe32(data, uint32_t(words.size()));
  for (uint16_t w : words) Put16(data, w);
  tree->data = std::move(data);
  // Surface materials: (triangle, material) pairs for every triangle.
  {
    Bytes& m = materials->data;
    // These streams start with u32 0, THEN the name (u8 length + bytes) at +4, then u32 n.
    const size_t name_at = 4, count_at = name_at + 1 + m[name_at];
    const uint32_t cnt = U32(m, count_at);
    if (m.size() < count_at + 4 + 4 * size_t(cnt)) return false;
    Bytes out(m.begin(), m.begin() + count_at);
    pure3d::PutLe32(out, uint32_t(cnt + added.size()));
    out.insert(out.end(), m.begin() + count_at + 4, m.begin() + count_at + 4 + 2 * cnt);   // triangles
    for (size_t i = 0; i < added.size(); ++i) Put16(out, uint16_t(first + i));
    out.insert(out.end(), m.begin() + count_at + 4 + 2 * cnt, m.begin() + count_at + 4 + 4 * cnt);  // materials
    for (size_t i = 0; i < added.size(); ++i) Put16(out, added_material[i]);
    out.insert(out.end(), m.begin() + count_at + 4 + 4 * cnt, m.end());
    m = std::move(out);
  }
  *patched = pure3d::ToBytes(file);
  REXLOG_INFO("Test course: {} triangles added to {} ({} -> {}), tree {} words", added.size(), kPackage, first,
              T.size(), words.size());
  const std::string dump = REXCVAR_GET(debug_test_course_dump);
  if (!dump.empty()) {
    std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(patched->data()),
                                                std::streamsize(patched->size()));
  }
  return true;
}

// ---------------------------------------------------------------------------
// THE COURSE'S OBJECTS (2026-11-19, findings/69 s.8): real game objects in the
// gap lanes, made by the spawn code (cheats/spawn.h, its exact-spot form) once
// per level load, 2 s after player 1's Crash first exists (every PC of an
// online session reaches that frame alike). Both templates have a package of
// their own in GlobalPackages, so they load anywhere:
//   * WorldObjects:HangingRope: the rope the levels hang (Ice Kingdom, Junk
//     have their own variants); ~15 units long, hanging DOWN from its spot,
//     grabbed by jumping at it (tested on Wumpa Island: Crash slid down it).
//   * StaticProps:env_Whumpa:prp_pformL1_platformA: a Wumpa platform, still
//     (its movement comes from the level's own scripts, not the template).
// ---------------------------------------------------------------------------
struct CourseObject {
  const char* lane;      // the gap lane it goes into (its middle)
  const char* template_name;
  double height;         // its spot above kBase
};
constexpr CourseObject kObjects[] = {
    {"ropegap", "WorldObjects:HangingRope", 14.0},  // bottom ~1 unit below the pads (15 long)
    {"platgap", "StaticProps:env_Whumpa:prp_pformL1_platformA", -0.5},
};
constexpr double kObjectsAfterSpawn = 2.0;  // seconds of game time after player 1's Crash exists

int g_frames_in_play = -1;     // frames since player 1's Crash appeared, -1 = not in play
bool g_objects_placed = false;

}  // namespace

void Tick(bool in_play, double frame_seconds, const float* player1) {
  if (!REXCVAR_GET(test_course) || !g_course_built) return;
  // The course is only in Wumpa Island's piece; the patch flag stays set
  // after a level change, so: player 1 within 400 units of the lanes too.
  const double dx = player1 ? player1[0] - (kX0 + 15) : 1e9, dz = player1 ? player1[2] - (kZ0 + 45) : 1e9;
  if (in_play && dx * dx + dz * dz > 400.0 * 400.0) in_play = false;
  if (!in_play) {
    g_frames_in_play = -1;
    g_objects_placed = false;
    return;
  }
  static double since = 0.0;
  if (g_frames_in_play < 0) {
    g_frames_in_play = 0;
    since = 0.0;
  }
  since += frame_seconds;
  if (g_objects_placed || since < kObjectsAfterSpawn) return;
  g_objects_placed = true;
  // The start lane (test_course_start): every playing player at its start,
  // 1.5 units apart across it (same frame on every PC of a session).
  const std::string& start = REXCVAR_GET(test_course_start);
  for (int k = 0; !start.empty() && k < int(std::size(kLanes)); ++k) {
    const Lane& l = kLanes[k];
    if (start != l.name) continue;
    for (int p = 0; p < 4; ++p) {
      const uint32_t body = more_players::CharacterOfPlayer(p);
      if (!body || !menu_input::PlayerPlays(p)) continue;
      const float at[3] = {float(LaneX0(k) + kPad / 2), float(kBase + HeightAt(l, ProfileOf(l), 0.0) + 0.5),
                           float(LaneZ0(k) + kWidth / 2 + 1.5 * p)};
      if (debug_console::PutBodyAt(body, at)) {
        REXLOG_INFO("Test course: player {} put at lane {}'s start ({:.2f} {:.2f} {:.2f})", p + 1, l.name, at[0],
                    at[1], at[2]);
      }
    }
  }
  for (const CourseObject& o : kObjects) {
    for (int k = 0; k < int(std::size(kLanes)); ++k) {
      const Lane& l = kLanes[k];
      if (std::string(l.name) != o.lane) continue;
      const float at[3] = {float(LaneX0(k) + kPad + ProfileOf(l).run / 2), float(kBase + o.height),
                           float(LaneZ0(k) + kWidth / 2)};
      spawn::Request(o.template_name, 0, 1, false, at, /*as_cheat=*/false);
      REXLOG_INFO("Test course: {} placed in lane {} at {:.2f} {:.2f} {:.2f}", o.template_name, l.name, at[0], at[1],
                  at[2]);
    }
  }
}

void Register() {
  if (!REXCVAR_GET(test_course)) return;
  data_patcher::Register("test course", kCategoryRoom, kPackage, &PatchPackage);
  for (int k = 0; k < int(std::size(kLanes)); ++k) {  // where each lane starts (course.py's lanes())
    const Lane& l = kLanes[k];
    const double y = kBase + HeightAt(l, ProfileOf(l), 0.0);
    REXLOG_INFO("Test course: lane {} starts at {:.2f} {:.2f} {:.2f} (goto 1 ...)", l.name, LaneX0(k) + kPad / 2,
                y + 0.5, LaneZ0(k) + kWidth / 2);
  }
}

}  // namespace test_course
