// =============================================================================
// decomp/shadow_caps.cpp -- a shadow volume's two caps streamed as triangles
// (sub_82393120) as C++, bit-identical (docs/findings/60 s.16)
// =============================================================================
//
// WHAT THIS IS
//   sub_823949A0 draws a stencil shadow volume (findings/12) in pieces; this
//   piece (r3 = the shadow object) streams its CAPS as immediate geometry:
//       count N  = the u32 at 0x8257223C (triangles)
//       indices  = 3 x s16 per triangle at 0x82572240 (6 bytes each)
//       table A  = *(*(r3 +72) +16): the shape's vertices (3 floats each)
//       table B  = 0x825758F8: the same vertices pushed away (3 floats each;
//                  presumably the extruded copy, name TBD)
//   Through the PDDI context at [0x82571B7C]: BeginPrims (vtable +124,
//   xnContext::BeginPrims 0x82430780) with the material at [0x825758F4]+16,
//   prim type 0, vertex format 0x2020, 6N vertices, 0; then per triangle six
//   vertices: A[i2] A[i1] A[i0] (one cap, wound backwards) then B[i0] B[i1]
//   B[i2]; each vertex = the colour word at 0x824EA558 (0x00FF0000) into the
//   stream's colour column and x y z into its position column, then the
//   stream's count +28 += 1, position +4 += stride, and each of the other
//   columns +8 +12 +16 +20 +24 += stride when not null; then EndPrims
//   (vtable +128, 0x82430870). ~0.65% of the main thread uncapped on Wumpa
//   Island, all its own instructions (the original re-reads the stream's
//   fields around every store).
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x82393120-0x823936B0 next to the
//   recompiled C++ (generated/default, DEFINE_REX_FUNC(sub_82393120)).
//   Every guest store happens in the original's order; floats go through
//   lfs / stfs (double(float) and back: a signalling NaN comes out quiet,
//   like the original).
//
// REGISTERS: the two virtual calls see EXACTLY the register file the original
//   gives them (BeginPrims: r3-r11, r26 r27 r29, ctr, lr, cr6 and r12 =
//   the entry lr; EndPrims: r3 r10 r11 ctr lr + the loop's leftovers: r4-r9,
//   r25 r28-r31, f0 f12 f13, cr6), they are the game's own functions (through
//   the PDDI intercept, so the recorder sees them), and the return leaves
//   what the original's epilogue leaves. 64-bit details kept: `lis` values
//   are sign-extended (0xFFFFFFFF8257....), adds are 64-bit.
//
// WHEN THE ORIGINAL RUNS INSTEAD: a context whose vtable slots +124 / +128
//   aren't xnContext's BeginPrims / EndPrims (checked before anything is
//   written).
//
// SWITCHES: --native_shadow_caps (default on); --debug_native_shadow_check =
//   original + ours on every call, the two virtual calls TAPED from the
//   original and REPLAYED to ours (TapedCheck below): every register at both
//   calls and at the return, the stack frame, the stream object and every
//   vertex byte compared; the game still draws once.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "check.h"

extern "C" REX_FUNC(__imp__sub_82393120);
extern "C" REX_FUNC(sub_82430780);  // xnContext::BeginPrims (PDDI intercept wrapper)
extern "C" REX_FUNC(sub_82430870);  // xnContext::EndPrims

REXCVAR_DEFINE_BOOL(native_shadow_caps, true, "CrashMoM",
                    "A shadow volume's caps streamed as triangles (sub_82393120) run as our C++ (same results); "
                    "false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_shadow_check, false, "CrashMoM",
                    "Debug: run the original shadow-volume geometry writers AND ours on every call and compare "
                    "memory and every register (slow; log lines \"Native check:\")");

