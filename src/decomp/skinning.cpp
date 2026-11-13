// =============================================================================
// decomp/skinning.cpp -- the characters' CPU skinning loop, rewritten in C++
// (sub_823ED688; the first game function replaced for SPEED, 2026-11-09)
// =============================================================================
//
// WHAT THE ORIGINAL DOES
//   Animated characters (Crash, titans, enemies) are skinned on the CPU: every
//   frame, each vertex is moved by the bones it is attached to before the
//   frame's draws stream it (loop 0x823F3130, findings/11). sub_823ED688 is
//   that per-vertex loop for one mesh piece. Called from sub_823F2DC0 and
//   sub_823F2F08 (the variant sub_823ECC00, used when record +88 is set, is
//   not this one). Arguments:
//     r3 = the piece's SKIN RECORD (big-endian guest struct, offsets below)
//     r4 = the bone MATRICES: *(r4 + 8) = array of 64-byte 4x4 float matrices
//     r5 = the PALETTE: *(r5 + 8) = u32 per palette slot -> matrix number
//     r6 = out: the skinned piece's bounding box, min xyz then max xyz
//   Per vertex i (positions etc. are 3 floats = 12 bytes):
//     bone slots  = 4 bytes at *(r3 + 80) + 4 i (palette slots of up to 4 bones)
//     weights     = *(r3 + 84) + 12 i: w0 w1 w2 (w3 = 1 - w0 - w1 - w2); a
//                   null pointer = one bone, weight 1.0
//     position    : *(r3 + 24) in  -> *(r3 + 32) out  (always)
//     normal      : *(r3 + 28) in  -> *(r3 + 36) out  (flags *(r3 + 8) & 0x10)
//     binormal    : *(r3 + 40) in  -> *(r3 + 44) out  (flags & 0x800)
//     tangent     : *(r3 + 48) in  -> *(r3 + 52) out  (flags & 0x1000)
//     vertex count: *(r3 + 12)
//   Position = sum over bones of w_k (p x M_k) (row vector times the matrix,
//   translation in row 3), but a vertex with w0 == 1.0 skips the other three
//   bones. The direction vectors use bone 0 only (3x3, no weights, not
//   normalised). The box starts at (+1e5, -1e5) (constants 0x8200F850 /
//   0x8200F860) and grows by every skinned position.
//
// WHY REWRITE IT (findings/57)
//   A profile of a 4-thread PC's log + our own sampler (console `profile`):
//   on Wumpa Island, uncapped, this one function was 23% of the game's main
//   thread. The recompiled version keeps every PowerPC register in memory and
//   turns each fused multiply-add (fmadds, ~34 per vertex) into a CALL into
//   the maths library: the game is compiled for plain x86-64 (no FMA
//   instruction) with -mcmodel=large, so std::fma can't be inlined.
//
// HOW THIS VERSION STAYS EXACT
//   The result must be BIT-IDENTICAL to the original (it feeds the picture and
//   the bounding boxes the game culls with). So the maths follows the original
//   instruction by instruction, in the same order:
//     fmuls / fadds / fsubs = float * + - (the recompiled code computes them in
//       double and rounds to float: for two floats that is the same result)
//     fmadds a, b, c = float(fma(double a, double b, double c)), exactly as the
//       recompiled code does (one fused double operation, then rounded to float)
//   and inputs are re-read after each output store, like the original, in case
//   an input and an output ever share memory. The CPU's own FMA instruction is
//   used when it has one (same correctly rounded result as the library call),
//   else std::fma. The flush-to-zero mode is set like the original's
//   (fpscr.disableFlushMode()).
//   CHECKED by --debug_native_skin_check: every call runs the original first,
//   keeps its outputs, runs this one, compares every byte (log "Skinning
//   check: ...").
//
// --native_skinning=false = the original (A/B measurements).
// =============================================================================

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "../guest_memory.h"

#include "cpu_features.h"  // decomp::CpuHasFma (the CPUID check)

extern "C" REX_FUNC(__imp__sub_823ED688);  // the recompiled original (fallback + answer key)

REXCVAR_DEFINE_BOOL(native_skinning, true, "CrashMoM",
                    "Characters' skinning (sub_823ED688) runs as our C++ (same results, faster); false = the "
                    "recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_skin_check, false, "CrashMoM",
                    "Debug: run the original skinning AND ours on every call and compare their outputs byte "
                    "for byte (slow; log lines \"Skinning check:\")");
REXCVAR_DEFINE_BOOL(native_skinning_simd, true, "CrashMoM",
                    "Characters' skinning four vertices at once (SIMD, same results) on CPUs with FMA; false = "
                    "one vertex at a time");
