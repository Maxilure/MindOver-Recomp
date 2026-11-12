// =============================================================================
// decomp/matrices.cpp -- Radical's matrix helpers and xnContext::
// UpdateTransforms rewritten in C++, bit-identical (findings/60, 2026-11-12)
// =============================================================================
//
// WHAT THIS IS
//   The first piece of the NATIVE PDDI BACK END: the code that, before every
//   draw whose matrices changed, works out the matrices the shaders get and
//   stores them in Direct3D's copy of the shader constants (the "device
//   copy", findings/09), where the native renderer's recorder reads them.
//   Measured (findings/60): UpdateTransforms is ~4% of the game's main thread
//   uncapped, two of its matrix helpers ~3.3% (they are also called by game
//   logic). The recompiled versions keep every PowerPC register in memory and
//   byte-swap every load and store; these do the same arithmetic in host
//   registers.
//
// THE FUNCTIONS (guest addresses; "matrix" = 16 big-endian floats, row-major,
// row vectors: a point is p x M, translation in row 3)
//   sub_82424358 Mul(out r3, a r4, b r5)      out = a x b (4x4), through a
//                temporary: out may be a or b
//   sub_824241D0 MulAffine(out r3, a r4, b r5) out = a x b for affine matrices
//                (column 3 taken as 0 0 0 1: out's column 3 = 0 0 0 1)
//   sub_82424618 Invert(m r3)                 in place: general 3x4 inverse,
//                or the rigid one below when the 3x3's determinant is within
//                1e-5 of 1
//   sub_82423BB8 InvertRigid(m r3)            in place: transpose the 3x3,
//                translation = -(t x R^T)
//   sub_82423C78 Orthonormalise(m r3)         rows 0-2: normalise row 0,
//                row 2 = row 0 x row 1 normalised, row 1 = row 2 x row 0
//   sub_82424880 Normalise3(v r3)             v / |v| (no zero check)
//   sub_82431730 xnContext::UpdateTransforms(context r3), below (a PDDI
//                intercept handler: the intercept layer already wraps it)
//
// HOW THEY STAY EXACT (the same rules as decomp/skinning.cpp)
//   * Each body was produced from the disassembly instruction by instruction
//     (a translator script: lfs/stfs -> loads/stores in the SAME ORDER, every
//     float register a local of the same name, f/r numbers kept), so the
//     operations, their order and their operands are the original's.
//   * fmuls / fadds / fsubs / fdivs / fsqrts = the float operation (the
//     recompiled code does them in double and rounds to float: the same
//     result for float inputs); fmadds a,c,b = float(fma(double a, double c,
//     double b)) like the recompiled code; fmsubs / fnmadds likewise; fneg =
//     sign bit flip.
//   * Stack temporaries are host arrays: nothing outside a function reads
//     them (the one exception, VS c8.w in UpdateTransforms, is read from the
//     guest stack where the original reads it).
//   * Register results: r3 as the original leaves it; the floating-point
//     scratch registers are NOT reproduced: no caller reads one after the
//     call (checked over every call site, findings/60 s.6).
//   * No fusing by the compiler (fp contract off), the FPU's flush-to-zero
//     mode set like the original's.
//   CHECKED by --debug_native_matrix_check: every call runs the original AND
//   ours on the same input, compares the bytes it may write and r3, keeps the
//   original's results (log "Native check:").
//
// --native_matrices=false = the recompiled originals.
// =============================================================================

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "../guest_memory.h"
#include "../pddi/intercept.h"
#include "check.h"
#include "cpu_features.h"
#include "matrices.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

extern "C" REX_FUNC(__imp__sub_82424358);
extern "C" REX_FUNC(__imp__sub_824241D0);
extern "C" REX_FUNC(__imp__sub_82424618);
extern "C" REX_FUNC(__imp__sub_82423BB8);
extern "C" REX_FUNC(__imp__sub_82423C78);
extern "C" REX_FUNC(__imp__sub_82424880);

REXCVAR_DEFINE_BOOL(native_matrices, true, "CrashMoM",
                    "Radical's matrix helpers and the renderer's UpdateTransforms run as our C++ (same results, "
                    "faster); false = the recompiled originals");
REXCVAR_DEFINE_BOOL(debug_native_matrix_no_fma, false, "CrashMoM",
                    "Debug: our matrix helpers without the CPU's FMA instruction (the path older CPUs take), to "
                    "check it too");
REXCVAR_DEFINE_BOOL(debug_native_matrix_check, false, "CrashMoM",
                    "Debug: run the original matrix helpers / UpdateTransforms AND ours on every call and compare "
                    "what they write byte for byte (slow; log lines \"Native check:\")");