namespace decomp_shadow_caps {
namespace {

constexpr uint32_t kCount = 0x8257223C;     // triangles (u32)
constexpr uint32_t kIndices = 0x82572240;   // 3 x s16 per triangle
constexpr uint32_t kTableB = 0x825758F8;    // 3 floats per vertex
constexpr uint32_t kColour = 0x824EA558;    // the colour word every vertex gets
constexpr uint32_t kContext = 0x82571B7C;   // the PDDI context pointer
constexpr uint32_t kMaterial = 0x825758F4;  // [it] + 16 = the material
constexpr uint32_t kBeginPrims = 0x82430780, kEndPrims = 0x82430870;
// `lis rX,-32169` / `lis rX,-32177`: sign-extended in the 64-bit registers.
constexpr uint64_t kHi8257 = 0xFFFFFFFF82570000ull, kHi824F = 0xFFFFFFFF824F0000ull;

inline uint32_t L(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint16_t L16(uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, GuestPtr(base, a), 2);
  return __builtin_bswap16(v);
}
inline void S(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, a), &v, 4);
}
inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
}
// lfs: a float widened to double; stfs: back to float.
inline double LF(uint8_t* base, uint32_t a) {
  const uint32_t w = L(base, a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
inline void SF(uint8_t* base, uint32_t a, double d) {
  const float f = float(d);
  uint32_t w;
  std::memcpy(&w, &f, 4);
  S(base, a, w);
}

// One vertex (one of the six blocks): colour, x y z from `at`, then the
// stream's count and columns moved on. Returns the stream's position pointer
// as it was (r9 after the last block) and the new one / old count + 1 via
// the out parameters (the last block's r7 / r8).
struct VertexLeft {
  uint64_t pos_before, pos_after, count_plus_1;
  double x, y, z;
};
inline VertexLeft Vertex(uint8_t* base, uint32_t st, uint32_t at) {
  VertexLeft v;
  S(base, L(base, st + 20), L(base, kColour));
  const uint32_t p0 = L(base, st + 4);
  v.z = LF(base, at + 8);
  v.y = LF(base, at + 4);
  v.x = LF(base, at + 0);
  SF(base, p0 + 0, v.x);
  SF(base, L(base, st + 4) + 4, v.y);
  SF(base, L(base, st + 4) + 8, v.z);
  const uint64_t count = L(base, st + 28);
  const uint64_t stride = L(base, st + 0);
  const uint64_t pos = L(base, st + 4);
  v.pos_before = pos;
  v.pos_after = stride + pos;
  v.count_plus_1 = count + 1;
  S(base, st + 28, uint32_t(v.count_plus_1));
  S(base, st + 4, uint32_t(v.pos_after));
  for (uint32_t c = 8; c <= 24; c += 4) {
    const uint32_t p = L(base, st + c);
    if (p) S(base, st + c, uint32_t(uint64_t(p) + stride));
  }
  return v;
}
// Index k (0-2) of triangle i -> the vertex's address in a table (12-byte
// entries, signed index, 32-bit address arithmetic like lfs / lfsx).
inline uint32_t At(uint8_t* base, uint32_t table, uint32_t i, uint32_t k) {
  const int32_t idx = int16_t(L16(base, kIndices + 6 * i + 2 * k));
  return table + uint32_t(idx) * 12u;
}

// The two virtual calls: the real ones (through the PDDI intercept), or the
// check's replay (see TapedCheck below).
struct Calls {
  PPCFunc* begin;
  PPCFunc* end;
};
constexpr Calls kRealCalls = {&sub_82430780, &sub_82430870};

bool Caps(PPCContext& ctx, uint8_t* base, const Calls& calls) {
  const uint32_t context = L(base, kContext);
  const uint32_t vt = L(base, context);
  if (L(base, vt + 124) != kBeginPrims || L(base, vt + 128) != kEndPrims) return false;

  const uint32_t shadow = ctx.r3.u32;
  const uint64_t entry_lr = ctx.lr;
  // Prologue: mflr r12; savegpr r25-r31 + lr; stwu r1,-144(r1).
  ctx.r12.u64 = entry_lr;
  const uint32_t r1 = ctx.r1.u32;
  const uint64_t saved[7] = {ctx.r25.u64, ctx.r26.u64, ctx.r27.u64, ctx.r28.u64,
                             ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  for (int k = 0; k < 7; ++k) S64(base, r1 - 64 + 8 * k, saved[k]);
  S(base, r1 - 8, uint32_t(entry_lr));
  S(base, r1 - 144, r1);
  ctx.r1.u64 = r1 - 144;  // stwu: the low word (the high one is 0 in guest code)

  ctx.r27.u64 = kHi8257;
  ctx.r10.u64 = L(base, shadow + 72);
  ctx.r11.u64 = L(base, kCount);
  ctx.r29.u64 = L(base, ctx.r10.u32 + 16);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
  if (!ctx.cr6.eq) {
    // BeginPrims(context, material, 0, 0x2020, 6N, 0).
    const uint64_t n = ctx.r11.u64;
    ctx.r26.u64 = kHi8257;
    const uint64_t n2 = uint32_t(n << 1);
    ctx.r8.u64 = 0;
    ctx.r7.u64 = uint32_t((n + n2) << 1);
    ctx.r3.u64 = context;
    ctx.r6.u64 = 8224;
    ctx.r11.u64 = L(base, kMaterial);
    ctx.r5.u64 = 0;
    ctx.r10.u64 = vt;
    ctx.r4.u64 = L(base, ctx.r11.u32 + 16);
    ctx.r9.u64 = kBeginPrims;
    ctx.ctr.u64 = kBeginPrims;
    ctx.lr = 0x82393180;
    calls.begin(ctx, base);

    const uint32_t st = ctx.r3.u32;
    ctx.r11.u64 = L(base, kCount);
    ctx.r4.u64 = ctx.r3.u64;
    ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
    if (!ctx.cr6.eq) {
      const uint32_t table_a = ctx.r29.u32;
      VertexLeft v{};
      uint32_t i = 0;
      uint32_t count;
      do {
        Vertex(base, st, At(base, table_a, i, 2));
        Vertex(base, st, At(base, table_a, i, 1));
        Vertex(base, st, At(base, table_a, i, 0));
        Vertex(base, st, At(base, kTableB, i, 0));
        Vertex(base, st, At(base, kTableB, i, 1));
        v = Vertex(base, st, At(base, kTableB, i, 2));
        i = (i + 1) & 0xFFFF;  // clrlwi r11,r11,16
        count = L(base, kCount);
      } while (i < count);
      // The registers the last block + the loop's test leave.
      ctx.r3.u64 = kHi8257 + 22776;  // 0x825758F8, sign-extended
      ctx.r5.u64 = kHi824F;
      ctx.r6.u64 = ctx.r3.u64 + 4;
      ctx.r7.u64 = v.pos_after;
      ctx.r8.u64 = v.count_plus_1;
      ctx.r9.u64 = v.pos_before;
      ctx.r10.u64 = count;
      ctx.r11.u64 = i;
      ctx.r25.u64 = ctx.r3.u64 + 4;
      ctx.r28.u64 = i;
      ctx.r31.u64 = kHi8257 + 8768;  // 0x82572240
      ctx.r30.u64 = ctx.r31.u64 + 4;
      ctx.f0.f64 = v.z;
      ctx.f13.f64 = v.y;
      ctx.f12.f64 = v.x;
      ctx.cr6.compare<uint32_t>(i, count, ctx.xer);
    }
    // EndPrims(context).
    ctx.r3.u64 = L(base, kContext);
    ctx.r11.u64 = L(base, ctx.r3.u32);
    ctx.r10.u64 = L(base, ctx.r11.u32 + 128);
    ctx.ctr.u64 = ctx.r10.u64;
    ctx.lr = 0x823936A4;
    calls.end(ctx, base);
  }
  // Epilogue: addi r1,r1,144; restgpr r25-r31 + lr from the stack.
  ctx.r1.s64 = ctx.r1.s64 + 144;
  const uint32_t top = ctx.r1.u32;
  ctx.r12.u64 = L(base, top - 8);
  ctx.lr = ctx.r12.u64;
  ctx.r25.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 64)));
  ctx.r26.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 56)));
  ctx.r27.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 48)));
  ctx.r28.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 40)));
  ctx.r29.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 32)));
  ctx.r30.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 24)));
  ctx.r31.u64 = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(GuestPtr(base, top - 16)));
  return true;
}