REXCVAR_DEFINE_BOOL(debug_native_skin_no_fma, false, "CrashMoM",
                    "Debug: our skinning without the CPU's FMA instruction (the path older CPUs take), "
                    "to check it too");


// No silent fusing: every a * b + c the compiler might turn into one fma would
// round differently from the original. Only Madd() fuses.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace {

// --- The skin record (r3) ------------------------------------------------------
constexpr uint32_t kFlags = 8;
constexpr uint32_t kCount = 12;
constexpr uint32_t kPositionsIn = 24, kPositionsOut = 32;
constexpr uint32_t kNormalsIn = 28, kNormalsOut = 36;
constexpr uint32_t kBinormalsIn = 40, kBinormalsOut = 44;
constexpr uint32_t kTangentsIn = 48, kTangentsOut = 52;
constexpr uint32_t kBoneSlots = 80;  // 4 bytes per vertex
constexpr uint32_t kWeights = 84;    // 3 floats per vertex, or 0
constexpr uint32_t kHasNormals = 0x10, kHasBinormals = 0x800, kHasTangents = 0x1000;
constexpr uint32_t kArray = 8;       // r4 / r5: the array pointer inside the object
constexpr uint32_t kBoxStart = 0x8200F850;  // +1e5 x3 (min's start); max's start = +16: -1e5 x3

