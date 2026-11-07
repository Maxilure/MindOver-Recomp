// =============================================================================
// ui/hud_scale.cpp -- see hud_scale.h
// =============================================================================
#include "hud_scale.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../players/more_players.h"
#include "scrooby.h"
#include "../native/widescreen.h"

// The game's functions the test parts use (ShowTestParts).
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name key (r3, 8 bytes) from a C string (r4)
extern "C" REX_FUNC(__imp__sub_8225C520);  // collectable counter: add one (counter, name, icon, max, flag)
extern "C" REX_FUNC(__imp__sub_8225C7E8);  // collectable counter: show it (counter, name, r5 1, f1 seconds)
extern "C" REX_FUNC(__imp__sub_8226EC40);  // timer display: start

REXCVAR_DEFINE_INT32(hud_size, 80, "CrashMoM",
                     "HUD size in percent (30-100; default 80; 100 = the original): every HUD part shrinks toward "
                     "its own screen corner")
    .range(30, 100);
REXCVAR_DEFINE_INT32(widescreen_hud, 0, "CrashMoM",
                     "Widescreen HUD (a picture wider than 16:9, native/widescreen.h): 0 = in place (where "
                     "the 16:9 game has it), 1 = screen edges (the corner pieces pushed out to the wide "
                     "picture's corners)")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(hud_smaller_four_players, true, "CrashMoM",
                    "While players 3-4 are in the game, the HUD is at most 80 % (four corners fit); a smaller HUD Size is kept as it is");


