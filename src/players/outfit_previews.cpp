// =============================================================================
// players/outfit_previews.cpp -- see outfit_previews.h
// =============================================================================
//
// HOW THE GAME'S ONE PREVIEW WORKS (read with xexdis + Ghidra, 2026-10-23;
// CSkinScreenAction = "the screen", vtable 0x82024D9C: slot 4 Enter
// 0x820E0188, slot 5 Update 0x820E0858 (f1 = dt), slot 6 Exit 0x820E0EB0):
//
//   SHOWING AN OUTFIT, sub_820E1228(screen, list index, r5): the shown
//   entry lives in ONE global, 0x825A0BA4 (+0 skin, +12 the template's name
//   key = hash of "Characters:" + the skin's name (sub_8229F7F8), +16 byte
//   "we hold a reference"). It gives the old entry's package reference back
//   (sub_822E4650) and takes one on the new (sub_822E44B8 = the inventory
//   section named by the key; loads it if needed). A locked entry = key 0.
//
//   STATE 3 (the screen's input state, sub_820E1C70, every frame):
//     * no preview yet (+92 == 0) and the key's section READY
//       (sub_822E4D88): section = sub_822E4680(inventory, &key), then
//       BUILD sub_820E0A30(screen, section):
//         - sub_822E7788(inventory, section) ... sub_822E77B8(inventory)
//           around it (new things come from that section), heap 8 current
//           (sub_823574E8);
//         - controller = sub_8211AA50(alloc 168, 0, animation set, page)
//           (our midasm OutfitsPreviewCharacter at 0x820E0AA8 picks the set
//           per outfit);
//         - actor = sub_820B0670(alloc 56, 0, controller, 0) -> screen +92,
//           its vtable slot 0 (set up), world matrix (+44) = identity with
//           the position from the table 0x82505E7C + 12 x page (sub_824239E0);
//         - sub_822E6B60(inventory, actor) = into the world (drawn);
//         - +104 = sub_8210AC90(controller), sub_82102F28(+104, actor, 0);
//         - +228 bit 0x40 cleared (= not animated yet), +116 = 0.
//     * the NEXT frame (bit 0x40 clear): sub_82102F28(+104, actor, 1); if
//       *(+104)+80: sub_820C8138(sub_822B79F0(it)); then sub_820E0BC8(screen,
//       section) = the ANIMATION: player = sub_82351340(alloc 172, the
//       actor's variable 38) -> +100 (and into variable 39), a random idle
//       sub_820E0C98 (clip -> +96, from the page's idle list, looked up in the
//       section), first step, and the menu's 3 lights (sub_823ACAB8 / AC90 /
//       ACAE0 on game+76 +444: the same constants every time); bit 0x40 set.
//     * after that, every frame: clip (+96) finished (vtable slot 8) -> a new
//       random idle; sub_82350F88(+100, dt) + sub_82351710(+100) = advance +
//       apply the animation; controller (+104) vtable slot 4 (it, actor,
//       r6 = 3, f1 = dt).
//   EXIT, sub_820E0EB0 -> sub_820E1180(screen): clip out of the player
//   (sub_823516D0) + destroyed, sub_820B0C58(actor, 0) (out of the world),
//   actor destroyed (its controller with it), +92 = 0; then the global's
//   package reference given back.
//
// WHY A "SHADOW SCREEN": everything above touches only the screen's +92
// actor, +96 clip, +100 animation player, +104 controller part, +116
// counter, +132 page (layout 0/1: picks the place and the idle list) and
// +228 flags; the package is passed in. So each of our previews owns a
// zeroed 256-byte block in guest memory standing in for the screen, and the
// game's own functions run on it unchanged (one per player). The package
// reference is ours too (one per preview, on its own key), so four previews
// = up to four sections held; two previews of the same template share the
// section (the inventory counts references).
//
// THE ZONE'S CHARACTER LIST (found 2026-10-23: with 2-4 previews built,
// only the FIRST was ever drawn). An actor added to the world
// (sub_822E6B60 -> sub_820B1B30) joins the inventory's current zone
// (inventory +0x24; its actor list at +0x1D8 is roomy); its drawable joins
// one of the zone's DRAW lists when its render behaviour sets up:
// sub_822F1CF8 (zone r3, drawable r4) pushes into +76 (characters; begin
// +76, end +80, capacity +84), +92 (drawable type 0) or +108, plus +124 for
// some, and a push past the capacity is DROPPED without a word
// (0x822F1DD8: end >= capacity -> skip). The zone's draw (sub_822F29B8 ->
// sub_822F2AF8, frustum test + draw per entry) only sees those lists.
// The chest's zone has room for exactly TWO characters (storage of 8
// bytes): the game's own preview + one. Seen with the console: `who` on the
// list's entries caught the push (sub_822F1CF8+0xF8, from the render
// behaviour's set-up sub_82185E60), the list read back as 2 entries =
// capacity. So while our previews are up the zone's +76 list gets a bigger
// storage of ours (entries copied over), and at teardown, once our models
// are gone, its own storage comes back with whatever is left (the game
// frees its own block with the zone, never ours).
// =============================================================================