// --- Big-endian reads / writes (host pointers into guest memory) -------------
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline float LoadF(const uint8_t* p) {
  const uint32_t v = Load32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// fmadds as the recompiled code computes it (see the header).
#if defined(__GNUC__) || defined(__clang__)
#define SKIN_INLINE __attribute__((always_inline)) inline
#else
#define SKIN_INLINE __forceinline
#endif
SKIN_INLINE float Madd(float a, float b, float c) {
  return float(std::fma(double(a), double(b), double(c)));
}

// A bone matrix: m[k] = the float at +4k (row r, column c = m[4r + c]).
// Read when used (each read is one byte-swapped load: cheap).
struct Matrix {
  const uint8_t* p;
  SKIN_INLINE float operator[](int k) const { return LoadF(p + 4 * k); }
};

// One direction vector (normal / binormal / tangent) times bone 0's 3x3. The
// original spells the three blocks with different register choices, but all
// three compute exactly this (checked instruction by instruction; fma(a, b, c)
// == fma(b, a, c)):
//   x = fma(in.x, m0, fma(in.z, m8,  in.y * m4))
//   y = fma(in.x, m1, fma(in.z, m9,  in.y * m5))
//   z = fma(in.x, m2, fma(in.z, m10, in.y * m6))
// Inputs are re-read after each store, like the original.
SKIN_INLINE void Direction(const Matrix& m, const uint8_t* in, uint8_t* out) {
  for (int k = 0; k < 3; ++k) {
    const float y = LoadF(in + 4), z = LoadF(in + 8), x = LoadF(in + 0);
    StoreF(out + 4 * k, Madd(x, m[k], Madd(z, m[8 + k], y * m[4 + k])));
  }
}

// p x M (translation added last), in the original's order. Returns x y z.
struct Vec3 {
  float x, y, z;
};
SKIN_INLINE Vec3 Transform(const Matrix& m, float px, float py, float pz) {
  const float a = m[5] * py;   // fmuls f2,f12,f4
  const float b = m[8] * pz;   // fmuls f3,f13,f5
  const float c = m[6] * py;   // fmuls f13,f10,f4
  const float y1 = Madd(m[1], px, a);  // fmadds f8
  const float x1 = Madd(m[4], py, b);  // fmadds f9
  const float z1 = Madd(m[2], px, c);  // fmadds f7
  const float y2 = Madd(m[9], pz, y1);
  const float x2 = Madd(px, m[0], x1);
  const float z2 = Madd(m[10], pz, z1);
  return {x2 + m[12], y2 + m[13], z2 + m[14]};
}

// The extra bones' transform (bones 1 and 2 share one pattern, bone 3 has its
// own): products and fusings exactly as the original's blend path.
SKIN_INLINE Vec3 TransformBlendAB(const Matrix& m, float px, float py, float pz) {
  // A: f1 = A8 pz; f2 = A5 py; f3 = A6 py; f1 = A4 py + f1; f2 = A1 px + f2;
  //    f3 = A2 px + f3; f1 = px A0 + f1; f2 = A9 pz + f2; f3 = A10 pz + f3
  float x = m[8] * pz, y = m[5] * py, z = m[6] * py;
  x = Madd(m[4], py, x);
  y = Madd(m[1], px, y);
  z = Madd(m[2], px, z);
  x = Madd(px, m[0], x);
  y = Madd(m[9], pz, y);
  z = Madd(m[10], pz, z);
  return {x + m[12], y + m[13], z + m[14]};
}
SKIN_INLINE Vec3 TransformBlendC(const Matrix& m, float px, float py, float pz) {
  // f4 = C8 pz; f12 = C6 py; f13 = C5 py; f9 = C4 py + f4; f4 = C2 px + f12;
  // f5 = C1 px + f13; f3 = px C0 + f9; f12 = C10 pz + f4; f13 = C9 pz + f5
  const float x0 = m[8] * pz, z0 = m[6] * py, y0 = m[5] * py;
  const float x1 = Madd(m[4], py, x0);
  const float z1 = Madd(m[2], px, z0);
  const float y1 = Madd(m[1], px, y0);
  const float x2 = Madd(px, m[0], x1);
  const float z2 = Madd(m[10], pz, z1);
  const float y2 = Madd(m[9], pz, y1);
  return {x2 + m[12], y2 + m[13], z2 + m[14]};
}

// The record's pointers, read once per call.
struct Io {
  uint32_t count = 0, flags = 0;
  const uint8_t* matrices = nullptr;
  const uint8_t* palette = nullptr;
  const uint8_t* slots = nullptr;
  const uint8_t* weights = nullptr;  // null = one bone, weight 1.0
  const uint8_t* pos_in = nullptr;
  uint8_t* pos_out = nullptr;
  const uint8_t* nrm_in = nullptr;
  uint8_t* nrm_out = nullptr;
  const uint8_t* bin_in = nullptr;
  uint8_t* bin_out = nullptr;
  const uint8_t* tan_in = nullptr;
  uint8_t* tan_out = nullptr;
  // Palette slot -> that bone's matrix (rlwinm ..,6,0,25 = the number x 64).
  SKIN_INLINE Matrix Bone(uint8_t slot) const { return Matrix{matrices + (Load32(palette + 4u * slot) << 6)}; }
};
SKIN_INLINE Io ReadIo(uint8_t* base, uint32_t record, uint32_t matrices_object, uint32_t palette_object) {
  Io io;
  const uint8_t* rec = GuestPtr(base, record);
  io.count = Load32(rec + kCount);
  if (!io.count) return io;
  io.flags = Load32(rec + kFlags);
  io.matrices = GuestPtr(base, Load32(GuestPtr(base, matrices_object + kArray)));
  io.palette = GuestPtr(base, Load32(GuestPtr(base, palette_object + kArray)));
  io.slots = GuestPtr(base, Load32(rec + kBoneSlots));
  const uint32_t weights_address = Load32(rec + kWeights);
  io.weights = weights_address ? GuestPtr(base, weights_address) : nullptr;
  io.pos_in = GuestPtr(base, Load32(rec + kPositionsIn));
  io.pos_out = GuestPtr(base, Load32(rec + kPositionsOut));
  if (io.flags & kHasNormals) {
    io.nrm_in = GuestPtr(base, Load32(rec + kNormalsIn));
    io.nrm_out = GuestPtr(base, Load32(rec + kNormalsOut));
  }
  if (io.flags & kHasBinormals) {
    io.bin_in = GuestPtr(base, Load32(rec + kBinormalsIn));
    io.bin_out = GuestPtr(base, Load32(rec + kBinormalsOut));
  }
  if (io.flags & kHasTangents) {
    io.tan_in = GuestPtr(base, Load32(rec + kTangentsIn));
    io.tan_out = GuestPtr(base, Load32(rec + kTangentsOut));
  }
  return io;
}

// vmaxfp / vminfp (lane = the new position first): SSE max/min semantics,
// so a NaN coordinate keeps the old bound, like the recompiled code. In
// vertex order (with +0 / -0 the order decides which one stays).
SKIN_INLINE void GrowBox(float x, float y, float z, float* min, float* max) {
  const float c3[3] = {x, y, z};
  for (int k = 0; k < 3; ++k) {
    max[k] = c3[k] > max[k] ? c3[k] : max[k];
    min[k] = c3[k] < min[k] ? c3[k] : min[k];
  }
}

// One vertex, the original's way (scalar).
SKIN_INLINE void Vertex(const Io& io, uint32_t i, float* min, float* max) {
  const size_t v = size_t(i) * 12;
  const Matrix m0 = io.Bone(io.slots[4 * size_t(i)]);
  if (io.nrm_in) Direction(m0, io.nrm_in + v, io.nrm_out + v);
  if (io.bin_in) Direction(m0, io.bin_in + v, io.bin_out + v);
  if (io.tan_in) Direction(m0, io.tan_in + v, io.tan_out + v);

  const uint8_t* weights = io.weights;
  const float w0 = weights ? LoadF(weights + v) : 1.0f;  // (constant 0x82002650 = 1.0)
  const uint8_t* p = io.pos_in + v;
  float x, y, z;
  {
    const float py = LoadF(p + 4), pz = LoadF(p + 8), px = LoadF(p + 0);
    const Vec3 t = Transform(m0, px, py, pz);
    x = t.x * w0;
    y = t.y * w0;
    z = t.z * w0;
  }
  if (!(w0 == 1.0f)) {  // fcmpu + beq: NaN takes the blend path too
    const uint8_t* s = io.slots + 4 * size_t(i);
    const float pz = LoadF(p + 8), py = LoadF(p + 4), px = LoadF(p + 0);
    const float w1 = LoadF(weights + v + 4), w2 = LoadF(weights + v + 8);
    const float w12 = w2 + w1;  // fadds f5,f6,f7
    const Vec3 a = TransformBlendAB(io.Bone(s[1]), px, py, pz);
    const Vec3 b = TransformBlendAB(io.Bone(s[2]), px, py, pz);
    // Bone 1 then bone 2 added on (x y z each: fmadds sum = coord w + sum).
    const float xa = Madd(a.x, w1, x), ya = Madd(a.y, w1, y), za = Madd(a.z, w1, z);
    const float xb = Madd(b.x, w2, xa), yb = Madd(b.y, w2, ya), zb = Madd(b.z, w2, za);
    const float w0_again = LoadF(weights + v);  // (re-read, as the original)
    const float w3 = 1.0f - (w12 + w0_again);   // fadds f12,f5,f13; fsubs f7,f11,f12
    const Vec3 c = TransformBlendC(io.Bone(s[3]), px, py, pz);
    x = Madd(c.x, w3, xb);
    z = Madd(c.z, w3, zb);
    y = Madd(c.y, w3, yb);
  }
  GrowBox(x, y, z, min, max);
  uint8_t* o = io.pos_out + v;
  StoreF(o + 0, x);
  StoreF(o + 4, y);
  StoreF(o + 8, z);
}

SKIN_INLINE void StartBox(uint8_t* base, float* min, float* max) {
  const uint8_t* box = GuestPtr(base, kBoxStart);
  for (int k = 0; k < 3; ++k) {
    min[k] = LoadF(box + 4 * k);
    max[k] = LoadF(box + 16 + 4 * k);
  }
}
SKIN_INLINE void StoreBox(uint8_t* base, uint32_t box_out, const float* min, const float* max) {
  uint8_t* box = GuestPtr(base, box_out);
  for (int k = 0; k < 3; ++k) {
    StoreF(box + 4 * k, min[k]);
    StoreF(box + 12 + 4 * k, max[k]);
  }
}

// The whole loop. Inlined into two wrappers below: one compiled for CPUs with
// the FMA instruction (std::fma becomes that instruction), one without.
SKIN_INLINE void Skin(uint8_t* base, uint32_t record, uint32_t matrices_object, uint32_t palette_object,
                      uint32_t box_out) {
  float min[3], max[3];
  StartBox(base, min, max);
  const Io io = ReadIo(base, record, matrices_object, palette_object);
  for (uint32_t i = 0; i < io.count; ++i) Vertex(io, i, min, max);
  StoreBox(base, box_out, min, max);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("fma"))) void SkinWithFma(uint8_t* b, uint32_t r, uint32_t m, uint32_t p, uint32_t o) {
  Skin(b, r, m, p, o);
}
#else
void SkinWithFma(uint8_t* b, uint32_t r, uint32_t m, uint32_t p, uint32_t o) { Skin(b, r, m, p, o); }
#endif
void SkinPlain(uint8_t* b, uint32_t r, uint32_t m, uint32_t p, uint32_t o) { Skin(b, r, m, p, o); }

