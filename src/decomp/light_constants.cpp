// =============================================================================
// decomp/light_constants.cpp -- xnContext's tint and light writer
// (sub_8242FF88) as C++, bit-identical (docs/findings/60 s.12)
// =============================================================================
//
// WHAT THIS IS
//   Before every draw (BeginPrims sub_82430780, BeginIndexedPrims
//   sub_824307F8, DrawPrimBuffer sub_82430878), PDDI's Xbox back end calls
//   this to put the current TINT and LIGHTS into Microsoft's D3D device as
//   shader constants. The native renderer reads them back from D3D's copy
//   (recorder.cpp: tint, ambient, light0, the point lights of the unlit
//   simple material), so this rewrite writes EXACTLY what the original
//   writes, byte for byte, its stack leftovers included. It was the largest
//   piece of Radical's renderer code still running as the original (0.8% of
//   the main thread uncapped, findings/60 s.9): next step of option 1
//   ("native back end, bit-identical") after the simple material's set-up.
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x8242FF88-0x82430600; the five
//   virtual calls' targets read from the vtables (xnContext 0x82016744,
//   xnExtUnLitColourTint 0x8201672C) and disassembled: all one-to-three-
//   instruction leaves, inlined here. SetPixelShaderConstantB sub_82312B58
//   (a leaf without a frame) inlined too.
//
// WHAT THE ORIGINAL DOES, IN ORDER (r31 = the context; dev = *(r31 +400))
//    1. TINT: the context in global 0x82571B7C, its slot 91 (sub_82430E60)
//       with 263 = the xnExtUnLitColourTint extension (*(ctx +1208)). If
//       there is one and it's enabled (slot 4: byte +12), its colour (slot 3:
//       word +8, stored at r1+116) -> (R, G, B) x 1/128 (constant
//       0x82047118); else (1, 1, 1). VS c100 (dev +3520) = those three + w =
//       a word of its OWN frame it never writes (r1+364: a leftover, copied
//       from the same guest address). u64 dev +0 |= 1<<38.
//    2. Only if the "lights changed" byte r31 +952 is set (then cleared):
//    3. AMBIENT: slot 38 (sub_82432E68) = the word *( *(r31 +144) +996),
//       ARGB -> PS c8 (dev +6144) = (R, G, B, A) / 255. dev +8 |= 1<<61.
//    4. EIGHT DIRECTIONAL LIGHT SLOTS: records at *(r31 +144) + 4 + 124 i,
//       a "changed" byte per slot at r31 +944 + i. Only CHANGED slots are
//       visited (byte cleared), and they are numbered j = 0, 1, ... in the
//       order visited (a slot that didn't change doesn't advance j: the
//       original's way, kept). Record +112 == 0 (directional) and byte +120
//       set: PS bool b<j> = 1, PS c(9 + 2j) = the direction (record +32..+44,
//       through r1+144), PS c(10 + 2j) = the colour word +116 / 255 (R, G, B,
//       A); else b<j> = 0. Dirty bits: dev +8 |= (1<<63) >> (row / 4).
//    5. UP TO FOUR POINT LIGHTS: the same records in order, +112 == 3 and
//       +120 set (no "changed" test): position (+0..+12 -> r1+288 + 16n),
//       colour +116 / 255 (r1+224 + 16n), the four parameters +80, +84,
//       +96, +76 in that order (-> r1+160 + 16n), until four are found.
//       (Same record view as step 4; the code below counts from the
//       record's start, so these are +4 there.) Then for each: PS c(30 + k) = position, c(34 + k) =
//       colour, c(38 + k) = parameters.
//    6. PS c29 (dev +6480) = (n, three leftover words r1+132 / 136 / 140).
//       dev +8 |= 1<<56.
//
// EXACTNESS
//   * byte / 255 and byte / 128: fcfid + frsp (exact) then fmuls (one
//     rounding) = float(byte) * k in C++, contraction off.
//   * Words moved with lfs / stfs go through float -> double -> float as the
//     recompiled code does (a signalling NaN comes out quiet); lwz / stw
//     copies stay raw.
//   * Stack: the original's frame is r1 - 544. Its prologue (r17-r31 and
//     the return address by savegpr 0x8245C0FC, f27-f31 by savefpr
//     0x8245D3E4 from r12 = r1 - 128, the back chain) and every temporary
//     (the 8-byte integer-to-float slots r1+80..+104, the colour and
//     direction copies) are written as it leaves them: later code at that
//     depth may read such leftovers (findings/60 s.10.1).
//   * Registers: r3 as the original leaves it (see `r3` below); every
//     callee-saved register is put back by its epilogue (we don't touch
//     them). Scratch registers aren't reproduced: the three callers don't
//     read one after the call before writing it (checked in xexdis:
//     0x824307C8, 0x82430844, 0x824308B8).
//   * The original runs instead while a frame is being TRACED (the inlined
//     SetPixelShaderConstantB would be missing from the trace) or when a
//     virtual slot holds another function than the one read here.
//
// SWITCHES: --native_light_constants (default on); --debug_native_light_check
//   = original + ours on every call, compared byte for byte (decomp/check.h).
// =============================================================================

