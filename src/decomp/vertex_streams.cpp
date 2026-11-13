// =============================================================================
// decomp/vertex_streams.cpp -- the prim stream's "position + normal +
// binormal + tangent + uv" writer (sub_82439A48) as C++, bit-identical
// (docs/findings/60 s.13)
// =============================================================================
//
// WHAT THIS IS
//   Radical's geometry streaming fills an xnPrimBufferStream (the object
//   at xnContext +428, findings/09 / 11) one column at a time: r3 = the
//   stream, whose words are
//       +0  stride (bytes from one vertex to the next)
//       +4  where the next POSITION goes      +8  the next NORMAL
//       +12 the next BINORMAL                 +16 the next TANGENT
//       +24 the next UV                       +28 how many were written
//   A family of writers (0x824398E8, 0x82439A48, 0x82439B88, 0x82439CC0:
//   one per vertex format, picked by sub_823E1780 from which arrays exist)
//   copies a batch of vertices in. This one copies all five columns and was
//   the largest piece of the renderer's own code left: ~3.1% of the main
//   thread uncapped on Wumpa Island (findings/60 s.13). Arguments:
//       r4 count, r5 positions (3 floats each), r8 normals, r7 binormals,
//       r6 tangents (3 floats each), r9 uvs (2 floats each).
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x82439A48-0x82439B88 next to the
//   recompiled C++ (generated/default, DEFINE_REX_FUNC(sub_82439A48)).
//
// WHAT THE ORIGINAL DOES
//   savegpr r22-r31 + the return address below r1 (0x8245C110); then, for
//   each vertex: position x y z -> [+4] +0/+4/+8, normal -> [+8], binormal ->
//   [+12], tangent -> [+16], uv u v -> [+24] (re-reading each column pointer
//   from the stream before EVERY store), every column pointer += stride;
//   finally +28 = the count. Each float goes through lfs / stfs: float ->
//   double -> float in the recompiled code, which keeps every value except
//   a SIGNALLING NaN, which comes out quiet (bit 22 set). Copied here as
//   raw words with that one bit fix-up.
//
// REGISTERS: one caller (sub_823E1780) tail-returns into its own caller,
//   which is reached through a vtable (sub_823F32D0), so "nobody reads a
//   scratch register after the call" can't be proven by a scan the way it
//   was for the set-ups. So EVERY register is left as the original leaves
//   it: the loop's pointers (r5-r11), r12 / lr (the restgpr), the last
//   vertex's 14 floats in f0-f13 (as lfs loaded them), cr0 / cr6 and xer.ca.
//   The check compares all registers (decomp/check.h Ranges::registers).
//
// WHEN THE ORIGINAL RUNS INSTEAD: a stream column that overlaps a source
//   array or the stream object itself (the original re-reads pointers and
//   sources between stores: order would matter), or a range that wraps or
//   crosses 0xE0000000 (the Windows host window shift, guest_memory.h).
//   Dest columns overlapping EACH OTHER (an interleaved vertex) are fine:
//   ours stores in the original's order.
//
// SWITCHES: --native_vertex_streams (default on); --debug_native_vertex_check
//   = original + ours on every call, compared byte for byte and register for
//   register.
// =============================================================================

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "check.h"

extern "C" REX_FUNC(__imp__sub_82439A48);

REXCVAR_DEFINE_BOOL(native_vertex_streams, true, "CrashMoM",
                    "The prim stream's position/normal/binormal/tangent/uv writer (sub_82439A48) runs as our C++ "
                    "(same results); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_vertex_check, false, "CrashMoM",
                    "Debug: run the original vertex stream writer AND ours on every call and compare memory and "
                    "every register (slow; log lines \"Native check:\")");

namespace decomp_vertex_streams {
namespace {

inline uint32_t L(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline void S(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, a), &v, 4);
}
inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
}

// A guest range [a, a + n) usable as ONE host block: no wrap, not across
// 0xE0000000 (the Windows window shift; guest_memory.h).
inline bool Contiguous(uint32_t a, uint64_t n) {
  const uint64_t end = uint64_t(a) + n;
  if (end > 0x100000000ull) return false;
  return !(a < 0xE0000000u && end > 0xE0000000ull);
}
inline bool Overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