// --- FOUR VERTICES AT ONCE (2026-11-13, findings/57 s.11) -------------------------
// The same operations as Vertex(), in the same order, on 4 vertices: each in
// a 64-bit lane of a 256-bit register, as a double holding a float value.
// Madd = _mm256_fmadd_pd (per lane exactly std::fma) then the round to float
// (R: cvtpd_ps + cvtps_pd = the recompiled code's float(...)); * + - in
// double then R (for two float values: the same as the float operation,
// double has more than 2 x 24 + 2 bits). Matrix rows are loaded 16 bytes at a
// time, byte-swapped by pshufb, transposed into lanes. The box grows in
// vertex order (scalar GrowBox), and outputs are stored after a group's
// inputs were read: so a call whose outputs overlap any input runs the
// scalar loop (SkinOverlaps).
#if defined(__x86_64__) || defined(_M_X64)
#define SKIN_HAVE_SIMD 1
#if defined(__GNUC__) || defined(__clang__)
#define SKIN_SIMD __attribute__((target("avx,fma,ssse3,sse4.1"), always_inline)) inline
#define SKIN_SIMD_FN __attribute__((target("avx,fma,ssse3,sse4.1")))
#else
#define SKIN_SIMD __forceinline
#define SKIN_SIMD_FN
#endif