// --- The check: the original's two calls TAPED, then REPLAYED to ours ----------
// The usual check (decomp/check.h) runs the original and then ours; here that
// would draw twice and give ours ANOTHER vertex buffer (other addresses, other
// registers). Instead, while the original runs, the SDK's indirect-call table
// entries of BeginPrims / EndPrims point at Tape*: each records the register
// file going in and coming out (and BeginPrims' stream + vertex bytes right
// after it, EndPrims' right before it). Ours then runs with Replay*: the
// register file it hands each call is COMPARED with the original's (the
// strictest test of the loop: every register at EndPrims), the stream object
// and every vertex byte are compared at EndPrims, and the call's recorded
// output is given back. The game sees one draw, the original's.
// The table: 8-byte host pointers per guest instruction after the image
// (crash_mom_pch.h REX_LOOKUP_FUNC: image 0x82000000 + 0x650000, code base
// 0x820B0000). Swapped only on the main thread, only while checking.
constexpr uint64_t kImageEnd = 0x82000000ull + 0x650000ull, kCodeBase = 0x820B0000ull;
inline PPCFunc** TableSlot(uint8_t* base, uint32_t guest) {
  return reinterpret_cast<PPCFunc**>(base + kImageEnd + (uint64_t(guest) - kCodeBase) * 2);
}

