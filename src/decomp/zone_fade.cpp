// =============================================================================
// decomp/zone_fade.cpp -- the per-frame fade of a zone's objects by distance
// (sub_822F4268) as C++, bit-identical (docs/findings/60 s.15)
// =============================================================================
//
// WHAT THIS IS
//   Once a frame, for each loaded zone, sub_822B4B08 walks the zone list
//   (*(*(0x825A....)+100), the zone manager) and calls this with
//       r3 = the zone, f1 = the frame's time step,
//       r4 = a point: (game object at +444) + 216 (x y z; the camera's
//            position, judging by how the fade follows it: name TBD).
//   The zone holds a list of objects (+184 first, +188 one past the last; 4-
//   byte pointers) and, parallel to it, an array of pairs (+200: fade, speed;
//   8 bytes each). For each object:
//       d      = |object +44/+48/+52 - point| - object +56 (its radius)
//       speed  = +|speed| when d < object +84 (its fade range), else -|speed|
//       fade  += speed x dt, then clamped: fade - 1 >= 0 -> 1; fade < 0 -> 0
//   (1 and 0 = the constants at 0x8201EE28 and 0x82046720, read, not assumed).
//   Objects fade in as the point comes near and out as it leaves (the fade
//   +0x170 that zone_lists.cpp's "every character drawn" fix relies on is
//   likely written from this array: name TBD). The largest piece of game code
//   of its own in the Wumpa Island profile after s.14: ~1.45% of the main
//   thread uncapped, all its own instructions.
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x822F4268-0x822F43AC next to the
//   recompiled C++ (generated/default, DEFINE_REX_FUNC(sub_822F4268)); the
//   body below follows those lines one for one: lfs = float widened to
//   double, fsubs / fmuls = float(a op b), fmadds = float(fma(a, b, c)),
//   fsqrts = float(sqrt(x)), stfs = float(x). Every guest load and store
//   happens at the original's moment, in its order (the point is re-read for
//   every object), so overlapping arrays give the original's answer too.
//   The two red-zone words the |speed| code goes through (r1-12, r1-16) are
//   written the same way.
//
// REGISTERS: the caller calls sub_822F4CE0 right after this one, so EVERY
//   register is left as the original leaves it (like vertex_streams.cpp):
//   r7-r11, cr0 / cr6, the carry, f0 and f2-f13 of the last object. r3, r4,
//   f1 and lr are not changed.
//
// SWITCHES: --native_zone_fade (default on); --debug_native_zone_check =
//   original + ours on every call, compared byte for byte (the pair array and
//   the red zone) and register for register (decomp/check.h).
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

extern "C" REX_FUNC(__imp__sub_822F4268);

REXCVAR_DEFINE_BOOL(native_zone_fade, true, "CrashMoM",
                    "The per-frame fade of a zone's objects by distance (sub_822F4268) runs as our C++ (same "
                    "results); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_zone_check, false, "CrashMoM",
                    "Debug: run the original zone fade AND ours on every call and compare memory and every register "
                    "(slow; log lines \"Native check:\")");