SKIN_SIMD __m256d R(__m256d x) { return _mm256_cvtps_pd(_mm256_cvtpd_ps(x)); }
SKIN_SIMD __m256d Fma4(__m256d a, __m256d b, __m256d c) { return R(_mm256_fmadd_pd(a, b, c)); }
SKIN_SIMD __m256d Mul4(__m256d a, __m256d b) { return R(_mm256_mul_pd(a, b)); }
SKIN_SIMD __m256d Add4(__m256d a, __m256d b) { return R(_mm256_add_pd(a, b)); }
SKIN_SIMD __m256d Sub4(__m256d a, __m256d b) { return R(_mm256_sub_pd(a, b)); }

// 16 big-endian floats of 4 matrices -> m[k] = element k of each lane's matrix.
struct Matrix4 {
  __m256d m[16];
};
SKIN_SIMD __m128 Row(const uint8_t* p) {
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), swap));
}
SKIN_SIMD void LoadMatrices(const uint8_t* const p[4], int rows, Matrix4& out) {
  for (int r = 0; r < rows; ++r) {
    __m128 a = Row(p[0] + 16 * r), b = Row(p[1] + 16 * r), c = Row(p[2] + 16 * r), d = Row(p[3] + 16 * r);
    _MM_TRANSPOSE4_PS(a, b, c, d);  // a = column 0 of each lane's row r, ...
    out.m[4 * r + 0] = _mm256_cvtps_pd(a);
    out.m[4 * r + 1] = _mm256_cvtps_pd(b);
    out.m[4 * r + 2] = _mm256_cvtps_pd(c);
    out.m[4 * r + 3] = _mm256_cvtps_pd(d);
  }
}
// Three floats per vertex for 4 vertices, 12 bytes apart = 48 contiguous
// bytes: a = x0 y0 z0 x1, b = y1 z1 x2 y2, c = z2 x3 y3 z3 (byte-swapped by
// pshufb), split into x y z by two blends and a shuffle each; Store3 = the
// same backwards. Bit moves only: the values are untouched.
SKIN_SIMD void Load3(const uint8_t* p, __m256d& x, __m256d& y, __m256d& z) {
  const __m128 a = Row(p), b = Row(p + 16), c = Row(p + 32);
  const __m128 nx = _mm_blend_ps(_mm_blend_ps(a, b, 0x4), c, 0x2);  // a0 c1 b2 a3
  const __m128 ny = _mm_blend_ps(_mm_blend_ps(a, b, 0x9), c, 0x4);  // b0 a1 c2 b3
  const __m128 nz = _mm_blend_ps(_mm_blend_ps(a, b, 0x2), c, 0x9);  // c0 b1 a2 c3
  x = _mm256_cvtps_pd(_mm_shuffle_ps(nx, nx, _MM_SHUFFLE(1, 2, 3, 0)));  // a0 a3 b2 c1
  y = _mm256_cvtps_pd(_mm_shuffle_ps(ny, ny, _MM_SHUFFLE(2, 3, 0, 1)));  // a1 b0 b3 c2
  z = _mm256_cvtps_pd(_mm_shuffle_ps(nz, nz, _MM_SHUFFLE(3, 0, 1, 2)));  // a2 b1 c0 c3
}
SKIN_SIMD void StoreRow(uint8_t* p, __m128 v) {
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_shuffle_epi8(_mm_castps_si128(v), swap));
}
SKIN_SIMD void Store3f(uint8_t* p, __m128 x, __m128 y, __m128 z) {
  const __m128 nx = _mm_shuffle_ps(x, x, _MM_SHUFFLE(1, 2, 3, 0));  // x0 x3 x2 x1
  const __m128 ny = _mm_shuffle_ps(y, y, _MM_SHUFFLE(2, 3, 0, 1));  // y1 y0 y3 y2
  const __m128 nz = _mm_shuffle_ps(z, z, _MM_SHUFFLE(3, 0, 1, 2));  // z2 z1 z0 z3
  StoreRow(p, _mm_blend_ps(_mm_blend_ps(nx, ny, 0x2), nz, 0x4));       // x0 y0 z0 x1
  StoreRow(p + 16, _mm_blend_ps(_mm_blend_ps(ny, nz, 0x2), nx, 0x4));  // y1 z1 x2 y2
  StoreRow(p + 32, _mm_blend_ps(_mm_blend_ps(nz, nx, 0x2), ny, 0x4));  // z2 x3 y3 z3
}
SKIN_SIMD void Store3(uint8_t* p, __m256d x, __m256d y, __m256d z) {
  Store3f(p, _mm256_cvtpd_ps(x), _mm256_cvtpd_ps(y), _mm256_cvtpd_ps(z));
}
// Direction(): x = fma(in.x, m0, fma(in.z, m8, in.y * m4)), y with m1 m9 m5, z m2 m10 m6.
SKIN_SIMD void Direction4(const Matrix4& m, const uint8_t* in, uint8_t* out) {
  __m256d x, y, z;
  Load3(in, x, y, z);
  __m256d o[3];
  for (int k = 0; k < 3; ++k) o[k] = Fma4(x, m.m[k], Fma4(z, m.m[8 + k], Mul4(y, m.m[4 + k])));
  Store3(out, o[0], o[1], o[2]);
}
struct V3 {
  __m256d x, y, z;
};
// Transform(): a = m5 py, b = m8 pz, c = m6 py; y1 = m1 px + a, x1 = m4 py + b,
// z1 = m2 px + c; y2 = m9 pz + y1, x2 = px m0 + x1, z2 = m10 pz + z1; + row 3.
SKIN_SIMD V3 Transform4(const Matrix4& M, __m256d px, __m256d py, __m256d pz) {
  const __m256d* m = M.m;
  const __m256d a = Mul4(m[5], py), b = Mul4(m[8], pz), c = Mul4(m[6], py);
  const __m256d y1 = Fma4(m[1], px, a), x1 = Fma4(m[4], py, b), z1 = Fma4(m[2], px, c);
  const __m256d y2 = Fma4(m[9], pz, y1), x2 = Fma4(px, m[0], x1), z2 = Fma4(m[10], pz, z1);
  return {Add4(x2, m[12]), Add4(y2, m[13]), Add4(z2, m[14])};
}
// TransformBlendAB / TransformBlendC compute the same values (same products,
// same fusing pairs; only the instruction order differs), so one Transform.
SKIN_SIMD V3 TransformBlend4(const Matrix4& M, __m256d px, __m256d py, __m256d pz) {
  const __m256d* m = M.m;
  __m256d x = Mul4(m[8], pz), y = Mul4(m[5], py), z = Mul4(m[6], py);
  x = Fma4(m[4], py, x);
  y = Fma4(m[1], px, y);
  z = Fma4(m[2], px, z);
  x = Fma4(px, m[0], x);
  y = Fma4(m[9], pz, y);
  z = Fma4(m[10], pz, z);
  return {Add4(x, m[12]), Add4(y, m[13]), Add4(z, m[14])};
}