namespace decomp_matrices {
namespace {

// --- Memory the bodies run on --------------------------------------------------
// The translated bodies are templates over M: GuestMem = guest addresses
// (the overrides), HostMem = byte offsets into a host float area laid out
// like the original's stack frame (inside the native UpdateTransforms).
// K() always reads a constant from the game's data.
struct GuestMem {
  uint8_t* base;
  float L(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, a), 4);
    v = __builtin_bswap32(v);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void S(uint32_t a, float f) const {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    std::memcpy(GuestPtr(base, a), &v, 4);
  }
  float K(uint32_t a) const { return L(a); }
};
struct HostMem {
  uint8_t* base;
  float* area;  // area[offset / 4]
  float L(uint32_t a) const { return area[a / 4]; }
  void S(uint32_t a, float f) const { area[a / 4] = f; }
  float K(uint32_t a) const { return GuestMem{base}.L(a); }
};

// always_inline everywhere: each body must land INSIDE the FMA / plain copies
// below (clang's flatten doesn't inline a call two levels down).
#define NM_INLINE __attribute__((always_inline)) inline
NM_INLINE float Madd(float a, float c, float b) { return float(std::fma(double(a), double(c), double(b))); }
NM_INLINE float Msub(float a, float c, float b) { return float(std::fma(double(a), double(c), -double(b))); }
NM_INLINE float NMadd(float a, float c, float b) { return float(-std::fma(double(a), double(c), double(b))); }
NM_INLINE float Bits(float a, uint32_t keep, uint32_t flip) {
  uint32_t v;
  std::memcpy(&v, &a, 4);
  v = (v & keep) ^ flip;
  std::memcpy(&a, &v, 4);
  return a;
}
NM_INLINE float Neg(float a) { return Bits(a, ~0u, 0x80000000u); }   // fneg
NM_INLINE float Abs(float a) { return Bits(a, 0x7FFFFFFFu, 0); }     // stfs + clrlwi 1 + lfs
NM_INLINE float Sqrt(float a) { return float(std::sqrt(double(a))); }

constexpr uint32_t kOne = 0x82002650;   // 1.0f in the game's data
constexpr uint32_t kZero = 0x8200074C;  // 0.0f
constexpr uint32_t kRigidEpsilon = 0x82015D58;  // 1e-5f

// The float locals every translated body uses (named like the registers).
#define NM_FLOATS                                                                                   \
  [[maybe_unused]] float f0 = 0, f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f7 = 0, f8 = 0,       \
                         f9 = 0, f10 = 0, f11 = 0, f12 = 0, f13 = 0, f20 = 0, f21 = 0, f22 = 0,        \
                         f23 = 0, f24 = 0, f25 = 0, f26 = 0, f27 = 0, f28 = 0, f29 = 0, f30 = 0, f31 = 0

// --- sub_82424358: out = a x b ------------------------------------------------
// Per element (the original's loops run rows 3..0, columns 3..0, into a stack
// temporary, then memcpy it to out): out[i][j] =
//   fma(b[3][j], a[i][3], fma(a[i][2], b[2][j], fma(b[0][j], a[i][0], b[1][j] * a[i][1])))
template <class M>
NM_INLINE void Mul(const M& m, uint32_t out, uint32_t a, uint32_t b) {
  float t[16];
  for (int i = 3; i >= 0; --i) {
    const uint32_t row = a + 16 * uint32_t(i);
    const float a0 = m.L(row), a1 = m.L(row + 4), a2 = m.L(row + 8), a3 = m.L(row + 12);
    for (int j = 3; j >= 0; --j) {
      const uint32_t col = b + 4 * uint32_t(j);
      const float p = m.L(col + 16) * a1;
      const float q = Madd(m.L(col), a0, p);
      const float r = Madd(a2, m.L(col + 32), q);
      t[4 * i + j] = Madd(m.L(col + 48), a3, r);
    }
  }
  for (uint32_t k = 0; k < 16; ++k) m.S(out + 4 * k, t[k]);
}

// --- sub_824241D0: out = a x b, affine ------------------------------------------
template <class M>
NM_INLINE void MulAffine(const M& m, uint32_t r3, uint32_t r4, uint32_t r5) {
  NM_FLOATS;
  float t[64];
    f10 = m.L(r5 + 24);                            // lfs f10,24(r5)
    f8 = m.L(r4 + 4);                              // lfs f8,4(r4)
    f0 = m.L(r5 + 16);                             // lfs f0,16(r5)
    f1 = f10 * f8;                               // fmuls f1,f10,f8
    f13 = m.L(r4 + 52);                            // lfs f13,52(r4)
    f5 = f0 * f8;                                // fmuls f5,f0,f8
    f11 = m.L(r5 + 20);                            // lfs f11,20(r5)
    f12 = f13 * f0;                              // fmuls f12,f13,f0
    f9 = f13 * f11;                              // fmuls f9,f13,f11
    f6 = m.L(r4 + 20);                             // lfs f6,20(r4)
    f3 = f11 * f8;                               // fmuls f3,f11,f8
    f4 = m.L(r4 + 36);                             // lfs f4,36(r4)
    f29 = m.L(r5 + 8);                             // lfs f29,8(r5)
    f7 = f13 * f10;                              // fmuls f7,f13,f10
    f28 = m.L(r4 + 0);                             // lfs f28,0(r4)
    f13 = f6 * f0;                               // fmuls f13,f6,f0
    f2 = m.L(r5 + 0);                              // lfs f2,0(r5)
    f31 = f6 * f11;                              // fmuls f31,f6,f11
    f8 = m.L(r4 + 48);                             // lfs f8,48(r4)
    f0 = f4 * f0;                                // fmuls f0,f4,f0
    f30 = m.L(r5 + 4);                             // lfs f30,4(r5)
    f6 = f6 * f10;                               // fmuls f6,f6,f10
    f1 = Madd(f29, f28, f1);                     // fmadds f1,f29,f28,f1
    f27 = m.L(r4 + 16);                            // lfs f27,16(r4)
    f11 = f4 * f11;                              // fmuls f11,f4,f11
    f22 = m.L(r4 + 8);                             // lfs f22,8(r4)
    f12 = Madd(f8, f2, f12);                     // fmadds f12,f8,f2,f12
    f26 = m.L(r5 + 32);                            // lfs f26,32(r5)
    f10 = f4 * f10;                              // fmuls f10,f4,f10
    f4 = m.L(r4 + 32);                             // lfs f4,32(r4)
    f9 = Madd(f8, f30, f9);                      // fmadds f9,f8,f30,f9
    f25 = m.L(r4 + 56);                            // lfs f25,56(r4)
    f5 = Madd(f2, f28, f5);                      // fmadds f5,f2,f28,f5
    f24 = m.L(r5 + 36);                            // lfs f24,36(r5)
    f3 = Madd(f30, f28, f3);                     // fmadds f3,f30,f28,f3
    f28 = m.L(r5 + 40);                            // lfs f28,40(r5)
    f23 = Madd(f27, f2, f13);                    // fmadds f23,f27,f2,f13
    f21 = m.L(r4 + 24);                            // lfs f21,24(r4)
    f31 = Madd(f27, f30, f31);                   // fmadds f31,f27,f30,f31
    f13 = m.K(0x82002650);                         // lfs f13,9808(r10)
    f2 = Madd(f4, f2, f0);                       // fmadds f2,f4,f2,f0
    f0 = m.K(0x8200074C);                          // lfs f0,1868(r11)
    f1 = Madd(f28, f22, f1);                     // fmadds f1,f28,f22,f1
    t[22] = f1;                                  // stfs f1,88(r1)
    f6 = Madd(f27, f29, f6);                     // fmadds f6,f27,f29,f6
    f27 = m.L(r4 + 40);                            // lfs f27,40(r4)
    f11 = Madd(f4, f30, f11);                    // fmadds f11,f4,f30,f11
    t[35] = f13;                                 // stfs f13,140(r1)
    f10 = Madd(f4, f29, f10);                    // fmadds f10,f4,f29,f10
    f20 = m.L(r5 + 48);                            // lfs f20,48(r5)
    f12 = Madd(f25, f26, f12);                   // fmadds f12,f25,f26,f12
    t[23] = f0;                                  // stfs f0,92(r1)
    f8 = Madd(f8, f29, f7);                      // fmadds f8,f8,f29,f7
    t[27] = f0;                                  // stfs f0,108(r1)
    f1 = Madd(f25, f24, f9);                     // fmadds f1,f25,f24,f9
    t[31] = f0;                                  // stfs f0,124(r1)
    f5 = Madd(f22, f26, f5);                     // fmadds f5,f22,f26,f5
    t[20] = f5;                                  // stfs f5,80(r1)
    f3 = Madd(f24, f22, f3);                     // fmadds f3,f24,f22,f3
    f4 = m.L(r5 + 52);                             // lfs f4,52(r5)
    f13 = Madd(f21, f24, f31);                   // fmadds f13,f21,f24,f31
    t[21] = f3;                                  // stfs f3,84(r1)
    f0 = Madd(f21, f26, f23);                    // fmadds f0,f21,f26,f23
    t[25] = f13;                                 // stfs f13,100(r1)
    f5 = Madd(f27, f26, f2);                     // fmadds f5,f27,f26,f2
    f7 = m.L(r5 + 56);                             // lfs f7,56(r5)
    t[24] = f0;                                  // stfs f0,96(r1)
    f6 = Madd(f21, f28, f6);                     // fmadds f6,f21,f28,f6
    f3 = Madd(f27, f24, f11);                    // fmadds f3,f27,f24,f11
    t[26] = f6;                                  // stfs f6,104(r1)
    f2 = Madd(f27, f28, f10);                    // fmadds f2,f27,f28,f10
    t[28] = f5;                                  // stfs f5,112(r1)
    f13 = f12 + f20;                             // fadds f13,f12,f20
    t[29] = f3;                                  // stfs f3,116(r1)
    t[30] = f2;                                  // stfs f2,120(r1)
    f0 = Madd(f25, f28, f8);                     // fmadds f0,f25,f28,f8
    f12 = f1 + f4;                               // fadds f12,f1,f4
    f11 = f0 + f7;                               // fadds f11,f0,f7
    t[32] = f13;                                 // stfs f13,128(r1)
    t[33] = f12;                                 // stfs f12,132(r1)
    t[34] = f11;                                 // stfs f11,136(r1)
  for (uint32_t k = 0; k < 16; ++k) m.S(r3 + 4 * k, t[20 + k]);  // memcpy(r3, r1 + 80, 64)
}

// --- sub_82423BB8: in place, rigid inverse --------------------------------------
template <class M>
NM_INLINE void InvertRigid(const M& m, uint32_t r3) {
  NM_FLOATS;
    f12 = m.L(r3 + 8);                             // lfs f12,8(r3)
    f11 = m.L(r3 + 32);                            // lfs f11,32(r3)
    m.S(r3 + 8, f11);                              // stfs f11,8(r3)
    f6 = m.L(r3 + 56);                             // lfs f6,56(r3)
    f4 = Neg(f6);                                // fneg f4,f6
    f7 = m.L(r3 + 48);                             // lfs f7,48(r3)
    f5 = Neg(f7);                                // fneg f5,f7
    f3 = m.L(r3 + 52);                             // lfs f3,52(r3)
    f6 = m.L(r3 + 16);                             // lfs f6,16(r3)
    f2 = Neg(f3);                                // fneg f2,f3
    f7 = m.L(r3 + 4);                              // lfs f7,4(r3)
    f3 = f6;                                     // fmr f3,f6
    m.S(r3 + 4, f6);                               // stfs f6,4(r3)
    f10 = m.L(r3 + 24);                            // lfs f10,24(r3)
    m.S(r3 + 32, f12);                             // stfs f12,32(r3)
    f9 = m.L(r3 + 36);                             // lfs f9,36(r3)
    m.S(r3 + 36, f10);                             // stfs f10,36(r3)
    f8 = f9;                                     // fmr f8,f9
    f1 = m.L(r3 + 0);                              // lfs f1,0(r3)
    f6 = f12 * f4;                               // fmuls f6,f12,f4
    m.S(r3 + 24, f9);                              // stfs f9,24(r3)
    f31 = f10 * f4;                              // fmuls f31,f10,f4
    f0 = m.K(0x8200074C);                          // lfs f0,1868(r11)
    f11 = m.L(r3 + 20);                            // lfs f11,20(r3)
    f9 = m.L(r3 + 40);                             // lfs f9,40(r3)
    m.S(r3 + 12, f0);                              // stfs f0,12(r3)
    m.S(r3 + 28, f0);                              // stfs f0,28(r3)
    m.S(r3 + 44, f0);                              // stfs f0,44(r3)
    f13 = m.K(0x82002650);                         // lfs f13,9808(r10)
    f12 = m.L(r3 + 8);                             // lfs f12,8(r3)
    f10 = f12 * f5;                              // fmuls f10,f12,f5
    m.S(r3 + 60, f13);                             // stfs f13,60(r3)
    f6 = Madd(f1, f5, f6);                       // fmadds f6,f1,f5,f6
    m.S(r3 + 16, f7);                              // stfs f7,16(r3)
    f1 = Madd(f11, f2, f31);                     // fmadds f1,f11,f2,f31
    f0 = Madd(f9, f4, f10);                      // fmadds f0,f9,f4,f10
    f13 = Madd(f7, f2, f6);                      // fmadds f13,f7,f2,f6
    m.S(r3 + 48, f13);                             // stfs f13,48(r3)
    f12 = Madd(f5, f3, f1);                      // fmadds f12,f5,f3,f1
    m.S(r3 + 52, f12);                             // stfs f12,52(r3)
    f11 = Madd(f8, f2, f0);                      // fmadds f11,f8,f2,f0
    m.S(r3 + 56, f11);                             // stfs f11,56(r3)
}

// --- sub_82424618: in place, inverse ---------------------------------------------
template <class M>
NM_INLINE void Invert(const M& m, uint32_t r31) {
  NM_FLOATS;
  float t[64];
    f11 = m.L(r31 + 32);                           // lfs f11,32(r31)
    f10 = m.L(r31 + 24);                           // lfs f10,24(r31)
    f5 = f10 * f11;                              // fmuls f5,f10,f11
    f9 = m.L(r31 + 40);                            // lfs f9,40(r31)
    f8 = m.L(r31 + 20);                            // lfs f8,20(r31)
    f0 = m.L(r31 + 4);                             // lfs f0,4(r31)
    f4 = f8 * f9;                                // fmuls f4,f8,f9
    f3 = f11 * f8;                               // fmuls f3,f11,f8
    f13 = m.L(r31 + 0);                            // lfs f13,0(r31)
    f7 = m.L(r31 + 36);                            // lfs f7,36(r31)
    f6 = m.L(r31 + 16);                            // lfs f6,16(r31)
    f2 = f7 * f10;                               // fmuls f2,f7,f10
    f12 = m.L(r31 + 8);                            // lfs f12,8(r31)
    f1 = f6 * f7;                                // fmuls f1,f6,f7
    f30 = f6 * f9;                               // fmuls f30,f6,f9
    f31 = m.K(0x82002650);                         // lfs f31,9808(r11)
    f28 = m.K(0x82015D58);                         // lfs f28,23896(r10)
    f29 = f0 * f5;                               // fmuls f29,f0,f5
    f27 = f3 * f12;                              // fmuls f27,f3,f12
    f26 = f2 * f13;                              // fmuls f26,f2,f13
    f25 = f30 * f0;                              // fmuls f25,f30,f0
    f29 = Madd(f13, f4, f29);                    // fmadds f29,f13,f4,f29
    f27 = Neg(f27);                              // fneg f27,f27
    f26 = Neg(f26);                              // fneg f26,f26
    f25 = Neg(f25);                              // fneg f25,f25
    f29 = Madd(f12, f1, f29);                    // fmadds f29,f12,f1,f29
    f29 = f29 + f27;                             // fadds f29,f29,f27
    f29 = f29 + f25;                             // fadds f29,f29,f25
    f29 = f29 + f26;                             // fadds f29,f29,f26
    f27 = f29 - f31;                             // fsubs f27,f29,f31
  // f27 = |f29 - 1| through the stack; "rigid" when <= 1e-5 (fcmpu + ble: a
  // NaN is not <=, so it takes the general way, like the original).
  f27 = Abs(f27);
  if (f27 <= f28) {
    InvertRigid(m, r31);
    return;
  }
    f28 = f12 * f6;                              // fmuls f28,f12,f6
    f27 = m.L(r31 + 52);                           // lfs f27,52(r31)
    f25 = f12 * f7;                              // fmuls f25,f12,f7
    f26 = m.L(r31 + 48);                           // lfs f26,48(r31)
    f23 = f12 * f11;                             // fmuls f23,f12,f11
    f24 = m.L(r31 + 56);                           // lfs f24,56(r31)
    f6 = f6 * f0;                                // fmuls f6,f6,f0
    f22 = m.L(r31 + 60);                           // lfs f22,60(r31)
    f12 = f12 * f8;                              // fmuls f12,f12,f8
    t[39] = f22;                                 // stfs f22,156(r1)
    f29 = f31 / f29;                             // fdivs f29,f31,f29
    f4 = f4 - f2;                                // fsubs f4,f4,f2
    f5 = f30 - f5;                               // fsubs f5,f30,f5
    f3 = f1 - f3;                                // fsubs f3,f1,f3
    f11 = f0 * f11;                              // fmuls f11,f0,f11
    f2 = Msub(f10, f13, f28);                    // fmsubs f2,f10,f13,f28
    f1 = Msub(f0, f9, f25);                      // fmsubs f1,f0,f9,f25
    f9 = Msub(f13, f9, f23);                     // fmsubs f9,f13,f9,f23
    f8 = Msub(f13, f8, f6);                      // fmsubs f8,f13,f8,f6
    f6 = Msub(f0, f10, f12);                     // fmsubs f6,f0,f10,f12
    f0 = f4 * f29;                               // fmuls f0,f4,f29
    t[24] = f0;                                  // stfs f0,96(r1)
    f5 = f5 * f29;                               // fmuls f5,f5,f29
    f7 = Msub(f7, f13, f11);                     // fmsubs f7,f7,f13,f11
    f4 = f2 * f29;                               // fmuls f4,f2,f29
    f13 = f3 * f29;                              // fmuls f13,f3,f29
    t[32] = f13;                                 // stfs f13,128(r1)
    f11 = f9 * f29;                              // fmuls f11,f9,f29
    t[29] = f11;                                 // stfs f11,116(r1)
    f9 = f8 * f29;                               // fmuls f9,f8,f29
    t[34] = f9;                                  // stfs f9,136(r1)
    f3 = f1 * f29;                               // fmuls f3,f1,f29
    f10 = f6 * f29;                              // fmuls f10,f6,f29
    t[26] = f10;                                 // stfs f10,104(r1)
    f12 = Neg(f5);                               // fneg f12,f5
    t[28] = f12;                                 // stfs f12,112(r1)
    f2 = f7 * f29;                               // fmuls f2,f7,f29
    f8 = Neg(f4);                                // fneg f8,f4
    t[30] = f8;                                  // stfs f8,120(r1)
    f1 = f27 * f11;                              // fmuls f1,f27,f11
    f7 = Neg(f3);                                // fneg f7,f3
    t[25] = f7;                                  // stfs f7,100(r1)
    f12 = f27 * f12;                             // fmuls f12,f27,f12
    f6 = Neg(f2);                                // fneg f6,f2
    t[33] = f6;                                  // stfs f6,132(r1)
    f11 = f27 * f8;                              // fmuls f11,f27,f8
    f8 = Madd(f26, f7, f1);                      // fmadds f8,f26,f7,f1
    f7 = Madd(f26, f0, f12);                     // fmadds f7,f26,f0,f12
    f5 = Madd(f26, f10, f11);                    // fmadds f5,f26,f10,f11
    f4 = NMadd(f24, f6, f8);                     // fnmadds f4,f24,f6,f8
    t[37] = f4;                                  // stfs f4,148(r1)
    f3 = NMadd(f24, f13, f7);                    // fnmadds f3,f24,f13,f7
    t[36] = f3;                                  // stfs f3,144(r1)
    f2 = NMadd(f24, f9, f5);                     // fnmadds f2,f24,f9,f5
    t[38] = f2;                                  // stfs f2,152(r1)
  // memcpy(r31, r1 + 96, 64), then column 3 = 0 0 0 1 (t[27], t[31], t[35]
  // are never written: their copies are overwritten right after).
  for (uint32_t k = 0; k < 16; ++k) {
    if (k == 3 || k == 7 || k == 11 || k == 15) continue;
    m.S(r31 + 4 * k, t[24 + k]);
  }
  m.S(r31 + 60, f31);
  f0 = m.K(kZero);
  m.S(r31 + 12, f0);
  m.S(r31 + 28, f0);
  m.S(r31 + 44, f0);
}

// --- sub_82424880: v / |v| -------------------------------------------------------
template <class M>
NM_INLINE void Normalise3(const M& m, uint32_t r3) {
  NM_FLOATS;
    f13 = m.L(r3 + 4);                             // lfs f13,4(r3)
    f12 = f13 * f13;                             // fmuls f12,f13,f13
    f11 = m.L(r3 + 0);                             // lfs f11,0(r3)
    f10 = m.L(r3 + 8);                             // lfs f10,8(r3)
    f0 = m.K(0x82002650);                          // lfs f0,9808(r11)
    f9 = Madd(f11, f11, f12);                    // fmadds f9,f11,f11,f12
    f8 = Madd(f10, f10, f9);                     // fmadds f8,f10,f10,f9
    f7 = Sqrt(f8);                               // fsqrts f7,f8
    f6 = f0 / f7;                                // fdivs f6,f0,f7
    f5 = f11 * f6;                               // fmuls f5,f11,f6
    m.S(r3 + 0, f5);                               // stfs f5,0(r3)
    f4 = f13 * f6;                               // fmuls f4,f13,f6
    m.S(r3 + 4, f4);                               // stfs f4,4(r3)
    f3 = f10 * f6;                               // fmuls f3,f10,f6
    m.S(r3 + 8, f3);                               // stfs f3,8(r3)
}

// --- sub_82423C78: in place, orthonormalise rows 0-2 -----------------------------
// Its two stack vectors (r1+80, r1+96) live in t[]; Normalise3 runs on them
// through a HostMem over t (same body).
template <class M>
NM_INLINE void Orthonormalise(const M& m, uint32_t r31) {
  NM_FLOATS;
  float t[64];
  const HostMem tm{m.base, t};  // the stack vectors, as memory
    f0 = m.L(r31 + 0);                             // lfs f0,0(r31)
    f13 = m.L(r31 + 4);                            // lfs f13,4(r31)
    f12 = m.L(r31 + 8);                            // lfs f12,8(r31)
    t[20] = f0;                                  // stfs f0,80(r1)
    t[21] = f13;                                 // stfs f13,84(r1)
    t[22] = f12;                                 // stfs f12,88(r1)
    f31 = m.L(r31 + 16);                           // lfs f31,16(r31)
    f30 = m.L(r31 + 20);                           // lfs f30,20(r31)
    f29 = m.L(r31 + 24);                           // lfs f29,24(r31)
  Normalise3(tm, 80);  // bl 0x82424880 (r3 = r1 + 80)
    f0 = t[22];                                  // lfs f0,88(r1)
    f13 = t[20];                                 // lfs f13,80(r1)
    f11 = f30 * f0;                              // fmuls f11,f30,f0
    f12 = t[21];                                 // lfs f12,84(r1)
    f10 = f29 * f13;                             // fmuls f10,f29,f13
    f9 = f12 * f31;                              // fmuls f9,f12,f31
    f8 = Msub(f29, f12, f11);                    // fmsubs f8,f29,f12,f11
    t[24] = f8;                                  // stfs f8,96(r1)
    f7 = Msub(f0, f31, f10);                     // fmsubs f7,f0,f31,f10
    t[25] = f7;                                  // stfs f7,100(r1)
    f6 = Msub(f30, f13, f9);                     // fmsubs f6,f30,f13,f9
    t[26] = f6;                                  // stfs f6,104(r1)
  Normalise3(tm, 96);  // bl 0x82424880 (r3 = r1 + 96)
    f0 = t[26];                                  // lfs f0,104(r1)
    f13 = t[21];                                 // lfs f13,84(r1)
    f12 = t[20];                                 // lfs f12,80(r1)
    f5 = f0 * f13;                               // fmuls f5,f0,f13
    f11 = t[22];                                 // lfs f11,88(r1)
    f10 = t[24];                                 // lfs f10,96(r1)
    f9 = t[25];                                  // lfs f9,100(r1)
    f4 = f11 * f10;                              // fmuls f4,f11,f10
    f3 = f9 * f12;                               // fmuls f3,f9,f12
    m.S(r31 + 4, f13);                             // stfs f13,4(r31)
    m.S(r31 + 0, f12);                             // stfs f12,0(r31)
    m.S(r31 + 8, f11);                             // stfs f11,8(r31)
    f2 = Msub(f9, f11, f5);                      // fmsubs f2,f9,f11,f5
    m.S(r31 + 16, f2);                             // stfs f2,16(r31)
    f1 = Msub(f0, f12, f4);                      // fmsubs f1,f0,f12,f4
    m.S(r31 + 20, f1);                             // stfs f1,20(r31)
    f13 = Msub(f10, f13, f3);                    // fmsubs f13,f10,f13,f3
    m.S(r31 + 24, f13);                            // stfs f13,24(r31)
    m.S(r31 + 32, f10);                            // stfs f10,32(r31)
    m.S(r31 + 36, f9);                             // stfs f9,36(r31)
    m.S(r31 + 40, f0);                             // stfs f0,40(r31)
}

// --- sub_82431730: xnContext::UpdateTransforms -----------------------------------
// r3 = the xnContext. Runs only when its "transforms changed" byte (+0x104C4)
// is set (and clears it). Reads the world matrix stack (+32: an object with
// +8 depth and +12 base; top = base + (depth rotl 6 & ~63)), the view matrix
// (+52), the camera matrix (vtable slot 21 = this + 156, pddiBaseContext::v21
// sub_8243E428), the matrices at +956 and +1020 (the projection side). Writes
// into the D3D device (+400) through SetVertexShaderConstantF (sub_82312998:
// raw bytes to device + 1920 + 16 x register, then the u64 at device+0 |= its
// mask) and directly:
//   VS c0-c3   = (world x camera^-1), translation cleared, orthonormalised   bit 63
//   VS c4-c7   = world x (+956): the MVP the shaders use                     bit 62
//   VS c12-c15 = world x camera^-1                                           bit 60
//   VS c16-c19 = view x camera x (+1020)                                     bit 59
//   VS c8      = camera row 3 xyz (before inverting); w = a word of the
//                original's own stack frame it never writes (left over from
//                earlier calls): read from the same guest address            bit 61
//   VS c9      = camera^-1 row 3                                             bit 61
//   PS c128    = camera^-1 row 3, PS c129 = camera^-1 row 2: u64 at device+8 |= 0x80000000
// The host area below mirrors the original's stack frame (offsets = its
// r1 offsets); +608 / +672 hold copies of the +956 / +1020 matrices (the
// original multiplies them straight from the context).
constexpr uint32_t kChangedByte = 0x104C4;
constexpr uint32_t kCameraSlotFn = 0x8243E428;  // vtable slot 21 we know (returns this + 156)
constexpr uint32_t kFrame = 656;                // the original's stack frame size

inline uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint64_t Load64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  return __builtin_bswap64(v);
}
inline void Store64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
}

