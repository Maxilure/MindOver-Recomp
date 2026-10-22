// =============================================================================
// translucent_sort.cpp -- see-through objects drawn in one steady order
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
// THE GAME'S COMPARE (sub_823AD928), for a and b in the same layer:
//   1. spheres apart in depth  -> the farther one first
//   2. boxes apart in depth    -> the farther one first
//   3. otherwise (they overlap) -> the SMALLER sphere first; equal radius ->
//      the farther centre first
// Each rule is sensible alone, but together they can go round in a circle
// (a before b, b before c, c before a): rule 3 for one pair, rules 1/2 for
// the others. qsort then gives an order that depends on EVERY entry in the
// list, not just on the two objects. Found by a dump of the list (this
// file's debug output) in the cave of the flicker: the frozen figure is a
// small entry (radius 2.5, depth 36-46) INSIDE a big flat ice sheet's
// bounds (radius 16, depth 27-56); compared directly the figure goes first
// (rule 3), which shows it through the ice. An explosion's particle
// systems add entries that, with rules 1-2, close a circle; in those
// frames qsort put the sheet first. The sheet writes depth, so the figure
// behind it then fails the depth test = gone (draw lists of the user's
// photo burst: 18 of 18 frames, "figure first" = visible, "sheet first" =
// gone, the camera identical).
//
// THE FIX: our own sort with ONE ranking that can't go round in a circle:
//   layer (high first), then the box's NEAR edge (min z, farther first),
//   then radius (smaller first: the game's rule 3 for equal near edges),
//   then the sphere centre (farther first), then the game's own collection
//   order (a stable sort).
// It agrees with the game's rules 1 and 2 always (an entry entirely behind
// another has the farther near edge), and with rule 3 whenever one object
// sits inside the other (the inner one's near edge is the farther one), as
// the frozen figure inside the ice does. Only partly overlapping pairs can
// come out differently from what qsort happened to give, and those had no
// fixed answer before either. The draw order now depends only on where the
// objects are: no more flicker. --translucent_sort_fix=false = the game's
// own qsort (for comparing).
// =============================================================================

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../guest_memory.h"
#include "../pddi/trace.h"
#include "guest.h"

REXCVAR_DEFINE_BOOL(translucent_sort_fix, true, "CrashMoM",
                    "See-through objects (ice, glass, particles) drawn in one steady order: no "
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
// entry, in drawing order. How the flicker's two objects were identified.
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

// One entry's ranking (see the header): read once, then sorted on the host.
struct Key {
  uint32_t entry;  // guest pointer, written back in the new order
  uint8_t layer;
  float near_z;    // box min z: the near edge
  float radius;
  float centre_z;
};

// A NaN would break the ranking (every comparison false): count it as 0.
float Sane(float v) { return std::isnan(v) ? 0.0f : v; }

}  // namespace

extern "C" REX_FUNC(sub_823ADA00) {
  const uint32_t traversal = ctx.r3.u32;
  if (!REXCVAR_GET(translucent_sort_fix)) {
    __imp__sub_823ADA00(ctx, base);
  } else {
    const uint32_t list = Load32(base, traversal + kListArray);
    const uint32_t count = Load32(base, traversal + kListCount);
    if (list && count > 1) {
      static thread_local std::vector<Key> keys;  // main thread only; kept to avoid reallocating
      keys.clear();
      keys.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = Load32(base, list + 4 * i);
        keys.push_back({e, Load8(base, e + kLayer), Sane(LoadFloat(base, e + kBoxMinZ)),
                        Sane(LoadFloat(base, e + kRadius)), Sane(LoadFloat(base, e + kCentreZ))});
      }
      // "a draws before b". Stable: equal keys keep the collection order.
      std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.layer != b.layer) return a.layer > b.layer;
        if (a.near_z != b.near_z) return a.near_z > b.near_z;
        if (a.radius != b.radius) return a.radius < b.radius;
        return a.centre_z > b.centre_z;
      });
      for (uint32_t i = 0; i < count; ++i) {
        rex::memory::store_and_swap<uint32_t>(GuestPtr(base, list + 4 * i), keys[i].entry);
      }
    }
  }
  if (pddi::trace::Recording()) DumpList(base, traversal);
}