// Do the outputs overlap any input? Then the scalar loop (its re-reads matter).
bool SkinOverlaps(const Io& io) {
  const uint64_t n12 = uint64_t(io.count) * 12;
  struct Span {
    const uint8_t* p;
    uint64_t n;
  };
  const Span outs[4] = {{io.pos_out, n12}, {io.nrm_out, io.nrm_out ? n12 : 0},
                        {io.bin_out, io.bin_out ? n12 : 0}, {io.tan_out, io.tan_out ? n12 : 0}};
  // Inputs: positions, directions, slots, weights; matrices and the palette
  // are bounded by their largest possible index (256 slots, 2^26 bytes of
  // matrices: checked loosely as "within 1 MB after" = never near outputs).
  const Span ins[8] = {{io.pos_in, n12},
                       {io.nrm_in, io.nrm_in ? n12 : 0},
                       {io.bin_in, io.bin_in ? n12 : 0},
                       {io.tan_in, io.tan_in ? n12 : 0},
                       {io.slots, uint64_t(io.count) * 4},
                       {io.weights, io.weights ? n12 : 0},
                       {io.palette, 1024},
                       {io.matrices, 1u << 20}};
  for (const Span& o : outs) {
    if (!o.n) continue;
    for (const Span& i : ins) {
      if (i.n && o.p < i.p + i.n && i.p < o.p + o.n) return true;
    }
  }
  return false;
}