#include "outfit_previews.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "outfits.h"

REXCVAR_DEFINE_STRING(debug_outfit_previews, "", "CrashMoM",
                      "Debug: at the chest, one 3D preview per listed outfit, e.g. \"0,11,9,4\" (outfit "
                      "numbers: 0 Crash .. 9 Coco, 10 Old School, 11 Carbon Crash, 12-17 Crash in colours); empty = off (the co-op picker drives the previews)");

extern "C" REX_FUNC(__imp__sub_8229F7F8);  // skin r3 -> its name
extern "C" REX_FUNC(__imp__sub_82357020);  // hash of a string (r3 string, r4 seed 0)
extern "C" REX_FUNC(__imp__sub_822E3FA8);  // the inventory manager
extern "C" REX_FUNC(__imp__sub_822E3FC8);  // inventory: does the section named *r4 exist
extern "C" REX_FUNC(__imp__sub_822E4D88);  // inventory: is it ready (loaded)
extern "C" REX_FUNC(__imp__sub_822E44B8);  // inventory: take a reference (loads it)
extern "C" REX_FUNC(__imp__sub_822E4650);  // inventory: give the reference back
extern "C" REX_FUNC(__imp__sub_822E4680);  // inventory: the section named *r4
extern "C" REX_FUNC(__imp__sub_820E0A30);  // skin screen: BUILD the preview (r4 section)
extern "C" REX_FUNC(__imp__sub_820E0BC8);  // skin screen: start its animation (r4 section)
extern "C" REX_FUNC(sub_820E0C98);         // skin screen: random idle (OUR override, outfits.cpp)
extern "C" REX_FUNC(__imp__sub_820E1180);  // skin screen: destroy the preview
extern "C" REX_FUNC(__imp__sub_820E0EB0);  // skin screen: Exit
extern "C" REX_FUNC(__imp__sub_82102F28);
extern "C" REX_FUNC(__imp__sub_822B79F0);
extern "C" REX_FUNC(__imp__sub_820C8138);
extern "C" REX_FUNC(__imp__sub_82350F88);  // animation player: advance (f1 dt)
extern "C" REX_FUNC(__imp__sub_82351710);
extern "C" REX_FUNC(__imp__sub_8227F130);  // a game heap's used / total bytes  // animation player: apply

