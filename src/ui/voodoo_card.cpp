// =============================================================================
// ui/voodoo_card.cpp -- see voodoo_card.h
// =============================================================================
//
// HOW THE DOLL IS DRAWN (2026-11-08): BY THE GAME, with its own draw call, so
// both renderers (native and emulated) show it and it is in photos.
//
// 1. THE MODEL. A doll kind's template "Collectables:c_voodoo<Kind>" is an
//    inventory SECTION (the level loads it when the level has such dolls;
//    the same lookup the spawn cheat uses: name hash sub_82357020, exists
//    sub_822E3FC8, ready sub_822E4D88, the section sub_822E4680). In it the
//    model is a Pure3D Geometry named "voodoo<Kind>Shape" (read from the
//    packages on the disc: one per kind). Found the way the game finds a
//    texture by name (sub_820D8908): the section's vtable slot 3 = find(r4 =
//    a "dynamic caster" object, r5 = the name's 64-bit key from
//    sub_8236ACB8), the caster's vtable picking the type: Geometry's =
//    0x8200D5BC (RTTI: DynamicCaster<Geometry>).
//    While a level is played we hold a reference on every doll kind's
//    section the level has loaded (sub_822E44B8, given back with
//    sub_822E4650 when play stops): picking up the last doll of a kind
//    would unload it before its card shows (KeepLoadedDolls below).
// 2. DRAWING IT. Every Pure3D drawable draws itself with vtable slot 6,
//    with whatever matrix is current. Geometry (vtable 0x8200E60C) keeps its
//    pieces in GROUPS at +0x50..+0x54, 20 bytes each: +4 = the piece. (Its
//    own slot 6, sub_823C8740 -> sub_823912F8, draws each group's list at
//    +8 instead, which is EMPTY for the doll: the levels draw it through
//    their sorting passes. Calling it drew nothing.) A piece =
//    PrimGroupOptimized (vtable 0x8200CADC), slot 6 sub_82392708 = +0x40
//    material (tShader, pddi shader at +0x10), +0x80 vertex buffer, fog per
//    the material, then xnContext::DrawPrimBuffer. Seen in a PDDI trace of a
//    spawned doll: ONE piece, the simple material.
// 3. WHERE. The card's icon element (AchBarIcon, a Scrooby picture: Sprite
//    @frontend, draw = vtable slot 6 sub_82374098) is drawn by the bar's page
//    with the page's matrices on the context's stack 0 (the slide, the HUD
//    size). Its draw is wrapped: while a doll shows, the picture is NOT
//    drawn and the doll is drawn instead: push, top' = L x top (row vectors:
//    v' = v . L . top), the pieces, pop. L = the model's middle to the origin,
//    turned about y, scaled to its height on the card, moved to the icon's
//    spot (its feet near the card's bottom, its head standing out above the
//    card's top edge, like the dolls in the levels).
// The context: the 2D helper global 0x82571B80 -> +0 (hud_scale.cpp's notes;
// slots 15 push, 16 pop, 18 the top's address, 14 load).
// =============================================================================
#include "voodoo_card.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../achievements/achievements.h"
#include "../guest_memory.h"
#include "../players/more_players.h"

extern "C" REX_FUNC(__imp__sub_82357020);  // hash of a string (r3 string, r4 seed 0)
extern "C" REX_FUNC(__imp__sub_822E3FC8);  // inventory: does the section named *r4 exist
extern "C" REX_FUNC(__imp__sub_822E4D88);  // inventory: is that section ready (loaded)
extern "C" REX_FUNC(__imp__sub_822E4680);  // inventory: the section named *r4
extern "C" REX_FUNC(__imp__sub_822E44B8);  // inventory: take a reference on section *r4
extern "C" REX_FUNC(__imp__sub_822E4650);  // inventory: give it back
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name key (r3 = 8 bytes) from a C string (r4)
extern "C" REX_FUNC(__imp__sub_82374098);
extern "C" REX_FUNC(__imp__sub_82430A40);  // xnContext::SetZTest (r4 = on)
extern "C" REX_FUNC(__imp__sub_82430A58);  // xnContext::SetZCompare (r4 = PDDI compare)
extern "C" REX_FUNC(__imp__sub_82430A78);  // xnContext::SetZWrite (r4 = on)  // Sprite@frontend slot 6: a Scrooby picture draws itself