SKIN_SIMD_FN void SkinSimd(uint8_t* base, uint32_t record, uint32_t matrices_object, uint32_t palette_object,
                           uint32_t box_out) {
  float min[3], max[3];
  StartBox(base, min, max);
  const Io io = ReadIo(base, record, matrices_object, palette_object);
  uint32_t i = 0;
  if (io.count >= 4 && !SkinOverlaps(io)) {
    const __m256d one = _mm256_set1_pd(1.0);
    for (; i + 4 <= io.count; i += 4) {
      const size_t v = size_t(i) * 12;
      const uint8_t* s = io.slots + 4 * size_t(i);
      const uint8_t* m0p[4];
      for (int l = 0; l < 4; ++l) m0p[l] = io.Bone(s[4 * l]).p;
      Matrix4 m0;
      LoadMatrices(m0p, 4, m0);
      if (io.nrm_in) Direction4(m0, io.nrm_in + v, io.nrm_out + v);
      if (io.bin_in) Direction4(m0, io.bin_in + v, io.bin_out + v);
      if (io.tan_in) Direction4(m0, io.tan_in + v, io.tan_out + v);

      __m256d px, py, pz;
      Load3(io.pos_in + v, px, py, pz);
      // w0 w1 w2 of the 4 vertices (the same 12-byte layout).
      __m256d w0 = one, w1 = one, w2 = one;
      if (io.weights) Load3(io.weights + v, w0, w1, w2);
      const V3 t = Transform4(m0, px, py, pz);
      __m256d x = Mul4(t.x, w0), y = Mul4(t.y, w0), z = Mul4(t.z, w0);
      // Lanes that blend: !(w0 == 1.0) (NaN blends too).
      const __m256d blend = _mm256_cmp_pd(w0, one, _CMP_NEQ_UQ);
      const int mask = _mm256_movemask_pd(blend);
      if (mask) {
        // Bones 1-3 of the blending lanes (bone 0 stands in for the others:
        // their results are thrown away, and their slots may hold anything).
        const uint8_t* mp[3][4];
        for (int l = 0; l < 4; ++l) {
          for (int k = 0; k < 3; ++k) mp[k][l] = (mask >> l & 1) ? io.Bone(s[4 * l + 1 + k]).p : m0p[l];
        }
        const __m256d w12 = Add4(w2, w1);
        Matrix4 mb;
        LoadMatrices(mp[0], 4, mb);
        const V3 a = TransformBlend4(mb, px, py, pz);
        LoadMatrices(mp[1], 4, mb);
        const V3 b = TransformBlend4(mb, px, py, pz);
        const __m256d xa = Fma4(a.x, w1, x), ya = Fma4(a.y, w1, y), za = Fma4(a.z, w1, z);
        const __m256d xb = Fma4(b.x, w2, xa), yb = Fma4(b.y, w2, ya), zb = Fma4(b.z, w2, za);
        const __m256d w3 = Sub4(one, Add4(w12, w0));
        LoadMatrices(mp[2], 4, mb);
        const V3 c = TransformBlend4(mb, px, py, pz);
        x = _mm256_blendv_pd(x, Fma4(c.x, w3, xb), blend);
        y = _mm256_blendv_pd(y, Fma4(c.y, w3, yb), blend);
        z = _mm256_blendv_pd(z, Fma4(c.z, w3, zb), blend);
      }
      const __m128 xf = _mm256_cvtpd_ps(x), yf = _mm256_cvtpd_ps(y), zf = _mm256_cvtpd_ps(z);
      float fx[4], fy[4], fz[4];
      _mm_storeu_ps(fx, xf);
      _mm_storeu_ps(fy, yf);
      _mm_storeu_ps(fz, zf);
      for (int l = 0; l < 4; ++l) GrowBox(fx[l], fy[l], fz[l], min, max);
      Store3f(io.pos_out + v, xf, yf, zf);
    }
  }
  for (; i < io.count; ++i) Vertex(io, i, min, max);
  StoreBox(base, box_out, min, max);
}
#else
#define SKIN_HAVE_SIMD 0
#endif