namespace outfit_previews {
namespace {

constexpr uint32_t kCharactersPrefix = 0x82024D8C;  // "Characters:"
constexpr uint32_t kHeaps = 0x8256F1D0;             // the game's heaps (16 bytes each)
constexpr uint32_t kInventoryOf = 100;         // game +100: the inventory manager
constexpr uint32_t kCurrentZone = 0x24;        // inventory: the zone new actors join
constexpr uint32_t kZoneCharacters = 76;       // zone: draw list begin / end / capacity
constexpr uint32_t kGrownSlots = 32;           // our storage for it
// Shadow screen fields (the screen's own offsets, see the header comment).
constexpr uint32_t kActor = 92, kClip = 96, kPlayer = 100, kController = 104, kPage = 132,
                   kFlags = 228, kShadowBytes = 256;
constexpr uint32_t kAnimated = 0x40;  // +228: animation started
constexpr uint32_t kMatrix = 44;      // actor: world matrix pointer (position at +48)
constexpr float kPreviewY = 0.1f;     // the game's height (table 0x82505E7C)
// Across the screen: the game puts page 1's preview at x = +1 (right half),
// page 2's at -1. kWorldWidth = world units from the left edge to the right
// edge at the models' depth: measured on F10 photos (2026-10-23) of four
// previews at x = -1.5 / -0.5 / 0.5 / 1.5: their middles at 0.26 / 0.42 /
// 0.58 / 0.73 of the picture's width = 0.159 of the width per unit, the
// screen's middle at x = 0 (the camera looks straight at it).
constexpr float kWorldWidth = 6.3f;

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void WriteFloat(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); Write32(a, b); }

struct Args {
  uint32_t r3 = 0, r4 = 0, r5 = 0, r6 = 0;
  double f1 = 0;
};
uint32_t Call(PPCFunc* f, PPCContext& ctx, uint8_t* base, const Args& a) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = a.r3; ctx.r4.u64 = a.r4; ctx.r5.u64 = a.r5; ctx.r6.u64 = a.r6; ctx.f1.f64 = a.f1;
  f(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
uint32_t Virtual(PPCContext& ctx, uint8_t* base, uint32_t object, int slot, Args a) {
  PPCFunc* f = rex::runtime::ResolveIndirectFunction(Read32(Read32(object) + 4 * slot));
  if (!f) return 0;
  a.r3 = object;
  return Call(f, ctx, base, a);
}

// One preview.
struct Preview {
  // wanted (set by the API)
  int want_outfit = -1;
  float x = 0.5f;
  float scale = 1.0f;
  float y = kPreviewY;  // world height of the feet (the game's 0.1)
  bool locked = false;
  // what is there
  uint32_t shadow = 0;     // guest: the shadow screen (kShadowBytes) + 2 keys at +240 / +248
  int built_outfit = -1;   // the model's outfit, -1 = none
  int pending_outfit = -1; // loading this one (its reference taken)
  uint32_t built_key = 0, pending_key = 0;  // template keys we hold a reference on (0 = none)
  int frames_loading = 0;
};
constexpr uint32_t kKeyBuilt = 240, kKeyPending = 248;  // inside the shadow block: 8-byte key slots

struct State {
  std::array<Preview, kMaxPreviews> p;
  int count = 0;       // wanted previews (0 = off)
  bool active = false;
  int building = -1;   // the outfit being built right now
  uint32_t scratch = 0;  // guest: a 128-byte string buffer
  std::string debug_seen;
  bool debug_driving = false;
  // The zone list we enlarged (0 = none): its own storage, to give back.
  uint32_t zone = 0, zone_begin = 0, zone_capacity = 0, grown = 0;
} g;

// Room for every preview in the zone's character list (see the header).
void GrowZoneList() {
  const uint32_t game = Read32(0x8259B190);
  const uint32_t inv = game ? Read32(game + kInventoryOf) : 0;
  const uint32_t zone = inv ? Read32(inv + kCurrentZone) : 0;
  if (!zone || zone == g.zone) return;
  const uint32_t list = zone + kZoneCharacters;
  const uint32_t begin = Read32(list), end = Read32(list + 4), capacity = Read32(list + 8);
  const uint32_t used = (end - begin) / 4;
  if (capacity - begin >= 4 * (used + kMaxPreviews + 1)) return;  // roomy already
  if (!g.grown) g.grown = rex::system::kernel_memory()->SystemHeapAlloc(4 * kGrownSlots);
  if (!g.grown || used > kGrownSlots - kMaxPreviews) return;
  std::memset(Guest(g.grown), 0, 4 * kGrownSlots);
  if (used) std::memcpy(Guest(g.grown), Guest(begin), 4 * used);
  g.zone = zone;
  g.zone_begin = begin;
  g.zone_capacity = capacity;
  Write32(list, g.grown);
  Write32(list + 4, g.grown + 4 * used);
  Write32(list + 8, g.grown + 4 * kGrownSlots);
  REXLOG_INFO("Outfit previews: zone {:08X}'s character list {} -> {} places", zone, (capacity - begin) / 4,
              kGrownSlots);
}
// Its own storage back, with what's left in the list (our models gone).
void RestoreZoneList() {
  if (!g.zone) return;
  const uint32_t list = g.zone + kZoneCharacters;
  if (Read32(list) != g.grown) {  // (someone else changed it: leave it)
    g.zone = 0;
    return;
  }
  const uint32_t used = (Read32(list + 4) - g.grown) / 4;
  const uint32_t room = (g.zone_capacity - g.zone_begin) / 4;
  if (used > room) {
    REXLOG_WARN("Outfit previews: zone list still holds {} (room {}): kept ours", used, room);
    return;
  }
  if (used) std::memcpy(Guest(g.zone_begin), Guest(g.grown), 4 * used);
  Write32(list, g.zone_begin);
  Write32(list + 4, g.zone_begin + 4 * used);
  Write32(list + 8, g.zone_capacity);
  REXLOG_INFO("Outfit previews: zone {:08X}'s character list given back ({} left)", g.zone, used);
  g.zone = 0;
}

uint32_t Inventory(PPCContext& ctx, uint8_t* base, uint32_t key_addr) {
  return Call(__imp__sub_822E3FA8, ctx, base, {0, key_addr});
}

// The template key of an outfit: the hash of "Characters:" + the game
// skin's name (what sub_820E1228 makes at 0x820E12C0..0x820E1308). Carbon
// and the colours = Crash's model (game skin 0).
uint32_t TemplateKey(PPCContext& ctx, uint8_t* base, int outfit) {
  const uint32_t skin = outfits::IsCarbon(outfit) ? 0u : uint32_t(outfit);
  const uint32_t name = Call(__imp__sub_8229F7F8, ctx, base, {skin});
  if (!name || !g.scratch) return 0;
  std::string full = "Characters:";
  for (uint32_t i = 0; i < 100; ++i) {
    const char c = char(*Guest(name + i));
    if (!c) break;
    full.push_back(c);
  }
  std::memcpy(Guest(g.scratch), full.c_str(), full.size() + 1);
  return Call(__imp__sub_82357020, ctx, base, {g.scratch, 0});
}

void TakeRef(PPCContext& ctx, uint8_t* base, uint32_t slot, uint32_t key) {
  Write32(slot, key);
  Write32(slot + 4, 0);
  const uint32_t inv = Inventory(ctx, base, slot);
  if (inv && (Call(__imp__sub_822E3FC8, ctx, base, {inv, slot}) & 0xFF)) {
    Call(__imp__sub_822E44B8, ctx, base, {inv, slot});
  } else {
    Write32(slot, 0);  // no such section: nothing held
  }
}
void GiveRef(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  if (!Read32(slot)) return;
  const uint32_t inv = Inventory(ctx, base, slot);
  if (inv) Call(__imp__sub_822E4650, ctx, base, {inv, slot});
  Write32(slot, 0);
}
bool Ready(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  if (!Read32(slot)) return false;
  const uint32_t inv = Inventory(ctx, base, slot);
  return inv && (Call(__imp__sub_822E4D88, ctx, base, {inv, slot}) & 0xFF);
}

// The game's heaps, for the memory question (logged at each build): the
// game's own report sub_8227F130(heap, &used, &total, &3rd) per heap the
// table 0x8256F1D0 (16 bytes each, sub_823575F0) holds, in KB.
std::string HeapReport(PPCContext& ctx, uint8_t* base) {
  std::ostringstream s;
  if (!g.scratch) return s.str();
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t heap = Read32(kHeaps + 16 * i);
    if (heap < 0x40000000) continue;
    Call(__imp__sub_8227F130, ctx, base, {i, g.scratch + 64, g.scratch + 68, g.scratch + 72});
    s << " h" << i << " " << (Read32(g.scratch + 64) >> 10) << "/" << (Read32(g.scratch + 68) >> 10);
  }
  auto* phys = rex::system::kernel_memory()->LookupHeapByType(true, 4096);
  if (phys) s << " | physical free " << (uint64_t(phys->GetUnreservedPageCount()) * 4096 >> 20) << " MB";
  return s.str();
}

// The actor's world matrix (4 x 4 floats, row-major, the position in the
// last row = +48): its size on the diagonal (1 = the game's), its place.
void SetPosition(uint32_t actor, float wx, float wy, float scale = 1.0f) {
  const uint32_t m = Read32(actor + kMatrix);
  if (m < 0x40000000) return;
  WriteFloat(m + 0, scale);
  WriteFloat(m + 20, scale);
  WriteFloat(m + 40, scale);
  WriteFloat(m + 48, wx);
  WriteFloat(m + 52, wy);
  WriteFloat(m + 56, 0.0f);
}

// The model goes (the game's own sub_820E1180 on our shadow screen).
void Destroy(PPCContext& ctx, uint8_t* base, Preview& pv) {
  if (pv.shadow && Read32(pv.shadow + kActor)) {
    Call(__imp__sub_820E1180, ctx, base, {pv.shadow});
    *Guest(pv.shadow + kFlags) = 0;
  }
  if (pv.shadow) GiveRef(ctx, base, pv.shadow + kKeyBuilt);
  pv.built_outfit = -1;
}
void Drop(PPCContext& ctx, uint8_t* base, Preview& pv) {
  Destroy(ctx, base, pv);
  if (pv.shadow) GiveRef(ctx, base, pv.shadow + kKeyPending);
  pv.pending_outfit = -1;
  pv.want_outfit = -1;
}

// Build the pending outfit (its section is ready): the old model goes,
// sub_820E0A30 builds the new one on the shadow screen.
void Build(PPCContext& ctx, uint8_t* base, Preview& pv, int index) {
  Destroy(ctx, base, pv);
  // The pending reference becomes the built one.
  std::memcpy(Guest(pv.shadow + kKeyBuilt), Guest(pv.shadow + kKeyPending), 8);
  Write32(pv.shadow + kKeyPending, 0);
  const uint32_t inv = Inventory(ctx, base, pv.shadow + kKeyBuilt);
  const uint32_t section = inv ? Call(__imp__sub_822E4680, ctx, base, {inv, pv.shadow + kKeyBuilt}) : 0;
  if (!section) {
    GiveRef(ctx, base, pv.shadow + kKeyBuilt);
    pv.pending_outfit = -1;
    return;
  }
  // Page = which idle list / animation set (Crash's 0, Coco's 1).
  const bool coco = !outfits::IsCarbon(pv.pending_outfit) && (pv.pending_outfit == 9 || pv.pending_outfit == 10);
  Write32(pv.shadow + kPage, coco ? 1u : 0u);
  g.building = pv.pending_outfit;
  Call(__imp__sub_820E0A30, ctx, base, {pv.shadow, section});
  g.building = -1;
  pv.built_outfit = pv.pending_outfit;
  pv.pending_outfit = -1;
  REXLOG_INFO("Outfit previews: preview {} built outfit {} (actor {:08X}, shadow screen {:08X}, part {:08X}) "
              "after {} frames;{}", index, pv.built_outfit, Read32(pv.shadow + kActor), pv.shadow,
              Read32(pv.shadow + kController), pv.frames_loading, HeapReport(ctx, base));
}

// The frame after the build: start the animation (sub_820E1C70 at
// 0x820E1DC8..0x820E1E04, with our shadow screen).
void StartAnimation(PPCContext& ctx, uint8_t* base, Preview& pv, uint32_t section) {
  const uint32_t actor = Read32(pv.shadow + kActor);
  const uint32_t part = Read32(pv.shadow + kController);
  g.building = pv.built_outfit;  // (the set-up of its behaviours may run now)
  Call(__imp__sub_82102F28, ctx, base, {part, actor, 1});
  if (const uint32_t inner = Read32(part + 80)) {
    Call(__imp__sub_820C8138, ctx, base, {Call(__imp__sub_822B79F0, ctx, base, {inner})});
  }
  Call(__imp__sub_820E0BC8, ctx, base, {pv.shadow, section});
  g.building = -1;
  *Guest(pv.shadow + kFlags) |= kAnimated;
}

// Every frame after (0x820E1E0C..0x820E1E68).
void Animate(PPCContext& ctx, uint8_t* base, Preview& pv, uint32_t section, double dt) {
  const uint32_t clip = Read32(pv.shadow + kClip);
  if (clip && (Virtual(ctx, base, clip, 8, {}) & 0xFF)) {
    Call(sub_820E0C98, ctx, base, {pv.shadow, section});  // a new random idle
  }
  const uint32_t player = Read32(pv.shadow + kPlayer);
  if (player) {
    Call(__imp__sub_82350F88, ctx, base, {player, 0, 0, 0, dt});
    Call(__imp__sub_82351710, ctx, base, {player});
  }
  const uint32_t part = Read32(pv.shadow + kController);
  if (part) Virtual(ctx, base, part, 4, {0, Read32(pv.shadow + kActor), 0, 3, dt});
}

void TearDown(PPCContext& ctx, uint8_t* base) {
  for (Preview& pv : g.p) Drop(ctx, base, pv);
  RestoreZoneList();
  g.active = false;
  g.count = 0;
}

// --debug_outfit_previews: "a,b,c" -> previews of those outfits, spread out.
void DebugDrive() {
  const std::string want = REXCVAR_GET(debug_outfit_previews);
  if (want == g.debug_seen) return;
  g.debug_seen = want;
  if (want.empty()) {
    if (g.debug_driving) End();
    g.debug_driving = false;
    return;
  }
  std::array<int, kMaxPreviews> outfits{};
  int n = 0;
  std::stringstream s(want);
  std::string item;
  while (n < kMaxPreviews && std::getline(s, item, ',')) {
    try {
      outfits[n] = std::stoi(item);
    } catch (...) {
      continue;
    }
    ++n;
  }
  if (!n) return;
  float scale = 1.0f;  // "...@0.8" = that size
  if (const size_t at = want.find('@'); at != std::string::npos) {
    try {
      scale = std::stof(want.substr(at + 1));
    } catch (...) {
    }
  }
  g.debug_driving = true;
  Begin(n);
  for (int i = 0; i < n; ++i) {
    Show(i, outfits[i]);
    Place(i, (float(i) + 0.5f) / float(n), scale);
  }
}

}  // namespace