// lfs + stfs on a raw word: unchanged, but a SIGNALLING NaN gets its quiet
// bit (the float -> double conversion).
// For 16 raw (big-endian) words at once, branch-free: ANY NaN gets bit 22 (a
// quiet NaN already has it, so only signalling ones change).
inline void QuietVertex(uint32_t* w) {
  for (int j = 0; j < 16; ++j) {
    const uint32_t x = __builtin_bswap32(w[j]);
    w[j] |= ((x & 0x7FFFFFFFu) > 0x7F800000u) ? __builtin_bswap32(0x00400000u) : 0u;
  }
}
// What lfs leaves in an FPR: the float widened to double.
inline uint64_t AsLoaded(const uint8_t* from) {
  uint32_t v;
  std::memcpy(&v, from, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  const double d = f;
  uint64_t out;
  std::memcpy(&out, &d, 8);
  return out;
}

// What the check runs covered ("covered" log line): calls ours handled,
// calls it left to the original (overlap / window), vertices copied.
struct Coverage {
  uint64_t handled = 0, fell_back = 0, vertices = 0;
} g_seen;

bool WriteBody(PPCContext& ctx, uint8_t* base);
bool Write(PPCContext& ctx, uint8_t* base) {
  const bool ok = WriteBody(ctx, base);
  if (ok) {
    ++g_seen.handled;
    g_seen.vertices += ctx.r4.u32;
  } else {
    ++g_seen.fell_back;
  }
  return ok;
}

bool WriteBody(PPCContext& ctx, uint8_t* base) {
  const uint32_t st = ctx.r3.u32;
  const uint32_t n = ctx.r4.u32;
  // Entry values (the loop's walkers are built from them).
  const uint64_t r5_0 = ctx.r5.u64, r6_0 = ctx.r6.u64, r7_0 = ctx.r7.u64;
  const uint32_t src[5] = {ctx.r5.u32, ctx.r8.u32, ctx.r7.u32, ctx.r6.u32, ctx.r9.u32};
  static constexpr uint32_t kColumn[5] = {4, 8, 12, 16, 24};
  uint32_t stride = 0, dst[5] = {};
  const uint8_t* sp[5] = {};

  if (n) {
    // Is the fast copy the same as the original's interleaved re-reads?
    if (!Contiguous(st, 32)) return false;
    stride = L(base, st + 0);
    const uint64_t src_bytes[5] = {12ull * n, 12ull * n, 12ull * n, 12ull * n, 8ull * n};
    const uint64_t span = uint64_t(stride) * (n - 1);
    for (int k = 0; k < 5; ++k) {
      dst[k] = L(base, st + kColumn[k]);
      if (!Contiguous(src[k], src_bytes[k])) return false;
    }
    for (int k = 0; k < 5; ++k) {
      const uint64_t bytes = span + (k == 4 ? 8 : 12);
      if (!Contiguous(dst[k], bytes) || Overlap(dst[k], bytes, st, 32)) return false;
      for (int s = 0; s < 5; ++s) {
        if (Overlap(dst[k], bytes, src[s], src_bytes[s])) return false;
      }
    }
  }

  // savegpr r22-r31 + the return address (0x8245C110; r12 = lr from mflr).
  const uint32_t r1 = ctx.r1.u32;
  const uint64_t saved[10] = {ctx.r22.u64, ctx.r23.u64, ctx.r24.u64, ctx.r25.u64, ctx.r26.u64,
                              ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  for (int k = 0; k < 10; ++k) S64(base, r1 - 88 + 8 * k, saved[k]);
  S(base, r1 - 8, uint32_t(ctx.lr));
  // cmplwi cr6,r4,0
  ctx.cr6.compare<uint32_t>(n, 0, ctx.xer);

  if (n) {
    // The copy, vertex by vertex, columns in the original's order.
    uint8_t* d[5];
    for (int k = 0; k < 5; ++k) {
      d[k] = GuestPtr(base, dst[k]);
      sp[k] = GuestPtr(base, src[k]);
    }
    // One vertex at a time: its 14 words gathered, quieted together (the
    // compiler makes that loop SIMD), stored column by column in the
    // original's order.
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t w[16];
      for (int k = 0; k < 4; ++k) std::memcpy(w + 3 * k, sp[k] + 12 * i, 12);
      std::memcpy(w + 12, sp[4] + 8 * i, 8);
      w[14] = w[15] = 0;
      QuietVertex(w);
      for (int k = 0; k < 4; ++k) {
        std::memcpy(d[k], w + 3 * k, 12);
        d[k] += stride;
      }
      std::memcpy(d[4], w + 12, 8);
      d[4] += stride;
    }
    // The stream's pointers after the last pass (stw = the low 32 bits).
    for (int k = 0; k < 5; ++k) S(base, st + kColumn[k], dst[k] + stride * n);
    // The registers the last pass leaves: lwz = zero-extended words, the
    // adds 64-bit.
    const uint64_t last12 = uint32_t(dst[2] + stride * (n - 1));
    const uint64_t last16 = uint32_t(dst[3] + stride * (n - 1));
    ctx.r5.u64 = last12 + stride;
    ctx.r7.u64 = last16;
    ctx.r9.u64 = last16 + stride;
    ctx.r8.u64 = ctx.r4.u64 - n;
    ctx.r6.s64 = int64_t(r6_0) + 12 * int64_t(n);
    ctx.r10.s64 = int64_t(r7_0) + 4 + 12 * int64_t(n);
    ctx.r11.s64 = int64_t(r5_0) + 8 + 12 * int64_t(n);
    // The last vertex's floats, as lfs left them.
    const uint32_t li = n - 1;
    const uint8_t* p = sp[0] + 12 * li;
    const uint8_t* q = sp[1] + 12 * li;
    const uint8_t* bn = sp[2] + 12 * li;
    const uint8_t* t = sp[3] + 12 * li;
    const uint8_t* u = sp[4] + 8 * li;
    ctx.f0.u64 = AsLoaded(p + 0);
    ctx.f13.u64 = AsLoaded(p + 4);
    ctx.f12.u64 = AsLoaded(p + 8);
    ctx.f11.u64 = AsLoaded(q + 0);
    ctx.f10.u64 = AsLoaded(q + 4);
    ctx.f9.u64 = AsLoaded(q + 8);
    ctx.f8.u64 = AsLoaded(bn + 0);
    ctx.f7.u64 = AsLoaded(bn + 4);
    ctx.f6.u64 = AsLoaded(bn + 8);
    ctx.f5.u64 = AsLoaded(t + 0);
    ctx.f4.u64 = AsLoaded(t + 4);
    ctx.f3.u64 = AsLoaded(t + 8);
    ctx.f2.u64 = AsLoaded(u + 0);
    ctx.f1.u64 = AsLoaded(u + 4);
    // addic. r8,r8,-1 on the last pass (r8 was 1): carry set, cr0 = "equal".
    ctx.xer.ca = 1;
    ctx.cr0.compare<int32_t>(0, 0, ctx.xer);
  }
  S(base, st + 28, n);
  // restgpr: r22-r31 as they were (untouched here), r12 = lr = the saved word.
  ctx.r12.u64 = uint32_t(ctx.lr);
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

decomp::Stats g_stats{"vertex streams"};

}  // namespace
}  // namespace decomp_vertex_streams