// False = not handled (an unknown camera slot): the caller runs the original.
NM_INLINE bool UpdateTransforms(PPCContext& ctx, uint8_t* base) {
  const uint32_t c = ctx.r3.u32;
  uint8_t* changed = GuestPtr(base, c + kChangedByte);
  if (*changed == 0) return true;  // r3 left as it was (the original's early exit)
  if (Load32(base, Load32(base, c) + 84) != kCameraSlotFn) return false;
  *changed = 0;
  const GuestMem g{base};
  float area[184];
  const HostMem h{base, area};
  auto fetch = [&](uint32_t to, uint32_t from) {  // guest matrix -> host area
    for (uint32_t k = 0; k < 16; ++k) area[to / 4 + k] = g.L(from + 4 * k);
  };
  auto copy = [&](uint32_t to, uint32_t from) { std::memcpy(&area[to / 4], &area[from / 4], 64); };
  const uint32_t stack = Load32(base, c + 32);
  const uint32_t depth = Load32(base, stack + 8);
  fetch(144, (((depth << 6) | (depth >> 26)) & 0xFFFFFFC0u) + Load32(base, stack + 12));  // world
  fetch(480, c + 52);    // view
  fetch(608, c + 956);
  fetch(672, c + 1020);
  Mul(h, 416, 144, 608);
  fetch(80, c + 156);    // camera (vtable slot 21)
  const float f31 = area[32], f30 = area[33], f29 = area[34];  // lfs 128/132/136(r1)
  Mul(h, 352, 480, 80);
  Mul(h, 544, 352, 672);
  Invert(h, 80);
  MulAffine(h, 288, 144, 80);
  copy(224, 288);
  Invert(h, 288);
  copy(144, 224);
  const float zero = g.K(kZero);
  area[48] = area[49] = area[50] = zero;  // stfs f0,192/196/200(r1)
  Orthonormalise(h, 144);
  const uint32_t dev = Load32(base, c + 400);
  auto constants = [&](uint32_t reg, uint32_t from, uint32_t rows, uint64_t mask) {
    for (uint32_t k = 0; k < 4 * rows; ++k) g.S(dev + 1920 + 16 * reg + 4 * k, area[from / 4 + k]);
    Store64(base, dev, Load64(base, dev) | mask);
  };
  constants(0, 144, 4, uint64_t(1) << 63);
  constants(4, 416, 4, uint64_t(1) << 62);
  constants(12, 224, 4, uint64_t(1) << 60);
  constants(16, 544, 4, uint64_t(1) << 59);
  const uint32_t r1 = ctx.r1.u32 - kFrame;  // the original's frame
  g.S(dev + 2048, f31);
  g.S(dev + 2052, f30);
  g.S(dev + 2056, f29);
  std::memcpy(GuestPtr(base, dev + 2060), GuestPtr(base, r1 + 220), 4);  // lfs f0,220(r1) -> stfs: raw word
  Store64(base, dev, Load64(base, dev) | (uint64_t(1) << 61));
  for (uint32_t k = 0; k < 4; ++k) g.S(dev + 2064 + 4 * k, area[32 + k]);
  Store64(base, dev, Load64(base, dev) | (uint64_t(1) << 61));
  for (uint32_t k = 0; k < 4; ++k) g.S(dev + 8064 + 4 * k, area[32 + k]);
  uint64_t r3 = Load64(base, dev + 8) | 0x80000000u;
  Store64(base, dev + 8, r3);
  for (uint32_t k = 0; k < 4; ++k) g.S(dev + 8080 + 4 * k, area[28 + k]);
  r3 = Load64(base, dev + 8) | 0x80000000u;  // oris r3,r4,32768 (r3 is the last value written)
  Store64(base, dev + 8, r3);
  ctx.r3.u64 = r3;
  return true;
}

