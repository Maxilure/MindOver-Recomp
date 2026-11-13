// =============================================================================
// decomp/frustum_test.cpp -- "is this box inside every plane?" (sub_822ED0C8)
// with its plane test (sub_82426498) inlined, as C++, bit-identical
// (docs/findings/60 s.14)
// =============================================================================
//
// WHAT THIS IS
//   The world's visibility code (13 call sites: the zone and draw-list walkers
//   sub_822B2268.., sub_822F2248..) asks, for each object's bounding box,
//   whether it is on the inner side of every plane of a list (the camera's
//   view volume, or a portal's). The box test:
//       r3 = the plane list, a vector: +0 first plane, +4 one past the last;
//            a plane = 16 bytes: normal nx ny nz, then d (4 floats)
//       r4 = the box: +0 min x y z, +12 max x y z (6 floats)
//       returns r3 = 1 inside / touching every plane, 0 = fully outside one.
//   Planes are walked from the LAST to the first; the first plane the box is
//   fully outside of ends the walk.
//
//   The plane test (sub_82426498, only called from here): for each axis the
//   box corner FURTHEST AGAINST the normal (normal component < 0.0 -> the
//   box's max, else its min; the constant at 0x82001868 is read, not
//   assumed), then
//       dot = nx*x + (nz*z + ny*y)    (fmuls, then two fmadds, in that order)
//       inside = !(dot > -d)          (a NaN answers "inside")
//   It's 30 instructions called ~10,000s of times a frame: ~1.2% of the main
//   thread uncapped (Wumpa Island), most of it the call itself. Alone it
//   would not pay (findings/60 s.9: a leaf that small IS its call cost), so
//   the loop that calls it is rewritten with the test inlined.
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x822ED0C8-0x822ED138 and
//   0x82426498-0x82426514 next to the recompiled C++ (generated/default,
//   DEFINE_REX_FUNC(sub_822ED0C8) / (sub_82426498)): every register and
//   rounding below follows those lines.
//
// ROUNDING: lfs widens a float to double; fmuls = float(a*b); fmadds =
//   float(fma(a, b, c)) in double, exactly as the recompiled code does it (the
//   game's own FMA is fused). Doubles and std::fma here give the same bits.
//
// REGISTERS: 13 callers, so (like vertex_streams.cpp) EVERY register is left
//   as the original leaves it instead of proving no caller reads one:
//     r9 = end - begin (64-bit), r10 = begin, r11 = end (no planes) or the
//     last plane test's answer, r12 = lr = the return address (restgpr),
//     cr0 + xer.ca from `srawi.`, cr6 from the last integer compare (the
//     caller's, which overwrites the plane test's float compare), and the
//     last plane test's f0 f6-f13. r28-r31 come back as they were.
//   Memory: the back chain (stwu, r1-128) and the saved r28-r31 + lr
//   (savegpr 0x8245C128: r28 at r1-40 .. r31 at r1-16, lr at r1-8).
//
// WHEN THE ORIGINAL RUNS INSTEAD: a list whose end is before its begin (the
//   original would then walk ~2^28 "planes"), or planes / box that wrap or
//   cross 0xE0000000 (the Windows window shift, guest_memory.h).
//
// SWITCHES: --native_frustum_test (default on); --debug_native_frustum_check
//   = original + ours on every call, compared byte for byte and register for
//   register (decomp/check.h).
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "check.h"
#include "cpu_features.h"

extern "C" REX_FUNC(__imp__sub_822ED0C8);

REXCVAR_DEFINE_BOOL(native_frustum_test, true, "CrashMoM",
                    "The visibility code's box-against-planes test (sub_822ED0C8 + sub_82426498) runs as our C++ "
                    "(same results); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_frustum_check, false, "CrashMoM",
                    "Debug: run the original box-against-planes test AND ours on every call and compare memory and "
                    "every register (slow; log lines \"Native check:\")");