namespace hud_scale {

float ViewHalfHeight() { return 0.5f / widescreen::CurrentAspect(); }
float ViewHalfWidth() { return 0.5f * widescreen::kGameAspect / widescreen::CurrentAspect(); }

namespace {

// The game's 2D helper object: +0 = the PDDI context (xnContext); its
// translate / scale helpers (sub_823812E8 / sub_82381308) multiply the stack
// top. Its address sits in this global (read by the front end's 2D set-up
// at 0x822630DC / 0x82263158).
constexpr uint32_t kHelperGlobal = 0x82571B80;
// The context's matrix-stack slots (vtable 0x82016744): 15 push (copy the top
// one deeper: 0x8243F0C0), 16 pop (0x8243F0F0, marks the transforms dirty).
// The stack type in r4: 0 = the one every 2D part draws with.
// 13 = load identity, 14 load (r5 = 16 floats; copies, marks dirty), 17
// multiply, 18 = the top's address (no side effect).
constexpr int kSlotLoad = 14, kSlotPush = 15, kSlotPop = 16, kSlotGetTop = 18;

// With players 3-4 in: the HUD at most this size. A CEILING, not a factor
// (2026-10-30): whoever already picked a HUD Size at or under it keeps
// exactly that size; above it, the HUD comes down to it.
constexpr float kFourPlayerMaxSize = 0.8f;

// The stack's OUTPUT space, the same for every HUD part: each frame (the
// front end's 2D set-up, every Scrooby page) starts from identity (slot 13)
// and then scales canvas units by 1.5 / 1280 around the canvas centre
// (320, 240): the screen's edges sit at x -+0.5, y -+0.28125 (720 / 1280 / 2).
// Working there, not in a part's own units (Scrooby texts draw in canvas /
// 640 units, the displays in canvas units), one anchor fits every part.
// WIDESCREEN (native/widescreen.h): the game then scales canvas units by
// 1 / (480 x aspect) instead (its display's height is 1 / aspect), so the
// top / bottom edges sit at y -+0.5 / aspect; x -+0.5 is the WIDE picture's
// edges, and the game's 16:9 edges (where the HUD is laid out) at
// -+0.5 x (16 / 9) / aspect. Both = the numbers above at 16:9.
// (ViewHalfHeight / ViewHalfWidth: above, public: hud_scale.h)
// THE ANNOUNCEMENT ("Attack" + its button, a dig spot's prompt; the page
// InGame_Announcement): its set-up sub_8225B5E8 puts the text 50 canvas units
// in from the screen's left edge (x = (W - 640) x -0.5 + 50, W = 853 wide)
// at y 300 (li r5,300 at 0x8225B674), 36 units tall, centred in that box.
// It shrinks toward the left edge at its own middle (canvas y 318), so it
// stays at its height and keeps a smaller margin, like the corners.
constexpr float kAnnouncementCanvasY = 300.0f + 36.0f / 2.0f;
float AnnouncementY() {
  return (kAnnouncementCanvasY - 240.0f) / (480.0f * widescreen::CurrentAspect());
}
uint32_t g_matrix = 0;  // guest scratch: the matrix we load (64 bytes)

uint32_t Read32(uint8_t* base, uint32_t a) {
  const uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
  (void)base;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The context's virtual `slot` with r4 = the stack type 0 (r5 as given);
// returns r3.
uint32_t CallContext(PPCContext& ctx, uint8_t* base, uint32_t context, int slot, uint32_t r5 = 0) {
  const uint32_t target = Read32(base, Read32(base, context) + 4 * slot);
  PPCFunc* function = rex::runtime::ResolveIndirectFunction(target);
  if (!function) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = context;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = r5;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
float ReadFloat(uint32_t a) {
  const uint32_t b = Read32(nullptr, a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void WriteFloat(uint32_t a, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
  p[0] = uint8_t(b >> 24), p[1] = uint8_t(b >> 16), p[2] = uint8_t(b >> 8), p[3] = uint8_t(b);
}

// Texts noted this frame and last (drawn by the page, not by their owner, so
// the owner's draw may come after them in a frame).
// An entry not noted again for a second is forgotten (a page unloaded with
// its level: that address may be another text now).
using Clock = std::chrono::steady_clock;
struct Noted {
  uint32_t text;
  int where;
  Clock::time_point when;
};
std::array<Noted, 32> g_texts{};
size_t g_text_count = 0;
bool Fresh(const Noted& n, Clock::time_point now) { return now - n.when < std::chrono::seconds(1); }

}  // namespace

float Scale() {
  float s = std::clamp(REXCVAR_GET(hud_size), 30, 100) / 100.0f;
  if (REXCVAR_GET(hud_smaller_four_players) && more_players::FourPlayerLayout()) {
    s = std::min(s, kFourPlayerMaxSize);
  }
  return s;
}

bool CornerMove(int where, float* s_out, float* sx_out, float* dx_out, float* dy_out) {
  if (where == kNone) return false;
  // The stretched kind: x times the picture's width over the game's 16:9
  // (around the screen's centre, x = 0 in the stack's output space).
  const float wide = widescreen::CurrentAspect() / widescreen::kGameAspect;
  if (where == kStretchWide) {
    if (wide < 1.001f) return false;
    *s_out = 1.0f, *sx_out = wide, *dx_out = 0.0f, *dy_out = 0.0f;
    return true;
  }
  const float s = Scale();
  // WIDESCREEN HUD "screen edges": the corner pieces (and the announcement,
  // on the left) move sideways by the room between the game's 16:9 edge and
  // the wide picture's edge; the top-centre ones stay.
  float shift = 0.0f;
  if (REXCVAR_GET(widescreen_hud) == 1 && where != kTopCentre) {
    const float room = 0.5f - ViewHalfWidth();
    shift = where == kAnnouncement ? -room : (where & 1) ? room : -room;
  }
  if (s >= 0.999f && shift == 0.0f) return false;
  // The anchor in the stack's OUTPUT space (x -0.5 .. 0.5 across the
  // picture, y -0.28 .. 0.28 bottom to top at 16:9: see ViewHalf*).
  float ax = 0.0f, ay = 0.0f;
  if (where == kTopCentre) {
    ay = ViewHalfHeight();
  } else if (where == kAnnouncement) {
    ax = -ViewHalfWidth(), ay = AnnouncementY();
  } else {
    ax = (where & 1) ? ViewHalfWidth() : -ViewHalfWidth();
    ay = where >= 2 ? -ViewHalfHeight() : ViewHalfHeight();
  }
  *s_out = s, *sx_out = s;
  *dx_out = ax * (1.0f - s) + shift;
  *dy_out = ay * (1.0f - s);
  return true;
}

bool Push(PPCContext& ctx, uint8_t* base, int where) {
  float s, sx, dx, dy;
  if (!CornerMove(where, &s, &sx, &dx, &dy)) return false;
  const uint32_t helper = Read32(base, kHelperGlobal);
  if (!helper) return false;
  const uint32_t context = Read32(base, helper);
  if (!context) return false;
  if (!g_matrix) g_matrix = rex::system::kernel_memory()->SystemHeapAlloc(64);
  if (!g_matrix) return false;
  CallContext(ctx, base, context, kSlotPush);
  // top' = top x C, C = scale (sx, s) then move by (dx, dy) (row vectors,
  // translation in row 3: v' = v . top . C). For an affine top only columns
  // 0-1 change: x sx / s, and row 3 also + the move.
  const uint32_t top = CallContext(ctx, base, context, kSlotGetTop);
  if (!top) return true;  // pushed: still popped
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = ReadFloat(top + 4 * i);
  for (int row = 0; row < 4; ++row) {
    m[row * 4 + 0] *= sx;
    m[row * 4 + 1] *= s;
  }
  m[12] += dx * m[15];
  m[13] += dy * m[15];
  for (int i = 0; i < 16; ++i) WriteFloat(g_matrix + 4 * i, m[i]);
  CallContext(ctx, base, context, kSlotLoad, g_matrix);  // copies + marks the transforms dirty
  return true;
}

void Pop(PPCContext& ctx, uint8_t* base) {
  const uint32_t helper = Read32(base, kHelperGlobal);
  if (!helper) return;
  if (const uint32_t context = Read32(base, helper)) CallContext(ctx, base, context, kSlotPop);
}

void NoteText(uint32_t text, int player) {
  if (!text) return;
  const auto now = Clock::now();
  Noted* free_slot = nullptr;
  for (size_t i = 0; i < g_text_count; ++i) {
    if (g_texts[i].text == text) {
      g_texts[i] = {text, player, now};
      return;
    }
    if (!free_slot && !Fresh(g_texts[i], now)) free_slot = &g_texts[i];
  }
  if (free_slot) *free_slot = {text, player, now};
  else if (g_text_count < g_texts.size()) g_texts[g_text_count++] = {text, player, now};
}

int WhereOfText(uint32_t text) {
  const auto now = Clock::now();
  for (size_t i = 0; i < g_text_count; ++i) {
    if (g_texts[i].text == text) return Fresh(g_texts[i], now) ? g_texts[i].where : kNone;
  }
  return kNone;
}

void ShowTestParts(PPCContext& ctx, uint8_t* base, int what) {
  const uint32_t uber = Read32(base, 0x8259B190);
  const uint32_t fe = uber ? Read32(base, uber + 52) : 0;
  if (!fe) return;
  if (what & kTestCounter) {
    // A counter named "porttest", icon frame 7 (HUD_Icon_Prop: the purple
    // wrench), 0 of 6; shown for 10 s. Adding twice is harmless (the add
    // looks the name up first).
    static uint32_t name = 0;
    if (!name) name = rex::system::kernel_memory()->SystemHeapAlloc(8);
    if (name) {
      scrooby::Call(__imp__sub_8236ACB8, ctx, base, name, scrooby::GuestAscii("porttest"));
      const PPCContext saved = ctx;
      ctx.r3.u64 = fe + 1628, ctx.r4.u64 = name, ctx.r5.u64 = 7, ctx.r6.u64 = 6, ctx.r7.u64 = 0;
      __imp__sub_8225C520(ctx, base);
      ctx = saved;
      ctx.r3.u64 = fe + 1628, ctx.r4.u64 = name, ctx.r5.u64 = 1, ctx.f1.f64 = 10.0;
      __imp__sub_8225C7E8(ctx, base);
      ctx = saved;
    }
  }
  if (what & kTestTimer) {
    if (const uint32_t timer = Read32(base, fe + 260)) {
      WriteFloat(timer + 12, 30.0f);  // seconds left
      uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(timer + 16);
      p[0] = p[1] = p[2] = p[3] = 0xFF;  // last whole second shown: none (-1)
      scrooby::Call(__imp__sub_8226EC40, ctx, base, timer);
      // Shown (CFrontendElement slot 5 sub_8225F0F0 = set +4 bit 0x40): the
      // update slides it in only then; the objectives normally do this.
      *rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(timer + 4) |= 0x40;
    }
  }
  if (what & kTestAnnouncement) {
    const uint32_t a = fe + 72;
    if (const uint32_t text = Read32(base, a + 12)) {
      scrooby::SetText(ctx, base, text, u"Attack TEST");
      WriteFloat(a + 16, 8.0f);  // seconds shown
    }
  }
  REXLOG_INFO("HUD: test parts shown ({})", what);
}

}  // namespace hud_scale