struct Region {
  uint32_t address = 0, bytes = 0;
  std::vector<uint8_t> data;
  void Take(uint8_t* base) {
    const uint8_t* p = GuestPtr(base, address);
    data.assign(p, p + bytes);
  }
  void Put(uint8_t* base) const {
    if (bytes) std::memcpy(GuestPtr(base, address), data.data(), bytes);
  }
  bool Same(uint8_t* base) const { return !bytes || std::memcmp(GuestPtr(base, address), data.data(), bytes) == 0; }
};
struct Tape {
  bool begun = false, ended = false;
  PPCContext begin_in{}, begin_out{}, end_in{}, end_out{};
  Region stream_after_begin, verts_after_begin, stream_at_end, verts_at_end;
  std::string differs;  // the first difference the replay saw
} g_tape;

// The stream's 32 bytes and the span its columns will cover for `vertices`.
void FindRegions(uint8_t* base, uint32_t st, uint32_t vertices, Region& stream, Region& verts) {
  stream.address = st;
  stream.bytes = 32;
  verts.address = verts.bytes = 0;
  const uint32_t stride = L(base, st);
  uint32_t lo = 0xFFFFFFFFu;
  uint64_t hi = 0;
  for (uint32_t c = 4; c <= 24; c += 4) {
    const uint32_t d = L(base, st + c);
    if (!d) continue;
    lo = d < lo ? d : lo;
    const uint64_t e = uint64_t(d) + uint64_t(stride) * vertices + 12;
    hi = e > hi ? e : hi;
  }
  if (hi > lo && hi - lo <= (8u << 20) && hi <= 0xFFFFFFFFull) {
    verts.address = lo;
    verts.bytes = uint32_t(hi - lo);
  }
}