// --- the API -----------------------------------------------------------------

void Begin(int count) {
  g.count = std::clamp(count, 0, kMaxPreviews);
  g.active = g.count > 0;
}
void Show(int i, int outfit) {
  if (i >= 0 && i < kMaxPreviews && outfit >= 0 && outfit <= outfits::kLastOutfit) g.p[i].want_outfit = outfit;
}
void Place(int i, float x, float scale, float world_y) {
  if (i >= 0 && i < kMaxPreviews) {
    g.p[i].x = x;
    g.p[i].y = world_y;
    g.p[i].scale = std::clamp(scale, 0.1f, 2.0f);
  }
}
void SetLocked(int i, bool locked) {
  if (i >= 0 && i < kMaxPreviews) g.p[i].locked = locked;
}
void End() { g.count = 0; }

// (outfit_previews.h) The models go before a respawn frees them; the wanted
// outfits stay, so Tick takes their packages again and builds once allowed.
void LetGoForRespawn(PPCContext& ctx, uint8_t* base) {
  for (Preview& pv : g.p) {
    if (pv.built_outfit < 0) continue;
    Destroy(ctx, base, pv);
    REXLOG_INFO("Outfit previews: a model let go for the players' respawn");
  }
}
bool Active() { return g.active; }
bool Loading(int i) {
  if (i < 0 || i >= kMaxPreviews) return false;
  const Preview& pv = g.p[i];
  return pv.want_outfit >= 0 && pv.built_outfit != pv.want_outfit;
}

