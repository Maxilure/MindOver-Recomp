// =============================================================================
// decomp/cpu_features.h -- does this CPU have the FMA instruction?
// =============================================================================
// The game's code is compiled for plain x86-64 (no FMA instruction), so
// std::fma is a maths-library call there. Our bit-identical rewrites
// (skinning.cpp, matrices.cpp) build a second copy of their maths with
// target("fma") and pick it once with this check (same correctly rounded
// result either way: fma rounds once by definition).
// FMA usable = CPUID.1:ECX bit 12 (FMA) + bit 27 (the OS saves the AVX
// registers) + XCR0 says it does (bits 1-2).
// <cpuid.h> + inline asm with any clang / GCC (also clang-cl on Windows,
// where MSVC's _xgetbv intrinsic would need -mxsave); MSVC's own intrinsics
// only with MSVC itself.
// =============================================================================
#pragma once

#if defined(__clang__) || defined(__GNUC__)
#include <cpuid.h>
#define DECOMP_GNU_CPUID 1
#else
#include <intrin.h>
#define DECOMP_GNU_CPUID 0
#endif

namespace decomp {

inline bool CpuHasFma() {
  unsigned c = 0;
#if DECOMP_GNU_CPUID
  unsigned a = 0, b = 0, d = 0;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
#else
  int r[4];
  __cpuid(r, 1);
  c = unsigned(r[2]);
#endif
  if (!(c & (1u << 12)) || !(c & (1u << 27))) return false;
#if DECOMP_GNU_CPUID
  unsigned lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  const unsigned long long xcr0 = (static_cast<unsigned long long>(hi) << 32) | lo;
#else
  const unsigned long long xcr0 = _xgetbv(0);
#endif
  return (xcr0 & 6) == 6;
}

}  // namespace decomp