namespace decomp_zone_fade {
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
inline float AsFloat(uint32_t w) {
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
inline uint32_t Word(float f) {
  uint32_t w;
  std::memcpy(&w, &f, 4);
  return w;
}
inline uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
inline double FromBits(uint64_t u) {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
// lfs / stfs.
inline double LF(uint8_t* base, uint32_t a) { return double(AsFloat(L(base, a))); }
inline void SF(uint8_t* base, uint32_t a, double d) { S(base, a, Word(float(d))); }

inline bool Fade(PPCContext& ctx, uint8_t* base) {
  const uint32_t zone = ctx.r3.u32;
  const uint32_t point = ctx.r4.u32;
  const uint32_t r1 = ctx.r1.u32;
  const double dt = ctx.f1.f64;

  // Prologue (no frame: a leaf).
  const uint64_t end = L(base, zone + 188);
  const uint64_t begin = L(base, zone + 184);
  uint64_t r7 = end - begin;
  uint64_t r9 = L(base, zone + 200);
  uint64_t r10 = begin;
  ctx.xer.ca = (int32_t(uint32_t(r7)) < 0) & ((uint32_t(r7) & 0x3) != 0);
  int64_t r11 = int32_t(uint32_t(r7)) >> 2;
  // A list whose end is before its begin: the original would walk ~2^32
  // entries; leave that to it (nothing written yet).
  if (r11 < 0) return false;
  ctx.cr0.compare<int32_t>(int32_t(r11), 0, ctx.xer);
  if (ctx.cr0.eq) {  // beqlr
    ctx.r7.u64 = r7;
    ctx.r8.u64 = begin;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = r10;
    ctx.r11.s64 = r11;
    return true;
  }
  uint64_t r8 = r9 + 4;
  const double f11 = LF(base, 0x8201EE28);  // 1.0 (lis r9,-32254 + -4568)
  const double f12 = LF(base, 0x82046720);  // 0.0 (lis r7,-32252 + 26400)

  double f0, f2, f3, f4, f5, f6, f7, f8, f9, f10, f13;
  bool lt = false;
  do {
    const uint32_t obj = L(base, uint32_t(r10));
    f0 = LF(base, point + 4);
    f13 = LF(base, point + 8);
    f10 = LF(base, point + 0);
    f9 = LF(base, obj + 48);
    f8 = double(float(f9 - f0));
    f7 = LF(base, obj + 52);
    f6 = double(float(f7 - f13));
    f5 = LF(base, obj + 44);
    f4 = double(float(f5 - f10));
    f3 = LF(base, obj + 56);
    f2 = LF(base, obj + 84);
    f0 = double(float(f8 * f8));
    f13 = double(float(std::fma(f6, f6, f0)));
    f10 = double(float(std::fma(f4, f4, f13)));
    f9 = double(float(std::sqrt(f10)));
    f8 = double(float(f9 - f3));
    const bool near = f8 < f2;

    const uint32_t speed_at = uint32_t(r8);
    f0 = LF(base, speed_at);
    if (near) {
      // stfs f0,-12(r1); lwz; clrlwi 1; stw -> +|speed|
      S(base, r1 - 12, Word(float(f0)));
      r9 = L(base, r1 - 12);
      r7 = uint32_t(r9) & 0x7FFFFFFFu;
      S(base, speed_at, uint32_t(r7));
    } else {
      // through r1-16, then fneg -> -|speed|
      S(base, r1 - 16, Word(float(f0)));
      r9 = L(base, r1 - 16);
      r7 = uint32_t(r9) & 0x7FFFFFFFu;
      S(base, r1 - 16, uint32_t(r7));
      f13 = LF(base, r1 - 16);
      f10 = FromBits(Bits(f13) ^ 0x8000000000000000ull);
      SF(base, speed_at, f10);
    }

    f0 = LF(base, speed_at - 4);
    f13 = LF(base, speed_at);
    f0 = double(float(std::fma(f13, dt, f0)));
    SF(base, speed_at - 4, f0);
    f10 = double(float(f0 - f11));
    f13 = !(f10 < f12) ? f11 : f0;
    lt = f0 < f12;
    f0 = !lt ? f13 : f12;
    SF(base, speed_at - 4, f0);

    ctx.xer.ca = uint32_t(r11) > 0;
    r11 = r11 - 1;
    ctx.cr0.compare<int32_t>(int32_t(r11), 0, ctx.xer);
    r10 += 4;
    r8 += 8;
  } while (!ctx.cr0.eq);

  // The last object's registers. r9 = the last clamp's answer (clrlwi of
  // "fade >= 0"), cr6 = cmplwi of it.
  r9 = lt ? 0 : 1;
  ctx.cr6.compare<uint32_t>(uint32_t(r9), 0, ctx.xer);
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.r11.s64 = r11;
  ctx.f0.f64 = f0;
  ctx.f2.f64 = f2;
  ctx.f3.f64 = f3;
  ctx.f4.f64 = f4;
  ctx.f5.f64 = f5;
  ctx.f6.f64 = f6;
  ctx.f7.f64 = f7;
  ctx.f8.f64 = f8;
  ctx.f9.f64 = f9;
  ctx.f10.f64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
  return true;
}

// Two builds: with the CPU's FMA and sqrt inlined, and without (std::fma = a
// maths-library call); same bits. Picked once (decomp/cpu_features.h).
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("fma"), flatten)) bool FadeFma(PPCContext& c, uint8_t* b) { return Fade(c, b); }
__attribute__((flatten)) bool FadePlain(PPCContext& c, uint8_t* b) { return Fade(c, b); }
#else
bool FadeFma(PPCContext& c, uint8_t* b) { return Fade(c, b); }
bool FadePlain(PPCContext& c, uint8_t* b) { return Fade(c, b); }
#endif
bool (*Pick())(PPCContext&, uint8_t*) {
  static bool (*const fn)(PPCContext&, uint8_t*) = [] {
    const bool fma = decomp::CpuHasFma();
    REXLOG_INFO("Zone fade: native C++ ({})", fma ? "the CPU's FMA instruction" : "no FMA instruction: std::fma");
    return fma ? &FadeFma : &FadePlain;
  }();
  return fn;
}

decomp::Stats g_stats{"zone fade"};

}  // namespace
}  // namespace decomp_zone_fade

extern "C" REX_FUNC(sub_822F4268) {
  using namespace decomp_zone_fade;
  const bool checking = REXCVAR_GET(debug_native_zone_check);
  decomp::Ranges ranges{};
  if (checking) {
    // The red zone the |speed| code uses, the pair array; every register.
    const uint32_t n = (L(base, ctx.r3.u32 + 188) - L(base, ctx.r3.u32 + 184)) >> 2;
    ranges = decomp::Ranges{{{ctx.r1.u32 - 16, 8}}, 1};
    if (n && n < 65536) {
      ranges.r[1] = {L(base, ctx.r3.u32 + 200), 8 * n};
      ranges.n = 2;
    }
    ranges.registers = true;
  }
  decomp::Run(REXCVAR_GET(native_zone_fade), checking, g_stats, ctx, base, &__imp__sub_822F4268, ranges,
              [&](PPCContext& c) { return Pick()(c, base); });
}