#include <cstdint>
#include <cstring>

#include "../players/outfits.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../pddi/trace.h"
#include "check.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

extern "C" REX_FUNC(__imp__sub_8242FF88);

REXCVAR_DEFINE_BOOL(native_light_constants, true, "CrashMoM",
                    "The renderer's tint and light writer (sub_8242FF88: VS c100, PS c8-c41 and b0-b7) runs as our "
                    "C++ (same results); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_light_check, false, "CrashMoM",
                    "Debug: run the original tint and light writer AND ours on every call and compare what they "
                    "write byte for byte (slow; log lines \"Native check:\")");

namespace decomp_lights {
namespace {

// --- the original's layout --------------------------------------------------------
constexpr uint32_t kFrame = 544;
constexpr uint32_t kContextGlobal = 0x82571B7C;  // the context slot 91 is asked on
constexpr uint32_t kOneOver128 = 0x82047118;     // the tint's scale
constexpr uint32_t kOneOver255 = 0x8200154C;     // the lights' scale
constexpr uint32_t kOne = 0x82002650;            // 1.0f (no tint)
constexpr uint32_t kTintExtension = 263;         // slot 91's argument
// Virtual slots (byte offsets in the vtable) and the functions read there.
constexpr uint32_t kExtensionSlot = 364, kExtensionFn = 0x82430E60;  // 263 -> *(ctx +1208)
constexpr uint32_t kAmbientSlot = 152, kAmbientFn = 0x82432E68;      // *(*(ctx +144) +996)
constexpr uint32_t kEnabledSlot = 16, kEnabledFn = 0x82431428;       // lbz r3,12(r3)
constexpr uint32_t kColourSlot = 12, kColourFn = 0x82431430;         // *r3 = *(r4 +8)
// Device bytes it writes: the dirty words +0..+40, VS c100, PS c8-c41, the
// PS bool words +10128.. (b0-b7 = the first word).
constexpr uint32_t kDeviceBytes = 10136;

// --- guest memory -----------------------------------------------------------------
inline uint32_t L(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline void S(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, a), &v, 4);
}
inline uint64_t L64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  return __builtin_bswap64(v);
}
inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
}
inline uint8_t LB(uint8_t* base, uint32_t a) { return *GuestPtr(base, a); }
inline void SB(uint8_t* base, uint32_t a, uint8_t v) { *GuestPtr(base, a) = v; }
inline float LF(uint8_t* base, uint32_t a) {
  const uint32_t v = L(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void SF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  S(base, a, v);
}
inline void Or64(uint8_t* base, uint32_t a, uint64_t mask) { S64(base, a, L64(base, a) | mask); }
// lfs + stfs: float -> double -> float (volatile: keep the round trip).
inline void MoveFloatWord(uint8_t* base, uint32_t to, uint32_t from) {
  volatile double d = LF(base, from);
  SF(base, to, float(d));
}
// A constant row's dirty bit: (1<<63) >> (row / 4) (the original's srad +
// srd pair with a zero first shift).
constexpr uint64_t RowBit(uint32_t row) { return (uint64_t(1) << 63) >> (row >> 2); }

// SetPixelShaderConstantB(dev, reg, &value, 1) (sub_82312B58).
void SetPsBool(uint8_t* base, uint32_t dev, uint32_t reg, uint32_t value) {
  const uint32_t at = dev + 4 * (2532 + (reg >> 5));
  const uint32_t bit = 1u << (reg & 31);
  S(base, at, (L(base, at) & ~bit) | ((value & 1) << (reg & 31)));
  Or64(base, dev + 32, uint64_t(1) << 56);
}

// The ARGB bytes, as the original orders its 8-byte integer temporaries.
inline uint32_t R(uint32_t c) { return (c >> 16) & 0xFF; }
inline uint32_t G(uint32_t c) { return (c >> 8) & 0xFF; }
inline uint32_t B(uint32_t c) { return c & 0xFF; }
inline uint32_t A(uint32_t c) { return c >> 24; }

inline uint32_t VSlot(uint8_t* base, uint32_t object, uint32_t slot) { return L(base, L(base, object) + slot); }

// What the check runs covered (logged with the check's summary): calls that
// rewrote the lights, directional slots set, point lights written.
struct Coverage {
  uint64_t calls = 0, lights_changed = 0, directional = 0, point = 0;
} g_seen;

// --- the function ----------------------------------------------------------------
bool Write(PPCContext& ctx, uint8_t* base) {
  if (pddi::trace::Recording()) return false;
  const uint32_t r31 = ctx.r3.u32;
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t F = r1 - kFrame;
  // The virtual calls' targets first (anything else: the original).
  const uint32_t context = L(base, kContextGlobal);
  if (VSlot(base, context, kExtensionSlot) != kExtensionFn) return false;
  const uint32_t ext = L(base, context + 1208);
  if (ext && (VSlot(base, ext, kEnabledSlot) != kEnabledFn || VSlot(base, ext, kColourSlot) != kColourFn)) {
    return false;
  }
  const bool lights_changed = LB(base, r31 + 952) != 0;
  if (lights_changed && VSlot(base, r31, kAmbientSlot) != kAmbientFn) return false;

  // Prologue: r17-r31 + the return address, f27-f31, the back chain.
  {
    const uint64_t gpr[15] = {ctx.r17.u64, ctx.r18.u64, ctx.r19.u64, ctx.r20.u64, ctx.r21.u64,
                              ctx.r22.u64, ctx.r23.u64, ctx.r24.u64, ctx.r25.u64, ctx.r26.u64,
                              ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
    for (int k = 17; k <= 31; ++k) S64(base, r1 - 8 * (32 - k) - 8, gpr[k - 17]);
    S(base, r1 - 8, uint32_t(ctx.lr));
    const uint64_t fpr[5] = {ctx.f27.u64, ctx.f28.u64, ctx.f29.u64, ctx.f30.u64, ctx.f31.u64};
    for (int k = 27; k <= 31; ++k) S64(base, r1 - 128 - 8 * (32 - k), fpr[k - 27]);
    S(base, F, r1);
  }

  const uint32_t dev = L(base, r31 + 400);
  uint64_t r3;  // what the original leaves in r3, path by path

  // 1. The tint -> VS c100.
  {
    float x = LF(base, kOne), y = x, z = x;
    r3 = ext;
    if (ext) {
      const uint8_t on = LB(base, ext + 12);
      r3 = on;
      if (on) {
        const uint32_t c = L(base, ext + 8);
        S(base, F + 116, c);
        S64(base, F + 80, R(c));  // the last of its three temporaries there (G, B, R)
        r3 = R(c);
        const float k = LF(base, kOneOver128);
        x = float(R(c)) * k;
        y = float(G(c)) * k;
        z = float(B(c)) * k;
      }
    }
    SF(base, dev + 3520, x);
    SF(base, dev + 3524, y);
    SF(base, dev + 3528, z);
    MoveFloatWord(base, dev + 3532, F + 364);
    Or64(base, dev + 0, uint64_t(1) << 38);
  }

  ++g_seen.calls;
  if (lights_changed) {
    ++g_seen.lights_changed;
    SB(base, r31 + 952, 0);
    const float k = LF(base, kOneOver255);
    const uint32_t records = L(base, r31 + 144);

    // 3. Ambient -> PS c8.
    {
      const uint32_t c = L(base, records + 996);
      // Its temporaries: the word at r1+80 (slot 38's answer), then A, R
      // (+96), G, B (+80): B and R stay.
      S64(base, F + 80, B(c));
      S64(base, F + 96, R(c));
      r3 = G(c);
      SF(base, dev + 6148, float(G(c)) * k);
      SF(base, dev + 6144, float(R(c)) * k);
      SF(base, dev + 6156, float(A(c)) * k);
      SF(base, dev + 6152, float(B(c)) * k);
      Or64(base, dev + 8, uint64_t(1) << 61);
    }

    // 4. The changed directional slots -> b<j>, c(9 + 2j), c(10 + 2j).
    uint32_t j = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      if (LB(base, r31 + 944 + i) == 0) continue;
      SB(base, r31 + 944 + i, 0);
      const uint32_t rec = records + 124 * i + 4;
      if (L(base, rec + 112) == 0 && LB(base, rec + 120) != 0) {
        for (uint32_t w = 0; w < 4; ++w) S(base, F + 144 + 4 * w, L(base, rec + 32 + 4 * w));
        const uint32_t c = L(base, rec + 116);
        S64(base, F + 96, A(c));
        S64(base, F + 104, G(c));
        S64(base, F + 88, B(c));
        S(base, F + 120, 1);
        S64(base, F + 80, R(c));
        SetPsBool(base, dev, j, 1);
        ++g_seen.directional;
        const uint32_t dir = dev + 6160 + 32 * j;
        for (uint32_t w = 0; w < 4; ++w) MoveFloatWord(base, dir + 4 * w, F + 144 + 4 * w);
        r3 = L64(base, dev + 8);
        Or64(base, dev + 8, RowBit(9 + 2 * j));
        const uint32_t col = dev + 6176 + 32 * j;
        SF(base, col + 0, float(R(c)) * k);
        SF(base, col + 4, float(G(c)) * k);
        SF(base, col + 8, float(B(c)) * k);
        SF(base, col + 12, float(A(c)) * k);
        Or64(base, dev + 8, RowBit(10 + 2 * j));
      } else {
        S(base, F + 112, 0);
        SetPsBool(base, dev, j, 0);
        r3 = dev;
      }
      ++j;
    }

    // 5. Up to four point lights, gathered on the stack first.
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8 && n < 4; ++i) {
      const uint32_t rec = records + 124 * i;  // = the original's r11 - 124
      if (L(base, rec + 116) != 3 || LB(base, rec + 124) == 0) continue;
      for (uint32_t w = 0; w < 4; ++w) S(base, F + 288 + 16 * n + 4 * w, L(base, rec + 4 + 4 * w));
      const uint32_t c = L(base, rec + 120);
      S64(base, F + 88, G(c));
      S64(base, F + 96, B(c));
      S64(base, F + 80, R(c));
      S64(base, F + 104, A(c));
      SF(base, F + 228 + 16 * n, float(G(c)) * k);
      SF(base, F + 232 + 16 * n, float(B(c)) * k);
      SF(base, F + 236 + 16 * n, float(A(c)) * k);
      SF(base, F + 224 + 16 * n, float(R(c)) * k);
      MoveFloatWord(base, F + 160 + 16 * n, rec + 84);
      MoveFloatWord(base, F + 164 + 16 * n, rec + 88);
      MoveFloatWord(base, F + 168 + 16 * n, rec + 100);
      MoveFloatWord(base, F + 172 + 16 * n, rec + 80);
      r3 = F + 160;
      ++n;
      ++g_seen.point;
    }
    if (n > 0) {
      r3 = uint64_t(6560) - uint64_t(F + 292);  // subfic r3,r10,6560 (64-bit)
      for (uint32_t m = 0; m < n; ++m) {
        for (uint32_t w = 0; w < 4; ++w) MoveFloatWord(base, dev + 6496 + 16 * m + 4 * w, F + 288 + 16 * m + 4 * w);
        Or64(base, dev + 8, RowBit(30 + m));
        for (uint32_t w = 0; w < 4; ++w) MoveFloatWord(base, dev + 6560 + 16 * m + 4 * w, F + 224 + 16 * m + 4 * w);
        Or64(base, dev + 8, RowBit(34 + m));
        for (uint32_t w = 0; w < 4; ++w) MoveFloatWord(base, dev + 6624 + 16 * m + 4 * w, F + 160 + 16 * m + 4 * w);
        Or64(base, dev + 8, RowBit(38 + m));
      }
    }

    // 6. PS c29 = (n, three leftovers).
    S64(base, F + 88, uint64_t(int64_t(int32_t(n))));
    MoveFloatWord(base, dev + 6488, F + 136);
    MoveFloatWord(base, dev + 6492, F + 140);
    MoveFloatWord(base, dev + 6484, F + 132);
    SF(base, dev + 6480, float(n));
    Or64(base, dev + 8, uint64_t(1) << 56);
  }

  ctx.r3.u64 = r3;
  return true;
}