int OutfitForActor(uint32_t actor) {
  if (!actor) return -1;
  // During a build the new actor isn't at the shadow screen's +92 yet:
  // a character set up meanwhile is the one being made.
  if (g.building >= 0) return g.building;
  for (const Preview& pv : g.p) {
    if (pv.shadow && pv.built_outfit >= 0 && Read32(pv.shadow + kActor) == actor) return pv.built_outfit;
  }
  return -1;
}
int OutfitOfScreen(uint32_t screen) {
  for (const Preview& pv : g.p) {
    if (pv.shadow && pv.shadow == screen) return pv.built_outfit;
  }
  return -1;
}
int BuildingOutfit() { return g.building; }

// --- the frame ---------------------------------------------------------------

void Tick(PPCContext& ctx, uint8_t* base, uint32_t screen, double dt) {
  DebugDrive();
  if (!g.scratch) g.scratch = rex::system::kernel_memory()->SystemHeapAlloc(128);
  if (g.count > 0) GrowZoneList();
  for (int i = 0; i < kMaxPreviews; ++i) {
    Preview& pv = g.p[i];
    const bool wanted = i < g.count;
    if (!wanted) {
      if (pv.built_outfit >= 0 || pv.pending_outfit >= 0) Drop(ctx, base, pv);
      continue;
    }
    if (!pv.shadow) {
      pv.shadow = rex::system::kernel_memory()->SystemHeapAlloc(kShadowBytes);
      if (!pv.shadow) continue;
      std::memset(Guest(pv.shadow), 0, kShadowBytes);
    }
    // A new pick: take a reference on its package (the old model stays meanwhile).
    if (pv.want_outfit >= 0 && pv.want_outfit != pv.built_outfit && pv.want_outfit != pv.pending_outfit) {
      GiveRef(ctx, base, pv.shadow + kKeyPending);
      const uint32_t key = TemplateKey(ctx, base, pv.want_outfit);
      TakeRef(ctx, base, pv.shadow + kKeyPending, key);
      pv.pending_outfit = Read32(pv.shadow + kKeyPending) ? pv.want_outfit : -1;
      pv.frames_loading = 0;
      REXLOG_INFO("Outfit previews: preview {} wants outfit {} (template key {:08X}{})", i, pv.want_outfit,
                  key, pv.pending_outfit < 0 ? ", NO SUCH SECTION" : "");
      if (pv.pending_outfit < 0) pv.want_outfit = pv.built_outfit;  // (don't retry every frame)
    }
    if (pv.pending_outfit >= 0) {
      ++pv.frames_loading;
      if (!outfits::PreviewHeld() && Ready(ctx, base, pv.shadow + kKeyPending)) Build(ctx, base, pv, i);
    }
    const uint32_t actor = Read32(pv.shadow + kActor);
    if (!actor) continue;
    const uint32_t inv = Inventory(ctx, base, pv.shadow + kKeyBuilt);
    const uint32_t section =
        inv && Read32(pv.shadow + kKeyBuilt) ? Call(__imp__sub_822E4680, ctx, base, {inv, pv.shadow + kKeyBuilt}) : 0;
    if (!section) continue;
    if (!(*Guest(pv.shadow + kFlags) & kAnimated)) {
      StartAnimation(ctx, base, pv, section);
    } else {
      Animate(ctx, base, pv, section, dt);
    }
    // Locked = out of sight (until the silhouette exists).
    SetPosition(actor, (pv.x - 0.5f) * kWorldWidth, pv.locked ? -1000.0f : pv.y, pv.scale);
  }
  if (g.count > 0) {
    // The game's own preview: out of sight while ours are up.
    if (const uint32_t own = Read32(screen + kActor); own > 0x40000000) SetPosition(own, 0.0f, -1000.0f);
  } else {
    g.active = false;
    bool any = false;
    for (const Preview& pv : g.p) any |= pv.built_outfit >= 0;
    if (!any) RestoreZoneList();
  }
}

}  // namespace outfit_previews

// The screen's Exit: our previews go first (else they'd stay in the world,
// drawn in the level after the menu), then the game's own.
extern "C" REX_FUNC(sub_820E0EB0) {
  bool any = false;
  for (const auto& pv : outfit_previews::g.p) any |= pv.built_outfit >= 0 || pv.pending_outfit >= 0;
  if (any) {
    outfit_previews::TearDown(ctx, base);
    REXLOG_INFO("Outfit previews: removed at the screen's Exit");
  }
  outfit_previews::g.debug_seen.clear();  // (the debug list comes back at the next open)
  __imp__sub_820E0EB0(ctx, base);
}
