// =============================================================================
// decomp/shadow_volume.cpp -- a stencil shadow volume's caps (sub_82393120)
// and sides (sub_823936B0) streamed as triangles, as C++, bit-identical
// (docs/findings/60 s.16-17)
// =============================================================================
//
// WHAT THIS IS
//   sub_823949A0 draws a stencil shadow volume (findings/12) in pieces, two of
//   them these (r3 = the shadow object). Both stream position-only triangles
//   as immediate geometry through the PDDI context at [0x82571B7C]:
//   BeginPrims (context vtable +124 = xnContext::BeginPrims 0x82430780) with
//   the material at [0x825758F4]+16, prim type 0, vertex format 0x2020 and
//   the vertex count; the vertices; EndPrims (vtable +128 = 0x82430870).
//   Two vertex tables: A = *(*(object +72) +16), the shape's own vertices,
//   and B = 0x825758F8, the same vertices pushed away (presumably the
//   extruded copy: name TBD); 3 floats each, signed 16-bit indices.
//
//   CAPS (sub_82393120): N = the u32 at 0x8257223C triangles, 3 indices each
//   at 0x82572240 (6 bytes); 6N vertices: A[i2] A[i1] A[i0] (one cap, wound
//   backwards), then B[i0] B[i1] B[i2]. Nothing at all when N is 0.
//   SIDES (sub_823936B0): first the material's vtable +32 with 0x46464944 and
//   the colour word (setting a parameter: name TBD); then E = the u32 at
//   0x82572238 edges, 2 indices each at 0x825739B0 (4 bytes); 6E vertices
//   per edge: A[e0] B[e0] A[e1], A[e1] B[e0] B[e1] (a quad as two triangles).
//   BeginPrims / EndPrims run even when E is 0.
//
//   Each vertex: the colour word at 0x824EA558 (0x00FF0000) into the stream's
//   colour column (+20), x y z into its position column (+4), then the
//   stream's count (+28) += 1 and the columns moved on by the stride (+0):
//   +4 always, +8 +12 +16 +20 +24 when not null. The original re-reads the
//   stream's fields around every store; together ~1.3% of the main thread
//   uncapped on Wumpa Island.
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x82393120-0x82393C50 next to the
//   recompiled C++ (generated/default). Every guest store happens in the
//   original's order; floats go through lfs / stfs (double(float) and back).
//
// REGISTERS: each virtual call is made through the SDK's indirect-call table,
//   exactly like the original's bctrl (so the PDDI intercept and the recorder
//   see it), with EXACTLY the register file the original gives it; the return
//   leaves what the original's epilogue leaves. 64-bit details kept: `lis`
//   values are sign-extended (0xFFFFFFFF8257....), adds are 64-bit.
//
// WHEN THE ORIGINAL RUNS INSTEAD: a context whose vtable slots +124 / +128
//   aren't BeginPrims / EndPrims, or a call target the table doesn't hold
//   (checked before anything is written).
//
// SWITCHES: --native_shadow_caps / --native_shadow_sides (default on);
//   --debug_native_shadow_check = original + ours on every call, the virtual
//   calls TAPED from the original and REPLAYED to ours (TapedCheck below):
//   every register at each call and at the return, the stack frame, the
//   stream object and every vertex byte compared; the game still draws once.
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
extern "C" REX_FUNC(__imp__sub_823936B0);

REXCVAR_DEFINE_BOOL(native_shadow_caps, true, "CrashMoM",
                    "A shadow volume's caps streamed as triangles (sub_82393120) run as our C++ (same results); "
                    "false = the recompiled original");
REXCVAR_DEFINE_BOOL(native_shadow_sides, true, "CrashMoM",
                    "A shadow volume's sides streamed as triangles (sub_823936B0) run as our C++ (same results); "
                    "false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_shadow_check, false, "CrashMoM",
                    "Debug: run the original shadow-volume geometry writers AND ours on every call and compare "
                    "memory and every register (slow; log lines \"Native check:\")");