decomp::Stats g_stats{"light constants"};


}  // namespace
}  // namespace decomp_lights

extern "C" REX_FUNC(sub_8242FF88) {
  using namespace decomp_lights;
  const bool checking = REXCVAR_GET(debug_native_light_check);
  decomp::Ranges ranges{};
  if (checking) {
    const uint32_t r31 = ctx.r3.u32;
    // The device's first 10136 bytes, the context's "changed" bytes and the
    // original's frame (all of it compared: we write what it writes).
    ranges = decomp::Ranges{{{L(base, r31 + 400), kDeviceBytes}, {r31 + 944, 9}, {ctx.r1.u32 - kFrame, kFrame}}, 3};
  }
  // The level-up screen's model in its player's light (players/outfits.h):
  // the records changed around this write only.
  outfits::BeforeLightWrite(ctx.r3.u32);
  decomp::Run(REXCVAR_GET(native_light_constants), checking, g_stats, ctx, base, &__imp__sub_8242FF88, ranges,
              [&](PPCContext& c) { return Write(c, base); });
  outfits::AfterLightWrite();
  if (checking && (g_seen.calls & 0x1FFFF) == 0 && g_seen.calls) {
    REXLOG_INFO("Native check: light constants covered {} calls, {} with lights changed, {} directional, {} point",
                g_seen.calls, g_seen.lights_changed, g_seen.directional, g_seen.point);
  }
}