// --- Two builds of the maths: with the CPU's FMA instruction and without ----------
// Every Madd is std::fma: without target("fma") a maths-library call (~36 of
// them per MulAffine). The FMA copy is used when the CPU has it
// (decomp/cpu_features.h); "flatten" puts each body in one function.
#if defined(__clang__) || defined(__GNUC__)
#define NM_FMA __attribute__((target("fma"), flatten))
#define NM_PLAIN __attribute__((flatten))
#else
#define NM_FMA
#define NM_PLAIN
#endif
struct Fns {
  void (*mul)(uint8_t*, uint32_t, uint32_t, uint32_t);
  void (*mul_affine)(uint8_t*, uint32_t, uint32_t, uint32_t);
  void (*invert)(uint8_t*, uint32_t);
  void (*invert_rigid)(uint8_t*, uint32_t);
  void (*orthonormalise)(uint8_t*, uint32_t);
  void (*normalise3)(uint8_t*, uint32_t);
  bool (*update_transforms)(PPCContext&, uint8_t*);
};
#define NM_VARIANT(ATTR, S)                                                                                    \
  ATTR void Mul##S(uint8_t* b, uint32_t o, uint32_t x, uint32_t y) { Mul(GuestMem{b}, o, x, y); }             \
  ATTR void MulAffine##S(uint8_t* b, uint32_t o, uint32_t x, uint32_t y) { MulAffine(GuestMem{b}, o, x, y); } \
  ATTR void Invert##S(uint8_t* b, uint32_t m) { Invert(GuestMem{b}, m); }                                     \
  ATTR void InvertRigid##S(uint8_t* b, uint32_t m) { InvertRigid(GuestMem{b}, m); }                           \
  ATTR void Orthonormalise##S(uint8_t* b, uint32_t m) { Orthonormalise(GuestMem{b}, m); }                     \
  ATTR void Normalise3##S(uint8_t* b, uint32_t v) { Normalise3(GuestMem{b}, v); }                             \
  ATTR bool UpdateTransforms##S(PPCContext& c, uint8_t* b) { return UpdateTransforms(c, b); }                 \
  constexpr Fns kFns##S = {&Mul##S, &MulAffine##S, &Invert##S, &InvertRigid##S, &Orthonormalise##S,             \
                           &Normalise3##S, &UpdateTransforms##S};