namespace decomp_shadow_volume {
namespace {

constexpr uint32_t kCapCount = 0x8257223C;   // triangles (u32)
constexpr uint32_t kCapIndices = 0x82572240; // 3 x s16 per triangle
constexpr uint32_t kSideCount = 0x82572238;  // edges (u32)
constexpr uint32_t kSideIndices = 0x825739B0;  // 2 x s16 per edge
constexpr uint32_t kTableB = 0x825758F8;     // 3 floats per vertex
constexpr uint32_t kColour = 0x824EA558;     // the colour word every vertex gets
constexpr uint32_t kContext = 0x82571B7C;    // the PDDI context pointer
constexpr uint32_t kMaterial = 0x825758F4;   // [it] + 16 = the material
constexpr uint32_t kBeginPrims = 0x82430780, kEndPrims = 0x82430870;
// `lis rX,-32169` / `lis rX,-32177`: sign-extended in the 64-bit registers.
constexpr uint64_t kHi8257 = 0xFFFFFFFF82570000ull, kHi824F = 0xFFFFFFFF824F0000ull;

inline uint32_t L(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint64_t L64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  return __builtin_bswap64(v);
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

// The SDK's indirect-call table (crash_mom_pch.h REX_LOOKUP_FUNC): an 8-byte
// host pointer per guest instruction after the image (0x82000000 + 0x650000),
// from the code base 0x820B0000 (code 0x41C51C + 0x10000 thunks).
constexpr uint64_t kImageEnd = 0x82000000ull + 0x650000ull, kCodeBase = 0x820B0000ull;
constexpr uint64_t kCodeRange = 0x41C51Cull + 0x10000ull;
inline PPCFunc** TableSlot(uint8_t* base, uint32_t guest) {
  return reinterpret_cast<PPCFunc**>(base + kImageEnd + (uint64_t(guest) - kCodeBase) * 2);
}
inline PPCFunc* Target(uint8_t* base, uint32_t guest) {
  if (uint64_t(guest) - kCodeBase >= kCodeRange || (guest & 3)) return nullptr;
  return *TableSlot(base, guest);
}

// The virtual calls in the order the function makes them: the real ones (from
// the table), or the check's replay.
struct Calls {
  PPCFunc* fn[3];
};

// One vertex: colour, x y z from `at`, then the stream's count and columns
// moved on. Returns what the LAST block leaves in registers.
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
// Index k of entry i (`size` bytes each) -> the vertex's address in a table
// (12-byte entries, signed index, 32-bit address arithmetic like lfs / lfsx).
inline uint32_t At(uint8_t* base, uint32_t indices, uint32_t size, uint32_t table, uint32_t i, uint32_t k) {
  const int32_t idx = int16_t(L16(base, indices + size * i + 2 * k));
  return table + uint32_t(idx) * 12u;
}

// Is the context's vtable the one both functions expect?
inline bool ContextOk(uint8_t* base) {
  const uint32_t vt = L(base, L(base, kContext));
  return L(base, vt + 124) == kBeginPrims && L(base, vt + 128) == kEndPrims &&
         Target(base, kBeginPrims) && Target(base, kEndPrims);
}

// r25-r31 by number (the context's fields are named, not an array).
inline PPCRegister& Gpr(PPCContext& ctx, int r) {
  switch (r) {
    case 25: return ctx.r25;
    case 26: return ctx.r26;
    case 27: return ctx.r27;
    case 28: return ctx.r28;
    case 29: return ctx.r29;
    case 30: return ctx.r30;
    default: return ctx.r31;
  }
}
// Prologue of both: mflr r12; savegpr rFirst-r31 + lr; stwu r1,-144(r1).
inline void Prologue(PPCContext& ctx, uint8_t* base, int first) {
  ctx.r12.u64 = ctx.lr;
  const uint32_t r1 = ctx.r1.u32;
  for (int r = first; r <= 31; ++r) S64(base, r1 - 8 * (32 - r) - 8, Gpr(ctx, r).u64);
  S(base, r1 - 8, uint32_t(ctx.lr));
  S(base, r1 - 144, r1);
  ctx.r1.u64 = r1 - 144;
}
// Epilogue: addi r1,r1,144; restgpr rFirst-r31 + lr from the stack.
inline void Epilogue(PPCContext& ctx, uint8_t* base, int first) {
  ctx.r1.s64 = ctx.r1.s64 + 144;
  const uint32_t top = ctx.r1.u32;
  ctx.r12.u64 = L(base, top - 8);
  ctx.lr = ctx.r12.u64;
  for (int r = first; r <= 31; ++r) Gpr(ctx, r).u64 = L64(base, top - 8 * (32 - r) - 8);
}
inline void Call(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t return_to) {
  ctx.lr = return_to;
  fn(ctx, base);
}

// ---- The caps (sub_82393120) -------------------------------------------------
bool Caps(PPCContext& ctx, uint8_t* base, const Calls& calls) {
  const uint32_t shadow = ctx.r3.u32;
  Prologue(ctx, base, 25);

  ctx.r27.u64 = kHi8257;
  ctx.r10.u64 = L(base, shadow + 72);
  ctx.r11.u64 = L(base, kCapCount);
  ctx.r29.u64 = L(base, ctx.r10.u32 + 16);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
  if (!ctx.cr6.eq) {
    // BeginPrims(context, material, 0, 0x2020, 6N, 0).
    const uint64_t n = ctx.r11.u64;
    ctx.r26.u64 = kHi8257;
    const uint64_t n2 = uint32_t(n << 1);
    ctx.r8.u64 = 0;
    ctx.r7.u64 = uint32_t((n + n2) << 1);
    ctx.r3.u64 = L(base, kContext);
    ctx.r6.u64 = 8224;
    ctx.r11.u64 = L(base, kMaterial);
    ctx.r5.u64 = 0;
    ctx.r10.u64 = L(base, ctx.r3.u32);
    ctx.r4.u64 = L(base, ctx.r11.u32 + 16);
    ctx.r9.u64 = L(base, ctx.r10.u32 + 124);
    ctx.ctr.u64 = ctx.r9.u64;
    Call(ctx, base, calls.fn[0], 0x82393180);

    const uint32_t st = ctx.r3.u32;
    ctx.r11.u64 = L(base, kCapCount);
    ctx.r4.u64 = ctx.r3.u64;
    ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
    if (!ctx.cr6.eq) {
      const uint32_t a = ctx.r29.u32;
      VertexLeft v{};
      uint32_t i = 0, count;
      do {
        Vertex(base, st, At(base, kCapIndices, 6, a, i, 2));
        Vertex(base, st, At(base, kCapIndices, 6, a, i, 1));
        Vertex(base, st, At(base, kCapIndices, 6, a, i, 0));
        Vertex(base, st, At(base, kCapIndices, 6, kTableB, i, 0));
        Vertex(base, st, At(base, kCapIndices, 6, kTableB, i, 1));
        v = Vertex(base, st, At(base, kCapIndices, 6, kTableB, i, 2));
        i = (i + 1) & 0xFFFF;  // clrlwi r11,r11,16
        count = L(base, kCapCount);
      } while (i < count);
      // What the last block + the loop's test leave.
      ctx.r3.u64 = kHi8257 + 22776;  // 0x825758F8
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
    Call(ctx, base, calls.fn[1], 0x823936A4);
  }
  Epilogue(ctx, base, 25);
  return true;
}

// ---- The sides (sub_823936B0) ------------------------------------------------
bool Sides(PPCContext& ctx, uint8_t* base, const Calls& calls) {
  Prologue(ctx, base, 26);

  // The material's vtable +32 (0x46464944, the colour word).
  ctx.r29.u64 = kHi8257;
  ctx.r30.u64 = ctx.r3.u64;
  ctx.r31.u64 = kHi824F;
  ctx.r4.u64 = 0x46464944;
  ctx.r11.u64 = L(base, kMaterial);
  ctx.r5.u64 = L(base, kColour);
  ctx.r3.u64 = L(base, ctx.r11.u32 + 16);
  ctx.r11.u64 = L(base, ctx.r3.u32);
  ctx.r10.u64 = L(base, ctx.r11.u32 + 32);
  ctx.ctr.u64 = ctx.r10.u64;
  Call(ctx, base, calls.fn[0], 0x823936EC);

  // BeginPrims(context, material, 0, 0x2020, 6E, 0).
  ctx.r28.u64 = kHi8257;
  ctx.r27.u64 = kHi8257;
  ctx.r10.u64 = L(base, kMaterial);
  ctx.r8.u64 = 0;
  ctx.r6.u64 = 8224;
  ctx.r5.u64 = 0;
  ctx.r11.u64 = L(base, kSideCount);
  ctx.r3.u64 = L(base, kContext);
  ctx.r9.u64 = uint32_t(ctx.r11.u64 << 1);
  ctx.r4.u64 = L(base, ctx.r10.u32 + 16);
  ctx.r9.u64 = ctx.r11.u64 + ctx.r9.u64;
  ctx.r7.u64 = uint32_t(ctx.r9.u64 << 1);
  ctx.r11.u64 = L(base, ctx.r3.u32);
  ctx.r10.u64 = L(base, ctx.r11.u32 + 124);
  ctx.ctr.u64 = ctx.r10.u64;
  Call(ctx, base, calls.fn[1], 0x8239372C);

  const uint32_t st = ctx.r3.u32;
  ctx.r9.u64 = L(base, ctx.r30.u32 + 72);
  ctx.r11.u64 = L(base, kSideCount);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
  ctx.r30.u64 = L(base, ctx.r9.u32 + 16);
  if (!ctx.cr6.eq) {
    const uint32_t a = ctx.r30.u32;
    VertexLeft v{};
    uint32_t i = 0, count;
    do {
      Vertex(base, st, At(base, kSideIndices, 4, a, i, 0));
      Vertex(base, st, At(base, kSideIndices, 4, kTableB, i, 0));
      Vertex(base, st, At(base, kSideIndices, 4, a, i, 1));
      Vertex(base, st, At(base, kSideIndices, 4, a, i, 1));
      Vertex(base, st, At(base, kSideIndices, 4, kTableB, i, 0));
      v = Vertex(base, st, At(base, kSideIndices, 4, kTableB, i, 1));
      i = (i + 1) & 0xFFFF;  // clrlwi r11,r11,16
      count = L(base, kSideCount);
    } while (i < count);
    // What the last block + the loop's test leave.
    ctx.r3.u64 = kHi8257 + 14768;  // 0x825739B0
    ctx.r5.u64 = kHi8257 + 22776;  // 0x825758F8
    ctx.r6.u64 = v.pos_after;
    ctx.r7.u64 = v.count_plus_1;
    ctx.r8.u64 = v.pos_before;
    ctx.r9.u64 = v.pos_before;
    ctx.r10.u64 = count;
    ctx.r11.u64 = i;
    ctx.r26.u64 = ctx.r5.u64 + 4;
    ctx.r29.u64 = i;
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
  Call(ctx, base, calls.fn[2], 0x82393C48);

  Epilogue(ctx, base, 26);
  return true;
}

// The sides' first call: the material's vtable +32 (0 = not in the table).
inline uint32_t MaterialSetter(uint8_t* base) {
  const uint32_t material = L(base, L(base, kMaterial) + 16);
  return L(base, L(base, material) + 32);
}

// ---- The check: the original's calls TAPED, then REPLAYED to ours -------------
// The usual check (decomp/check.h) runs the original and then ours; here that
// would draw twice and give ours ANOTHER vertex buffer (other addresses, other
// registers). Instead, while the original runs, the SDK's indirect-call table
// entries of its virtual calls point at Tape*: each records the register file
// going in and coming out (BeginPrims also the stream object + vertex bytes
// right after it, EndPrims right before it). Ours then runs with Replay*: the
// register file it hands each call is COMPARED with the original's (at
// EndPrims that is every register the loop leaves), the stream object and
// every vertex byte are compared at EndPrims, and the call's recorded output
// is given back. The game draws once, the original's draw. Main thread only
// (the table swap is seen by every thread: debug only).
enum Kind { kPlain, kBegin, kEnd };
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
struct TapedCall {
  uint32_t address = 0;
  Kind kind = kPlain;
  PPCFunc* real = nullptr;
  bool made = false, replayed = false;
  PPCContext in{}, out{};
};
struct Tape {
  TapedCall calls[3];
  int n = 0;
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
void Note(const std::string& what) {
  if (g_tape.differs.empty()) g_tape.differs = what;
}

template <int I>
void TapeCall(PPCContext& ctx, uint8_t* base) {
  TapedCall& c = g_tape.calls[I];
  c.in = ctx;
  if (c.kind == kEnd) {
    g_tape.stream_at_end = g_tape.stream_after_begin;
    g_tape.verts_at_end = g_tape.verts_after_begin;
    g_tape.stream_at_end.Take(base);
    g_tape.verts_at_end.Take(base);
  }
  c.real(ctx, base);
  c.out = ctx;
  c.made = true;
  if (c.kind == kBegin) {
    FindRegions(base, ctx.r3.u32, c.in.r7.u32, g_tape.stream_after_begin, g_tape.verts_after_begin);
    g_tape.stream_after_begin.Take(base);
    g_tape.verts_after_begin.Take(base);
  }
}
template <int I>
void ReplayCall(PPCContext& ctx, uint8_t* base) {
  TapedCall& c = g_tape.calls[I];
  if (!c.made) return Note(fmt::format("call {} made, the original didn't make it", I));
  c.replayed = true;
  const std::string d = decomp::RegistersDiffer(c.in, ctx);
  if (!d.empty()) Note(fmt::format("at call {} ({:08X}): {}", I, c.address, d));
  if (c.kind == kEnd) {
    if (!g_tape.stream_at_end.Same(base)) Note("the stream object at EndPrims");
    if (!g_tape.verts_at_end.Same(base)) Note("the vertex bytes at EndPrims");
  }
  ctx = c.out;
}
constexpr PPCFunc* kTapes[3] = {&TapeCall<0>, &TapeCall<1>, &TapeCall<2>};
constexpr Calls kReplay = {{&ReplayCall<0>, &ReplayCall<1>, &ReplayCall<2>}};

template <class Ours>
void TapedCheck(decomp::Stats& st, PPCContext& ctx, uint8_t* base, PPCFunc* original, const uint32_t* targets,
                const Kind* kinds, int n, Ours ours) {
  g_tape = Tape{};
  g_tape.n = n;
  Region stack;
  stack.address = ctx.r1.u32 - 144;
  stack.bytes = 144;
  stack.Take(base);
  const Region stack_before = stack;
  // 1. The original, its calls taped.
  for (int i = 0; i < n; ++i) {
    TapedCall& c = g_tape.calls[i];
    c.address = targets[i];
    c.kind = kinds[i];
    c.real = *TableSlot(base, targets[i]);
    *TableSlot(base, targets[i]) = kTapes[i];
  }
  PPCContext answer = ctx;
  original(answer, base);
  for (int i = n - 1; i >= 0; --i) *TableSlot(base, targets[i]) = g_tape.calls[i].real;
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
  ours(mine, kReplay);
  for (int i = 0; i < n; ++i) {
    if (g_tape.calls[i].made && !g_tape.calls[i].replayed) Note(fmt::format("call {} not made by ours", i));
  }
  const std::string d = decomp::RegistersDiffer(answer, mine);
  if (!d.empty()) Note("at the return: " + d);
  if (!stack_after.Same(base)) Note("the stack frame");
  ++st.calls;
  if (st.calls == 1) decomp::AllStats().push_back(&st);
  if (!g_tape.differs.empty()) {
    ++st.differed;
    if (st.described < 8) {
      ++st.described;
      REXLOG_WARN("Native check: {} DIFFERS (r3 {:08X}): {}", st.name, ctx.r3.u32, g_tape.differs);
    }
  }
  // 3. The original's results stay.
  stack_after.Put(base);
  stream_after.Put(base);
  verts_after.Put(base);
  ctx = answer;
  decomp::MaybeReport();
}

decomp::Stats g_caps{"shadow caps"}, g_sides{"shadow sides"};

}  // namespace
}  // namespace decomp_shadow_volume

using namespace decomp_shadow_volume;

extern "C" REX_FUNC(sub_82393120) {
  if (!REXCVAR_GET(native_shadow_caps) || !ContextOk(base)) return __imp__sub_82393120(ctx, base);
  static const uint32_t targets[2] = {kBeginPrims, kEndPrims};
  static const Kind kinds[2] = {kBegin, kEnd};
  if (REXCVAR_GET(debug_native_shadow_check)) {
    TapedCheck(g_caps, ctx, base, &__imp__sub_82393120, targets, kinds, 2,
               [&](PPCContext& c, const Calls& calls) { Caps(c, base, calls); });
    return;
  }
  ctx.fpscr.disableFlushMode();
  const Calls real = {{Target(base, kBeginPrims), Target(base, kEndPrims), nullptr}};
  Caps(ctx, base, real);
}

extern "C" REX_FUNC(sub_823936B0) {
  const uint32_t setter = MaterialSetter(base);
  if (!REXCVAR_GET(native_shadow_sides) || !ContextOk(base) || !Target(base, setter)) {
    return __imp__sub_823936B0(ctx, base);
  }
  const uint32_t targets[3] = {setter, kBeginPrims, kEndPrims};
  static const Kind kinds[3] = {kPlain, kBegin, kEnd};
  if (REXCVAR_GET(debug_native_shadow_check)) {
    TapedCheck(g_sides, ctx, base, &__imp__sub_823936B0, targets, kinds, 3,
               [&](PPCContext& c, const Calls& calls) { Sides(c, base, calls); });
    return;
  }
  ctx.fpscr.disableFlushMode();
  const Calls real = {{Target(base, setter), Target(base, kBeginPrims), Target(base, kEndPrims)}};
  Sides(ctx, base, real);
}
