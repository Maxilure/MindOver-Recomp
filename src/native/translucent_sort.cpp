// =============================================================================
// translucent_sort.cpp -- see-through objects drawn in a steady order
//                         (--translucent_sort_fix, on by default)
// =============================================================================
//
// THE BUG (2026-10-22, docs/findings/48): in the ice caves a frozen figure
// behind a sheet of clear ice FLICKERS (gone for a frame or several) while
// explosions are on screen, e.g. a masked co-op player firing into the
// tunnel. Same on the emulated picture = the ORIGINAL game's behaviour.
//
// HOW THE GAME DRAWS SEE-THROUGH THINGS: Pure3D's TranslucentTraversal
// (RTTI, vtable 0x8200D658) collects one 100-byte ENTRY per see-through
// primitive group (slot 6, sub_823AD538) and, in slot 3 (sub_823ADA00),
// sorts an array of pointers to them with the C library's qsort:
//     qsort(entries = traversal+0x28, count = traversal+0x18, 4, sub_823AD928)
// then draws them in that order (far ones first, like a painter).
// Entry layout (written by sub_823AD538):
//   +0x00 bounding sphere centre x y z in VIEW space (z = depth, bigger =
//         farther), +0x0C its radius
//   +0x10 bounding box min x y z, +0x1C box max x y z (view space too)
//   +0x28 the drawable, +0x30 its prim-group record (+4 = the PrimGroup)
//   +0x58 byte: the sort LAYER (a float x 64; higher layers draw first)
//
// THE GAME'S COMPARE (sub_823AD928, Compare() below), a and b:
//   0. different layers         -> the higher layer first
//   1. spheres apart in depth   -> the farther one first
//   2. boxes apart in depth     -> the farther one first
//   3. otherwise (they overlap) -> the SMALLER sphere first; equal radius ->
//      the farther centre first
// Each rule is sensible alone, but together they can go round in a circle
// (a before b, b before c, c before a): rule 3 for one pair, rules 1/2 for
// the others. qsort then gives an order that depends on EVERY entry in the
// list, not just on the two objects. Found by a dump of the list (this
// file's debug output) in the cave of the flicker: the frozen figure is a
// small entry (radius 2.5, depth 36-46) INSIDE a big flat ice sheet's
// bounds (radius 16, depth 27-56): rule 3 puts the figure first, which
// shows it through the ice. An explosion's particle systems add entries
// that close a circle through rules 1-2; in those frames qsort put the
// sheet first. The sheet writes depth, so the figure behind it then fails
// the depth test = gone (draw lists of a photo burst: 18 of 18 frames,
// "figure first" = visible, "sheet first" = gone, the camera identical).
//
// THE FIX: every pair is decided by the game's OWN compare (an exact copy),
// and the order is built from those decisions (a topological sort): each
// step draws the entry that no remaining entry must come before. Without a
// circle that is exactly the order the game's rules ask for, the same one
// qsort gives in calm frames. Only when the rules go round in a circle does
// every remaining entry have someone "before" it; then the entry whose box
// NEAR edge is farthest goes next (an entry entirely behind another always
// has the farther near edge, so this follows rules 1-2), and the rest goes
// on from there. In the cave: the explosion entries behind the figure have
// the farther near edges, so they go first and break the circle; then the
// figure, then the sheet, as rule 3 wants.
//
// A FIRST VERSION (same day) ranked every entry by its near edge alone. It
// fixed the explosion case but made a new flicker while the camera moved:
// the ice sheet and a second ice layer around the figure start at the same
// depth (27.05), so rounding picked their order frame by frame (rule 3
// always says "sheet first"; the sharp/faint alternation of the figure seen
// through them). Keeping the game's own pairwise decisions avoids such ties.
//
// --translucent_sort_fix=false = the game's own qsort (for comparing).
// =============================================================================

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "../guest_memory.h"
#include "../pddi/trace.h"
#include "guest.h"

REXCVAR_DEFINE_BOOL(translucent_sort_fix, true, "CrashMoM",
                    "See-through objects (ice, glass, particles) drawn in a steady order: no "
                    "flicker when explosions are on screen (false = the game's own sort)");

extern "C" REX_FUNC(__imp__sub_823ADA00);  // TranslucentTraversal slot 3: the qsort