NM_VARIANT(NM_FMA, Fma)
NM_VARIANT(NM_PLAIN, Plain)

const Fns& Pick() {
  static const Fns* fns = [] {
    const bool fma = decomp::CpuHasFma() && !REXCVAR_GET(debug_native_matrix_no_fma);
    REXLOG_INFO("Matrices: native C++ ({})", fma ? "the CPU's FMA instruction" : "no FMA instruction: std::fma");
    return fma ? &kFnsFma : &kFnsPlain;
  }();
  return *fns;
}

// --- The check: decomp/check.h --------------------------------------------------
using decomp::Range;
using decomp::Ranges;
using decomp::Stats;
template <class Ours>
void Run(Stats& st, PPCContext& ctx, uint8_t* base, PPCFunc* original, const Ranges& ranges, Ours ours) {
  decomp::Run(REXCVAR_GET(native_matrices), REXCVAR_GET(debug_native_matrix_check), st, ctx, base, original,
              ranges, ours);
}

}  // namespace
}  // namespace decomp_matrices

using namespace decomp_matrices;

extern "C" REX_FUNC(sub_82424358) {
  static Stats st{"Mul"};
  Run(st, ctx, base, &__imp__sub_82424358, Ranges{{{ctx.r3.u32, 64}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().mul(base, c.r3.u32, c.r4.u32, c.r5.u32);
    return true;
  });
}
extern "C" REX_FUNC(sub_824241D0) {
  static Stats st{"MulAffine"};
  Run(st, ctx, base, &__imp__sub_824241D0, Ranges{{{ctx.r3.u32, 64}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().mul_affine(base, c.r3.u32, c.r4.u32, c.r5.u32);
    return true;
  });
}
extern "C" REX_FUNC(sub_82424618) {
  static Stats st{"Invert"};
  Run(st, ctx, base, &__imp__sub_82424618, Ranges{{{ctx.r3.u32, 64}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().invert(base, c.r3.u32);
    return true;
  });
}
extern "C" REX_FUNC(sub_82423BB8) {
  static Stats st{"InvertRigid"};
  Run(st, ctx, base, &__imp__sub_82423BB8, Ranges{{{ctx.r3.u32, 64}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().invert_rigid(base, c.r3.u32);
    return true;
  });
}
extern "C" REX_FUNC(sub_82423C78) {
  static Stats st{"Orthonormalise"};
  Run(st, ctx, base, &__imp__sub_82423C78, Ranges{{{ctx.r3.u32, 64}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().orthonormalise(base, c.r3.u32);
    c.r3.u64 = c.r1.u64 - 160 + 96;  // addi r3,r1,96 in its own frame (left in r3)
    return true;
  });
}
extern "C" REX_FUNC(sub_82424880) {
  static Stats st{"Normalise3"};
  Run(st, ctx, base, &__imp__sub_82424880, Ranges{{{ctx.r3.u32, 12}}, 1}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    Pick().normalise3(base, c.r3.u32);
    return true;
  });
}
// xnContext::UpdateTransforms is already wrapped by the PDDI intercept layer
// (pddi/intercept.h): ours goes in as its replacement handler (Install(),
// called by CrashMomApp at start).
void OnUpdateTransforms(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  static Stats st{"UpdateTransforms"};
  const uint32_t dev = Load32(base, ctx.r3.u32 + 400);
  // What it may write: the changed byte, the device's dirty masks + VS c0-c19
  // + PS c128-c129 (one range from the device's +0 to the end of PS c129).
  Run(st, ctx, base, original, Ranges{{{ctx.r3.u32 + kChangedByte, 1}, {dev, 8096}}, 2}, [&](PPCContext& c) {
    c.fpscr.disableFlushMode();
    return Pick().update_transforms(c, base);
  });
}

namespace decomp_matrices {
void Install() { pddi::SetHandler(pddi::kFn_82431730, &OnUpdateTransforms); }
}  // namespace decomp_matrices