extern "C" REX_FUNC(sub_82439A48) {
  using namespace decomp_vertex_streams;
  const bool checking = REXCVAR_GET(debug_native_vertex_check);
  decomp::Ranges ranges{};
  if (checking) {
    // The stream object, the saved registers below r1, and the span the five
    // columns cover (they interleave in one vertex buffer; a call whose span
    // is implausibly large isn't compared, only run).
    const uint32_t st = ctx.r3.u32, n = ctx.r4.u32;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    if (n) {
      const uint32_t stride = L(base, st);
      for (const uint32_t c : {4u, 8u, 12u, 16u, 24u}) {
        const uint32_t d = L(base, st + c);
        lo = d < lo ? d : lo;
        const uint64_t end = uint64_t(d) + uint64_t(stride) * n + 12;
        hi = end > hi ? uint32_t(end > 0xFFFFFFFFu ? 0xFFFFFFFFu : end) : hi;
      }
    }
    ranges = decomp::Ranges{{{st, 32}, {ctx.r1.u32 - 88, 88}}, 2};
    if (n && hi > lo && hi - lo <= (8u << 20)) {
      ranges.r[2] = {lo, hi - lo};
      ranges.n = 3;
    }
    ranges.registers = true;
  }
  decomp::Run(REXCVAR_GET(native_vertex_streams), checking, g_stats, ctx, base, &__imp__sub_82439A48, ranges,
              [&](PPCContext& c) { return Write(c, base); });
  if (checking && ((g_seen.handled + g_seen.fell_back) & 0x3FF) == 0) {
    REXLOG_INFO("Native check: vertex streams covered {} handled, {} left to the original, {} vertices",
                g_seen.handled, g_seen.fell_back, g_seen.vertices);
  }
}