void TapeBegin(PPCContext& ctx, uint8_t* base) {
  g_tape.begin_in = ctx;
  sub_82430780(ctx, base);
  g_tape.begin_out = ctx;
  g_tape.begun = true;
  FindRegions(base, ctx.r3.u32, g_tape.begin_in.r7.u32, g_tape.stream_after_begin, g_tape.verts_after_begin);
  g_tape.stream_after_begin.Take(base);
  g_tape.verts_after_begin.Take(base);
}
void TapeEnd(PPCContext& ctx, uint8_t* base) {
  g_tape.end_in = ctx;
  g_tape.stream_at_end = g_tape.stream_after_begin;
  g_tape.verts_at_end = g_tape.verts_after_begin;
  g_tape.stream_at_end.Take(base);
  g_tape.verts_at_end.Take(base);
  sub_82430870(ctx, base);
  g_tape.end_out = ctx;
  g_tape.ended = true;
}
void Note(const std::string& what) {
  if (g_tape.differs.empty()) g_tape.differs = what;
}
void ReplayBegin(PPCContext& ctx, uint8_t*) {
  if (!g_tape.begun) return Note("BeginPrims called, the original didn't");
  const std::string d = decomp::RegistersDiffer(g_tape.begin_in, ctx);
  if (!d.empty()) Note("at BeginPrims: " + d);
  ctx = g_tape.begin_out;
}
void ReplayEnd(PPCContext& ctx, uint8_t* base) {
  if (!g_tape.ended) return Note("EndPrims called, the original didn't");
  const std::string d = decomp::RegistersDiffer(g_tape.end_in, ctx);
  if (!d.empty()) Note("at EndPrims: " + d);
  if (!g_tape.stream_at_end.Same(base)) Note("the stream object at EndPrims");
  if (!g_tape.verts_at_end.Same(base)) Note("the vertex bytes at EndPrims");
  ctx = g_tape.end_out;
}
constexpr Calls kReplayCalls = {&ReplayBegin, &ReplayEnd};

decomp::Stats g_check{"shadow caps"};

void TapedCheck(PPCContext& ctx, uint8_t* base) {
  g_tape = Tape{};
  Region stack;
  stack.address = ctx.r1.u32 - 144;
  stack.bytes = 144;
  const Region stack_before = (stack.Take(base), stack);
  // 1. The original, its two calls taped.
  PPCFunc** begin_slot = TableSlot(base, kBeginPrims);
  PPCFunc** end_slot = TableSlot(base, kEndPrims);
  PPCFunc* const begin_real = *begin_slot;
  PPCFunc* const end_real = *end_slot;
  *begin_slot = &TapeBegin;
  *end_slot = &TapeEnd;
  PPCContext answer = ctx;
  __imp__sub_82393120(answer, base);
  *begin_slot = begin_real;
  *end_slot = end_real;
  Region stack_after = stack, stream_after = g_tape.stream_at_end, verts_after = g_tape.verts_at_end;
  stack_after.Take(base);
  stream_after.Take(base);
  verts_after.Take(base);
  // 2. Ours from the same start, the calls replayed.
  stack_before.Put(base);
  g_tape.stream_after_begin.Put(base);
  g_tape.verts_after_begin.Put(base);
  PPCContext mine = ctx;
  mine.fpscr.disableFlushMode();
  const bool handled = Caps(mine, base, kReplayCalls);
  if (handled) {
    if (g_tape.begun && !g_tape.ended) Note("EndPrims not called");
    const std::string d = decomp::RegistersDiffer(answer, mine);
    if (!d.empty()) Note("at the return: " + d);
    if (!stack_after.Same(base)) Note("the stack frame");
  }
  ++g_check.calls;
  if (g_check.calls == 1) decomp::AllStats().push_back(&g_check);
  if (handled && !g_tape.differs.empty()) {
    ++g_check.differed;
    if (g_check.described < 8) {
      ++g_check.described;
      REXLOG_WARN("Native check: shadow caps DIFFERS (r3 {:08X}): {}", ctx.r3.u32, g_tape.differs);
    }
  }
  // 3. The original's results stay.
  stack_after.Put(base);
  stream_after.Put(base);
  verts_after.Put(base);
  ctx = answer;
  decomp::MaybeReport();
}


}  // namespace
}  // namespace decomp_shadow_caps

extern "C" REX_FUNC(sub_82393120) {
  using namespace decomp_shadow_caps;
  if (!REXCVAR_GET(native_shadow_caps)) {
    __imp__sub_82393120(ctx, base);
  } else if (REXCVAR_GET(debug_native_shadow_check)) {
    TapedCheck(ctx, base);
  } else {
    ctx.fpscr.disableFlushMode();
    if (!Caps(ctx, base, kRealCalls)) __imp__sub_82393120(ctx, base);
  }
}