namespace decomp_frustum_test {
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
// lfs: a big-endian float widened to double.
inline double F(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}

// [a, a + n) as ONE host block: no wrap, not across 0xE0000000.
inline bool Contiguous(uint32_t a, uint64_t n) {
  const uint64_t end = uint64_t(a) + n;
  if (end > 0x100000000ull) return false;
  return !(a < 0xE0000000u && end > 0xE0000000ull);
}

// What one plane test leaves behind (the last one's floats end up in the
// registers).
struct PlaneTest {
  double f0, f6, f7, f8, f9, f10, f11, f12, f13;
  bool inside;
};

// sub_82426498, line for line (see the header).
inline PlaneTest TestPlane(const uint8_t* plane, const uint8_t* box, double zero) {
  PlaneTest t;
  t.f9 = F(plane + 0);
  t.f10 = (t.f9 < zero) ? F(box + 12) : F(box + 0);
  const double ny = F(plane + 4);
  const double y = (ny < zero) ? F(box + 16) : F(box + 4);
  t.f13 = F(plane + 8);
  t.f0 = (t.f13 < zero) ? F(box + 20) : F(box + 8);
  t.f12 = double(float(ny * y));
  t.f11 = F(plane + 12);
  const uint64_t neg = Bits(t.f11) ^ 0x8000000000000000ull;  // fneg: the sign bit only
  std::memcpy(&t.f8, &neg, 8);
  t.f7 = double(float(std::fma(t.f13, t.f0, t.f12)));
  t.f6 = double(float(std::fma(t.f9, t.f10, t.f7)));
  t.inside = !(t.f6 > t.f8);  // ble: "not greater" (unordered = inside)
  return t;
}

inline bool Test(PPCContext& ctx, uint8_t* base) {
  const uint32_t list = ctx.r3.u32;
  const uint32_t box = ctx.r4.u32;
  if (!Contiguous(list, 8) || !Contiguous(box, 24)) return false;
  const uint32_t end = L(base, list + 4);
  const uint32_t begin = L(base, list + 0);
  // subf + srawi.: the plane count (and the carry srawi leaves).
  const uint64_t r9 = uint64_t(end) - uint64_t(begin);
  const int32_t count = int32_t(uint32_t(r9)) >> 4;
  if (count < 0) return false;
  if (count && !Contiguous(begin, uint64_t(count) * 16)) return false;

  // Prologue: savegpr r28-r31 + lr below r1, then stwu r1,-128(r1).
  const uint32_t r1 = ctx.r1.u32;
  S64(base, r1 - 40, ctx.r28.u64);
  S64(base, r1 - 32, ctx.r29.u64);
  S64(base, r1 - 24, ctx.r30.u64);
  S64(base, r1 - 16, ctx.r31.u64);
  S(base, r1 - 8, uint32_t(ctx.lr));
  S(base, r1 - 128, r1);

  ctx.r9.u64 = r9;
  ctx.r10.u64 = begin;
  ctx.r11.u64 = end;
  ctx.xer.ca = (int32_t(uint32_t(r9)) < 0) & ((uint32_t(r9) & 0xF) != 0);
  ctx.cr0.compare<int32_t>(count, 0, ctx.xer);

  uint64_t result = 1;
  if (count) {
    const uint32_t zero_bits = L(base, 0x82001868);
    float zf;
    std::memcpy(&zf, &zero_bits, 4);
    const double zero = zf;
    const uint8_t* planes = GuestPtr(base, begin);
    const uint8_t* b = GuestPtr(base, box);
    PlaneTest t{};
    int32_t left = count;
    do {
      --left;
      t = TestPlane(planes + 16 * left, b, zero);
      if (!t.inside) {
        // cmplwi cr6,r11,0 -> equal; li r3,0.
        ctx.cr6.compare<uint32_t>(0, 0, ctx.xer);
        result = 0;
        break;
      }
      // cmplwi said "not equal", then cmpwi cr6,r30,0.
      ctx.cr6.compare<int32_t>(left, 0, ctx.xer);
    } while (left != 0);
    ctx.r11.u64 = result;  // clrlwi r11,r3,24 of the last plane test
    ctx.f0.u64 = Bits(t.f0);
    ctx.f6.u64 = Bits(t.f6);
    ctx.f7.u64 = Bits(t.f7);
    ctx.f8.u64 = Bits(t.f8);
    ctx.f9.u64 = Bits(t.f9);
    ctx.f10.u64 = Bits(t.f10);
    ctx.f11.u64 = Bits(t.f11);
    ctx.f12.u64 = Bits(t.f12);
    ctx.f13.u64 = Bits(t.f13);
  }
  ctx.r3.u64 = result;
  // restgpr: r28-r31 as they were (untouched), r12 = lr = the saved word.
  ctx.r12.u64 = uint32_t(ctx.lr);
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

// Two builds: with the CPU's FMA instruction (std::fma inlined) and without
// (std::fma = a maths-library call); same bits (fma rounds once by
// definition). Picked once (decomp/cpu_features.h), like matrices.cpp.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("fma"), flatten)) bool TestFma(PPCContext& c, uint8_t* b) { return Test(c, b); }
__attribute__((flatten)) bool TestPlain(PPCContext& c, uint8_t* b) { return Test(c, b); }
#else
bool TestFma(PPCContext& c, uint8_t* b) { return Test(c, b); }
bool TestPlain(PPCContext& c, uint8_t* b) { return Test(c, b); }
#endif
bool (*Pick())(PPCContext&, uint8_t*) {
  static bool (*const fn)(PPCContext&, uint8_t*) = [] {
    const bool fma = decomp::CpuHasFma();
    REXLOG_INFO("Frustum test: native C++ ({})", fma ? "the CPU's FMA instruction" : "no FMA instruction: std::fma");
    return fma ? &TestFma : &TestPlain;
  }();
  return fn;
}

decomp::Stats g_stats{"frustum test"};

}  // namespace
}  // namespace decomp_frustum_test

extern "C" REX_FUNC(sub_822ED0C8) {
  using namespace decomp_frustum_test;
  const bool checking = REXCVAR_GET(debug_native_frustum_check);
  decomp::Ranges ranges{};
  if (checking) {
    // The saved registers + back chain below r1; every register.
    ranges = decomp::Ranges{{{ctx.r1.u32 - 128, 128}}, 1};
    ranges.registers = true;
  }
  decomp::Run(REXCVAR_GET(native_frustum_test), checking, g_stats, ctx, base, &__imp__sub_822ED0C8, ranges,
              [&](PPCContext& c) { return Pick()(c, base); });
}