using SkinFn = void (*)(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
// One vertex at a time (FMA or plain build), picked once; the four-at-once
// version is used per call while native_skinning_simd is on (live switch).
SkinFn PickSkin() {
  const bool fma = decomp::CpuHasFma() && !REXCVAR_GET(debug_native_skin_no_fma);
  REXLOG_INFO("Skinning: native C++ ({}{})", fma ? "the CPU's FMA instruction" : "no FMA instruction: std::fma",
              SKIN_HAVE_SIMD && fma ? "; four vertices at once while native_skinning_simd is on" : "");
  return fma ? &SkinWithFma : &SkinPlain;
}
// The one for this call. (SSSE3's byte swap comes with every CPU with FMA.)
SkinFn CurrentSkin(SkinFn scalar) {
#if SKIN_HAVE_SIMD
  if (scalar == &SkinWithFma && REXCVAR_GET(native_skinning_simd)) return &SkinSimd;
#endif
  return scalar;
}

// --- The byte-for-byte check (debug_native_skin_check) ------------------------

struct CheckStats {
  uint64_t calls = 0, vertices = 0, mismatched_calls = 0;
  int detailed = 0;  // mismatches described in the log (first few only)
  std::chrono::steady_clock::time_point last_report{};
};

// The output ranges of one call: (guest address, bytes).
std::vector<std::pair<uint32_t, uint32_t>> Outputs(uint8_t* base, uint32_t record, uint32_t box_out) {
  const uint8_t* rec = GuestPtr(base, record);
  const uint32_t count = Load32(rec + kCount), flags = Load32(rec + kFlags);
  std::vector<std::pair<uint32_t, uint32_t>> out;
  out.push_back({box_out, 24});
  if (!count) return out;
  out.push_back({Load32(rec + kPositionsOut), count * 12});
  if (flags & kHasNormals) out.push_back({Load32(rec + kNormalsOut), count * 12});
  if (flags & kHasBinormals) out.push_back({Load32(rec + kBinormalsOut), count * 12});
  if (flags & kHasTangents) out.push_back({Load32(rec + kTangentsOut), count * 12});
  return out;
}

void CheckedCall(PPCContext& ctx, uint8_t* base, SkinFn ours) {
  static CheckStats stats;  // main thread only (the frame's draws)
  const uint32_t record = ctx.r3.u32, matrices = ctx.r4.u32, palette = ctx.r5.u32, box = ctx.r6.u32;
  const auto ranges = Outputs(base, record, box);
  // If an output overlaps an input, running the original first would change
  // the inputs of ours: such calls are only run once (the original) and counted.
  // (Never seen; the check would say so.)
  __imp__sub_823ED688(ctx, base);
  std::vector<std::vector<uint8_t>> original;
  for (const auto& [a, n] : ranges) {
    const uint8_t* p = GuestPtr(base, a);
    original.emplace_back(p, p + n);
  }
  ours(base, record, matrices, palette, box);
  const uint32_t count = Load32(GuestPtr(base, record) + kCount);
  ++stats.calls;
  stats.vertices += count;
  for (size_t r = 0; r < ranges.size(); ++r) {
    const uint8_t* p = GuestPtr(base, ranges[r].first);
    if (std::memcmp(p, original[r].data(), ranges[r].second) == 0) continue;
    ++stats.mismatched_calls;
    if (stats.detailed < 8) {
      ++stats.detailed;
      size_t at = 0;
      while (at < ranges[r].second && p[at] == original[r][at]) ++at;
      const size_t word = at & ~size_t(3);
      REXLOG_WARN("Skinning check: MISMATCH in output {} (0 box, 1 positions, 2+ directions) at byte {} "
                  "(vertex {}), original {:08X} ours {:08X}; record {:08X} count {} flags {:08X}",
                  r, at, r ? word / 12 : 0, Load32(original[r].data() + word), Load32(p + word), record, count,
                  Load32(GuestPtr(base, record) + kFlags));
    }
    // Leave the ORIGINAL's bytes in place (the game keeps the answer key's results).
    std::memcpy(GuestPtr(base, ranges[r].first), original[r].data(), ranges[r].second);
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - stats.last_report > std::chrono::seconds(10)) {
    stats.last_report = now;
    REXLOG_INFO("Skinning check: {} calls, {} vertices compared, {} calls differed", stats.calls, stats.vertices,
                stats.mismatched_calls);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_823ED688) {
  static const SkinFn scalar = PickSkin();
  const SkinFn ours = CurrentSkin(scalar);
  if (!REXCVAR_GET(native_skinning)) {
    __imp__sub_823ED688(ctx, base);
    return;
  }
  if (REXCVAR_GET(debug_native_skin_check)) {
    CheckedCall(ctx, base, ours);
    return;
  }
  // The original leaves the FPU in "no flush to zero" mode (its loop sets it);
  // so do we, so code after this sees the same state.
  ctx.fpscr.disableFlushMode();
  ours(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