namespace voodoo_card {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kInventoryManager = 0x8259B264;
constexpr uint32_t kGeometryCaster = 0x8200D5BC;  // vtable of DynamicCaster<Geometry>
constexpr uint32_t kHelperGlobal = 0x82571B80;    // the game's 2D helper (+0 = the PDDI context)
constexpr int kSlotLoad = 14, kSlotPush = 15, kSlotPop = 16, kSlotGetTop = 18;
constexpr int kSlotDisplay = 6;  // every Pure3D drawable: draw yourself
constexpr uint32_t kMaxPieces = 16;

// The kinds, in doll-mission order (achievements.cpp DollOf).
constexpr const char* kKinds[] = {"Whumpa", "Ice", "Nina", "Desert", "Junk", "Phantom", "Crash"};
constexpr int kKindCount = 7;

// THE MODELS' SIZE (bounding boxes of the 7 geometries on the disc, all
// alike): y from -0.44 to 0.84 (1.28 tall), x -0.47..0.35, z -0.52..0.31.
constexpr float kModelMidY = 0.2f, kModelHeight = 1.28f;

// ON THE CARD (canvas units, y up; card x 147-493, y 20-87, the icon's box
// x 158-203, y 31-76, 45 wide, options_page.cpp): the doll's middle over the
// icon's middle (4 units right of it: its arms stay on the glass), 74 units
// tall, feet 3 under the icon's box (8 above the card's bottom), head ~15
// over the card's top edge; one turn every 4 s, starting face on (the model
// looks down -z: half a turn first).
// WHICH UNITS: the icon's draw works in the units of its element's position
// (+92 / +96 = canvas x the game's unit, the float at 0x8204701C, 1/640: the
// stack top then scales by 0.75 = 640 canvas units across the 4:3 middle, as
// scrooby::SetBox writes them); the doll is placed from that position, so
// the page's own offset and slide (in the stack) need no knowing.
constexpr float kIconSide = 45.0f, kDollHeight = 74.0f, kDollBelowIcon = 3.0f, kDollRight = 4.0f;
constexpr uint32_t kUnitConstant = 0x8204701C;
constexpr float kTurnSeconds = 4.0f;

std::atomic<int> g_wanted{-1};       // the doll asked for (front-end thread)
std::atomic<uint32_t> g_icon{0};     // the card's icon element

// Owned by the game's main thread (the page draws there):
int g_on_card = -1;         // the kind drawn on the card now (its turn starts when it comes)
uint32_t g_scratch = 0;     // guest scratch: names, keys, caster, matrix
Clock::time_point g_shown;  // when the current doll came up (its turn starts there)
constexpr uint32_t kScratchText = 0, kScratchKey = 128, kScratchName = 136, kScratchCaster = 144,
                   kScratchMatrix = 160, kScratchSize = 224;

uint32_t Read32(uint32_t a) {
  const uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
float ReadFloat(uint32_t a) {
  const uint32_t b = Read32(a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void WriteFloat(uint32_t a, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  Write32(a, b);
}
void WriteText(uint32_t a, const std::string& s) {
  uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
  std::memcpy(p, s.c_str(), s.size() + 1);
}

uint32_t Call(PPCFunc* f, PPCContext& ctx, uint8_t* base, uint32_t r3, uint64_t r4 = 0, uint64_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  f(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
// Vtable slot `slot` of object `self`.
uint32_t CallSlot(PPCContext& ctx, uint8_t* base, uint32_t self, int slot, uint64_t r4 = 0, uint64_t r5 = 0) {
  PPCFunc* f = rex::runtime::ResolveIndirectFunction(Read32(Read32(self) + 4 * slot));
  return f ? Call(f, ctx, base, self, r4, r5) : 0;
}

bool Scratch() {
  if (!g_scratch) g_scratch = rex::system::kernel_memory()->SystemHeapAlloc(kScratchSize);
  return g_scratch != 0;
}

// Section key of kind k ("Collectables:c_voodoo<Kind>") into the scratch.
void SectionKey(PPCContext& ctx, uint8_t* base, int k) {
  WriteText(g_scratch + kScratchText, std::string("Collectables:c_voodoo") + kKinds[k]);
  Write32(g_scratch + kScratchKey, Call(__imp__sub_82357020, ctx, base, g_scratch + kScratchText, 0));
}

// The section of kind k, 0 = not loaded in this level.
uint32_t Section(PPCContext& ctx, uint8_t* base, int k) {
  const uint32_t inventory = Read32(kInventoryManager);
  if (!inventory) return 0;
  SectionKey(ctx, base, k);
  const bool exists = Call(__imp__sub_822E3FC8, ctx, base, inventory, g_scratch + kScratchKey) & 0xFF;
  const bool ready = exists && (Call(__imp__sub_822E4D88, ctx, base, inventory, g_scratch + kScratchKey) & 0xFF);
  const uint32_t section = ready ? Call(__imp__sub_822E4680, ctx, base, inventory, g_scratch + kScratchKey) : 0;
  return section;
}

// Takes / gives back our reference on kind k's section.
void Hold(PPCContext& ctx, uint8_t* base, int k, bool take) {
  const uint32_t inventory = Read32(kInventoryManager);
  if (!inventory) return;
  SectionKey(ctx, base, k);
  Call(take ? __imp__sub_822E44B8 : __imp__sub_822E4650, ctx, base, inventory, g_scratch + kScratchKey);
}

// Kind k's model (a Geometry), 0 = not found.
uint32_t Model(PPCContext& ctx, uint8_t* base, int k) {
  const uint32_t section = Section(ctx, base, k);
  if (!section) return 0;
  WriteText(g_scratch + kScratchText, std::string("voodoo") + kKinds[k] + "Shape");
  Call(__imp__sub_8236ACB8, ctx, base, g_scratch + kScratchName, g_scratch + kScratchText);
  const uint64_t key = uint64_t(Read32(g_scratch + kScratchName)) << 32 | Read32(g_scratch + kScratchName + 4);
  Write32(g_scratch + kScratchCaster, kGeometryCaster);
  const uint32_t found = CallSlot(ctx, base, section, 3, g_scratch + kScratchCaster, key);
  return found;
}

// KEEPING THE DOLLS' PACKAGES LOADED (2026-11-08, a playtest: a Crash doll
// picked up, the message came, no doll on the card; log "the Crash doll's
// model isn't loaded in this level"). A doll kind's section is loaded while
// the level has dolls of that kind standing; picking up the last one there
// lets it go (the actor held the reference), so it was already unloaded when
// the card asked a frame later. In the tests the doll was never picked up.
// Now, while a level is played, every doll kind whose section is loaded gets
// a reference of ours (looked at twice a second), so a pick-up can't unload
// it before its card has shown. All given back when play stops (a level
// change loads with the game state off "in play", as the spawn cheat sees
// it; the main menu too): the next level decides again. Cost: at most the 7
// small doll packages stay loaded for the rest of a level.
bool g_kept[kKindCount] = {};
Clock::time_point g_last_look;

void KeepLoadedDolls(PPCContext& ctx, uint8_t* base, bool playing) {
  if (!playing) {
    for (int k = 0; k < kKindCount; ++k) {
      if (!g_kept[k]) continue;
      Hold(ctx, base, k, false);
      g_kept[k] = false;
      REXLOG_INFO("Voodoo card: {} dolls' package let go (play stopped)", kKinds[k]);
    }
    return;
  }
  const auto now = Clock::now();
  if (now - g_last_look < std::chrono::milliseconds(500)) return;
  g_last_look = now;
  for (int k = 0; k < kKindCount; ++k) {
    if (g_kept[k] || !Section(ctx, base, k)) continue;
    Hold(ctx, base, k, true);
    g_kept[k] = true;
    REXLOG_INFO("Voodoo card: {} dolls' package kept loaded for their message card", kKinds[k]);
  }
}

// The doll, drawn where the card's icon is (called inside the icon's draw).
// False = nothing drawn (the picture is drawn instead).
bool DrawDoll(PPCContext& ctx, uint8_t* base, int k) {
  const uint32_t helper = Read32(kHelperGlobal);
  const uint32_t context = helper ? Read32(helper) : 0;
  if (!context) return false;
  const uint32_t model = Model(ctx, base, k);
  if (!model) return false;
  if (g_on_card != k) {
    g_on_card = k;
    g_shown = Clock::now();
    REXLOG_INFO("Voodoo card: doll {} on the card (model {:08X})", kKinds[k], model);
  }
  CallSlot(ctx, base, context, kSlotPush);
  const uint32_t top = CallSlot(ctx, base, context, kSlotGetTop);
  if (top) {
    float t[16];
    for (int i = 0; i < 16; ++i) t[i] = ReadFloat(top + 4 * i);
    // L = move the model's middle to 0, turn about y, scale, move to the spot
    // (row vectors: v . L). As a 4x4 with translation in row 3.
    const float unit = ReadFloat(kUnitConstant);
    const uint32_t icon = g_icon.load();
    const float ex = ReadFloat(icon + 92), ey = ReadFloat(icon + 96);  // the icon box's bottom-left
    const float s = kDollHeight * unit / kModelHeight;
    const float secs = std::chrono::duration<float>(Clock::now() - g_shown).count();
    const float a = 3.14159265f + secs / kTurnSeconds * 6.2831853f;
    const float c = std::cos(a), sn = std::sin(a);
    const float spot_x = ex + (kIconSide / 2 + kDollRight) * unit;
    const float ty = ey + (kDollHeight / 2 - kDollBelowIcon) * unit;
    // rows: x axis, y axis, z axis, translation (of v - (0, mid, 0)).
    const float l[16] = {c * s, 0, -sn * s, 0,  //
                         0, s, 0, 0,             //
                         sn * s, 0, c * s, 0,    //
                         spot_x, ty - kModelMidY * s, 0, 1};
    float m[16];
    for (int r = 0; r < 4; ++r) {
      for (int col = 0; col < 4; ++col) {
        float v = 0;
        for (int i = 0; i < 4; ++i) v += l[r * 4 + i] * t[i * 4 + col];
        m[r * 4 + col] = v;
      }
    }
    for (int i = 0; i < 16; ++i) WriteFloat(g_scratch + kScratchMatrix + 4 * i, m[i]);
    CallSlot(ctx, base, context, kSlotLoad, 0, g_scratch + kScratchMatrix);
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("Voodoo card: the icon's matrix top: [{} {} {} {}] [{} {} {} {}] [{} {} {} {}] [{} {} {} {}]; "
                  "icon at {} {}, unit {}",
                  t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14],
                  t[15], ex, ey, unit);
    }
    // DEPTH FOR THE DOLL'S DRAW. The HUD draws with the depth test's compare
    // "always" and no depth writes, so the model's own parts drew in their
    // file order: the back of the head over the face, the snout through the
    // back of the head. (Culling back faces fixed the head but cut off the
    // snout facing the camera: the snout is drawn two-sided.) The bar's page
    // draws into the post surface, whose depth buffer the game cleared at
    // that pass's start (to "far": the game's depth is REVERSED, near = 1),
    // so the doll gets the world's own rule: test on, PDDI compare 3 =
    // greater-or-equal (table 0x824EDCE4 -> D3D 6), writes on. Tried: compare
    // 5 (less-or-equal) = nothing drawn. The context's render state block
    // (+140: +8 test byte, +12 compare, +16 write byte) is put back after.
    // (The doll's depth stays in the buffer: a later HUD draw that tests
    // depth could be hidden where the doll is, while the card shows. None
    // seen.)
    const uint32_t state = Read32(context + 140);
    if (!state) {
      CallSlot(ctx, base, context, kSlotPop);
      return true;
    }
    const uint8_t* sp = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(state);
    const uint32_t old_test = sp[8], old_compare = Read32(state + 12), old_write = sp[16];
    constexpr uint32_t kGreaterOrEqual = 3;
    Call(__imp__sub_82430A40, ctx, base, context, 1);
    Call(__imp__sub_82430A58, ctx, base, context, kGreaterOrEqual);
    Call(__imp__sub_82430A78, ctx, base, context, 1);
    // Each group's piece (+4), drawn with its own slot 6.
    const uint32_t first = Read32(model + 0x50), end = Read32(model + 0x54);
    for (uint32_t g = first; g && g + 20 <= end && g - first < 20 * kMaxPieces; g += 20) {
      if (const uint32_t piece = Read32(g + 4)) CallSlot(ctx, base, piece, kSlotDisplay);
    }
    Call(__imp__sub_82430A40, ctx, base, context, old_test);
    Call(__imp__sub_82430A58, ctx, base, context, old_compare);
    Call(__imp__sub_82430A78, ctx, base, context, old_write);
  }
  CallSlot(ctx, base, context, kSlotPop);
  return true;
}

}  // namespace

bool Show(PPCContext& ctx, uint8_t* base, int doll, uint32_t icon) {
  if (doll < 0 || doll >= kKindCount || !Scratch()) {
    g_wanted = -1;
    return false;
  }
  // A kind not loaded in this level: no doll (the card's words take its room).
  if (!Model(ctx, base, doll)) {
    REXLOG_INFO("Voodoo card: the {} doll's model isn't loaded in this level: the card shows without it",
                kKinds[doll]);
    g_wanted = -1;
    return false;
  }
  g_icon = icon;
  g_wanted = doll;
  return true;
}

void Hide() { g_wanted = -1; }

int Showing() { return g_wanted; }

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  if (!Scratch()) return;
  if (g_wanted < 0) g_on_card = -1;
  KeepLoadedDolls(ctx, base, more_players::InPlay() && REXCVAR_GET(message_bar_card));
}

}  // namespace voodoo_card

// Sprite@frontend slot 6: a Scrooby picture draws itself. The card's icon
// while a doll shows: the doll instead.
extern "C" REX_FUNC(sub_82374098) {
  using namespace voodoo_card;
  const int doll = g_wanted.load();
  if (doll >= 0 && doll < kKindCount && ctx.r3.u32 == g_icon.load()) {
    // (Not drawable after all, its section gone: nothing, never the icon's
    // last achievement picture.)
    if (Scratch()) DrawDoll(ctx, base, doll);
    return;
  }
  __imp__sub_82374098(ctx, base);
}