namespace {

using native::guest::Load32;
using native::guest::Load8;
using native::guest::LoadFloat;

// Entry fields (see the header).
constexpr uint32_t kCentreZ = 0x08;
constexpr uint32_t kRadius = 0x0C;
constexpr uint32_t kBoxMinZ = 0x18;
constexpr uint32_t kBoxMaxZ = 0x24;
constexpr uint32_t kDrawable = 0x28;
constexpr uint32_t kGroupRecord = 0x30;
constexpr uint32_t kLayer = 0x58;
// The traversal's list: pointer array and count.
constexpr uint32_t kListArray = 0x28;
constexpr uint32_t kListCount = 0x18;

// Debug aid: while a PDDI trace records a frame (--debug_pddi_trace_dir +
// F10 / its trigger file), the sorted list goes into the log, one line per
// entry, in drawing order. How the flicker's objects were identified.
void DumpList(const uint8_t* base, uint32_t traversal) {
  const uint32_t list = Load32(base, traversal + kListArray);
  const uint32_t count = Load32(base, traversal + kListCount);
  if (!count) return;
  REXLOG_INFO("TranslucentSort: traversal {:08X}, {} entries in drawing order ({}):", traversal,
              count, REXCVAR_GET(translucent_sort_fix) ? "our order" : "the game's qsort");
  for (uint32_t i = 0; i < count && list; ++i) {
    const uint32_t e = Load32(base, list + 4 * i);
    const uint32_t record = Load32(base, e + kGroupRecord);
    const uint32_t group = record ? Load32(base, record + 4) : 0;
    REXLOG_INFO("TranslucentSort:  {:3} layer {:2} centre ({:.2f} {:.2f} {:.2f}) r {:.2f} "
                "z {:.2f}..{:.2f} drawable {:08X} group {:08X}",
                i, Load8(base, e + kLayer), LoadFloat(base, e + 0), LoadFloat(base, e + 4),
                LoadFloat(base, e + kCentreZ), LoadFloat(base, e + kRadius),
                LoadFloat(base, e + kBoxMinZ), LoadFloat(base, e + kBoxMaxZ),
                Load32(base, e + kDrawable), group);
  }
}

// What the compare reads of one entry, copied to the host once.
struct Entry {
  uint32_t guest;  // the entry's guest address, written back in the new order
  uint8_t layer;
  float centre_z, radius, min_z, max_z;
};

// The game's compare sub_823AD928, line for line (checked against the
// disassembly): > 0 = a draws AFTER b, < 0 = a draws BEFORE b. It never
// answers 0. (The layer bytes are compared as byte x 1/64 floats there:
// the same order as the bytes.)
int Compare(const Entry& a, const Entry& b) {
  if (a.layer != b.layer) return a.layer < b.layer ? 1 : -1;
  if (a.centre_z + a.radius <= b.centre_z - b.radius) return 1;   // 1. a's sphere nearer
  if (b.centre_z + b.radius <= a.centre_z - a.radius) return -1;
  if (a.max_z <= b.min_z) return 1;                               // 2. a's box nearer
  if (b.max_z <= a.min_z) return -1;
  if (a.radius > b.radius) return 1;                              // 3. bigger one later
  if (a.radius < b.radius) return -1;
  return a.centre_z < b.centre_z ? 1 : -1;                        //    equal: nearer later
}

}  // namespace

extern "C" REX_FUNC(sub_823ADA00) {
  const uint32_t traversal = ctx.r3.u32;
  if (!REXCVAR_GET(translucent_sort_fix)) {
    __imp__sub_823ADA00(ctx, base);
  } else {
    const uint32_t list = Load32(base, traversal + kListArray);
    const uint32_t n = Load32(base, traversal + kListCount);
    if (list && n > 1) {
      // Main thread only; kept between frames to avoid reallocating.
      static thread_local std::vector<Entry> entries;
      static thread_local std::vector<uint8_t> before;  // n x n: [i * n + j] = i draws before j
      static thread_local std::vector<uint32_t> waiting;  // entries that must draw before i
      static thread_local std::vector<uint8_t> done;
      entries.clear();
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = Load32(base, list + 4 * i);
        entries.push_back({e, Load8(base, e + kLayer), LoadFloat(base, e + kCentreZ),
                           LoadFloat(base, e + kRadius), LoadFloat(base, e + kBoxMinZ),
                           LoadFloat(base, e + kBoxMaxZ)});
      }
      // Every pair decided once by the game's compare (a few thousand
      // compares for the usual 40-150 entries).
      before.assign(size_t(n) * n, 0);
      waiting.assign(n, 0);
      for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = i + 1; j < n; ++j) {
          if (Compare(entries[i], entries[j]) < 0) {
            before[size_t(i) * n + j] = 1;
            ++waiting[j];
          } else {
            before[size_t(j) * n + i] = 1;
            ++waiting[i];
          }
        }
      }
      // Build the order: next = an entry nobody remaining must precede
      // (without a circle there is exactly one); in a circle, choose within
      // the highest remaining LAYER (layers never form circles: the game
      // draws them strictly one after the other), the farthest near edge;
      // near edges within kSameEdge count as equal (layers of one ice wall
      // share their front, 29.04 and 29.04), and then the game's own
      // decision for that pair wins.
      constexpr float kSameEdge = 0.01f;  // game units (about a centimetre)
      done.assign(n, 0);
      for (uint32_t k = 0; k < n; ++k) {
        int top_layer = -1;
        for (uint32_t i = 0; i < n; ++i) {
          if (!done[i] && entries[i].layer > top_layer) top_layer = entries[i].layer;
        }
        uint32_t best = UINT32_MAX;
        for (uint32_t i = 0; i < n; ++i) {
          if (done[i] || entries[i].layer != top_layer) continue;
          if (best == UINT32_MAX) {
            best = i;
            continue;
          }
          const bool free_i = waiting[i] == 0, free_best = waiting[best] == 0;
          const float edge_i = entries[i].min_z, edge_best = entries[best].min_z;
          if (free_i != free_best) {
            if (free_i) best = i;
          } else if (!(std::fabs(edge_i - edge_best) <= kSameEdge)) {
            if (edge_i > edge_best) best = i;  // (a NaN edge never wins here)
          } else if (before[size_t(i) * n + best]) {
            best = i;  // equal edges: the game's own decision for these two
          }
        }
        done[best] = 1;
        rex::memory::store_and_swap<uint32_t>(GuestPtr(base, list + 4 * k), entries[best].guest);
        for (uint32_t j = 0; j < n; ++j) {
          if (!done[j] && before[size_t(best) * n + j]) --waiting[j];
        }
      }
    }
  }
  if (pddi::trace::Recording()) DumpList(base, traversal);
}
