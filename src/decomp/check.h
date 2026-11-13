// =============================================================================
// decomp/check.h -- the byte-for-byte check shared by the bit-identical
// rewrites of game functions (matrices.cpp; findings/60)
// =============================================================================
// A rewrite names the guest memory its original may write (one or two
// ranges) and runs through Run(): with its "check" switch on, every call
// runs the ORIGINAL on a copy of the registers first (its written bytes and
// registers kept as the answer), then OURS on the same input, compares the
// bytes and r3, logs the first differences per function ("Native check: X
// DIFFERS ...") and a summary every 10 s, and leaves the original's results
// in place. Scratch registers aren't compared: a scan of every call site
// shows no caller reads one after the call (findings/60 s.6).
// =============================================================================
#pragma once

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../guest_memory.h"

namespace decomp {

struct Range {
  uint32_t address, bytes;
  // false = only put back before ours runs, never compared: the guest stack
  // below the caller's frame, whose leftover words a function may read (the
  // original's own run changes them; simple_setup.cpp).
  bool compare = true;
};
// At most six ranges per function; a fixed array (no allocation per call:
// the overrides run ~200,000 times a second).
struct Ranges {
  Range r[6];
  size_t n;
  // true = also compare EVERY register the guest can see (r0-r31, f0-f31,
  // cr0-cr7, xer, lr, ctr), not only r3: for a rewrite that reproduces the
  // scratch registers too because a caller can't be proven not to read them
  // (vertex_streams.cpp).
  bool registers = false;
  size_t size() const { return n; }
  const Range& operator[](size_t i) const { return r[i]; }
  const Range* begin() const { return r; }
  const Range* end() const { return r + n; }
};
struct Stats {
  const char* name;
  uint64_t calls = 0, differed = 0;
  int described = 0;
};
inline std::vector<Stats*>& AllStats() {
  static std::vector<Stats*> all;
  return all;
}
inline void MaybeReport() {
  static std::chrono::steady_clock::time_point last{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  std::string line;
  for (const Stats* s : AllStats()) {
    line += fmt::format("{}{} {} calls / {} differed", line.empty() ? "" : ", ", s->name, s->calls, s->differed);
  }
  REXLOG_INFO("Native check: {}", line);
}


// Every guest-visible register of two contexts: "" when equal, else the
// first difference (Ranges::registers).
inline std::string RegistersDiffer(const PPCContext& a, const PPCContext& b) {
#define DECOMP_REG(name)                                                                              \
  if (a.name.u64 != b.name.u64)                                                                       \
    return fmt::format(#name " original {:X} ours {:X}", a.name.u64, b.name.u64);
  DECOMP_REG(r0) DECOMP_REG(r1) DECOMP_REG(r2) DECOMP_REG(r3) DECOMP_REG(r4) DECOMP_REG(r5) DECOMP_REG(r6)
  DECOMP_REG(r7) DECOMP_REG(r8) DECOMP_REG(r9) DECOMP_REG(r10) DECOMP_REG(r11) DECOMP_REG(r12) DECOMP_REG(r13)
  DECOMP_REG(r14) DECOMP_REG(r15) DECOMP_REG(r16) DECOMP_REG(r17) DECOMP_REG(r18) DECOMP_REG(r19) DECOMP_REG(r20)
  DECOMP_REG(r21) DECOMP_REG(r22) DECOMP_REG(r23) DECOMP_REG(r24) DECOMP_REG(r25) DECOMP_REG(r26) DECOMP_REG(r27)
  DECOMP_REG(r28) DECOMP_REG(r29) DECOMP_REG(r30) DECOMP_REG(r31) DECOMP_REG(ctr)
  DECOMP_REG(f0) DECOMP_REG(f1) DECOMP_REG(f2) DECOMP_REG(f3) DECOMP_REG(f4) DECOMP_REG(f5) DECOMP_REG(f6)
  DECOMP_REG(f7) DECOMP_REG(f8) DECOMP_REG(f9) DECOMP_REG(f10) DECOMP_REG(f11) DECOMP_REG(f12) DECOMP_REG(f13)
  DECOMP_REG(f14) DECOMP_REG(f15) DECOMP_REG(f16) DECOMP_REG(f17) DECOMP_REG(f18) DECOMP_REG(f19) DECOMP_REG(f20)
  DECOMP_REG(f21) DECOMP_REG(f22) DECOMP_REG(f23) DECOMP_REG(f24) DECOMP_REG(f25) DECOMP_REG(f26) DECOMP_REG(f27)
  DECOMP_REG(f28) DECOMP_REG(f29) DECOMP_REG(f30) DECOMP_REG(f31)
#undef DECOMP_REG
  if (a.lr != b.lr) return fmt::format("lr original {:X} ours {:X}", a.lr, b.lr);
  const PPCCRRegister* ca[8] = {&a.cr0, &a.cr1, &a.cr2, &a.cr3, &a.cr4, &a.cr5, &a.cr6, &a.cr7};
  const PPCCRRegister* cb[8] = {&b.cr0, &b.cr1, &b.cr2, &b.cr3, &b.cr4, &b.cr5, &b.cr6, &b.cr7};
  for (int i = 0; i < 8; ++i) {
    if (ca[i]->raw() != cb[i]->raw()) return fmt::format("cr{} original {:X} ours {:X}", i, ca[i]->raw(), cb[i]->raw());
  }
  if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) {
    return fmt::format("xer original {}{}{} ours {}{}{}", a.xer.so, a.xer.ov, a.xer.ca, b.xer.so, b.xer.ov, b.xer.ca);
  }
  return "";
}

// Runs the original on a copy of the registers, keeps what it wrote, runs
// ours on the same input, compares; the original's results stay (memory and
// registers: the answer key wins).
template <class Ours>
void Checked(Stats& st, PPCContext& ctx, uint8_t* base, PPCFunc* original, const Ranges& ranges,
             Ours ours) {
  if (st.calls == 0) AllStats().push_back(&st);
  std::vector<std::vector<uint8_t>> before, theirs;
  for (const Range& r : ranges) {
    const uint8_t* p = GuestPtr(base, r.address);
    before.emplace_back(p, p + r.bytes);
  }
  PPCContext answer = ctx;
  original(answer, base);
  for (const Range& r : ranges) {
    const uint8_t* p = GuestPtr(base, r.address);
    theirs.emplace_back(p, p + r.bytes);
  }
  for (size_t i = 0; i < ranges.size(); ++i) std::memcpy(GuestPtr(base, ranges[i].address), before[i].data(), ranges[i].bytes);
  PPCContext mine = ctx;
  const bool handled = ours(mine);
  ++st.calls;
  bool same = !handled || mine.r3.u64 == answer.r3.u64;
  std::string what = same ? "" : fmt::format("r3 original {:X} ours {:X}", answer.r3.u64, mine.r3.u64);
  if (handled && same && ranges.registers) {
    what = RegistersDiffer(answer, mine);
    same = what.empty();
  }
  for (size_t i = 0; handled && i < ranges.size(); ++i) {
    if (!ranges[i].compare) continue;
    const uint8_t* p = GuestPtr(base, ranges[i].address);
    if (std::memcmp(p, theirs[i].data(), ranges[i].bytes) == 0) continue;
    uint32_t at = 0;
    while (p[at] == theirs[i][at]) ++at;
    at &= ~3u;
    uint32_t o, m;
    std::memcpy(&o, &theirs[i][at], 4);
    std::memcpy(&m, p + at, 4);
    if (what.empty()) {
      what = fmt::format("bytes at {:08X} (+{} of range {}): original {:08X} ours {:08X}", ranges[i].address + at, at,
                         i, __builtin_bswap32(o), __builtin_bswap32(m));
    }
    same = false;
  }
  if (!same) {
    ++st.differed;
    if (st.described < 8) {
      ++st.described;
      REXLOG_WARN("Native check: {} DIFFERS (r3 {:08X} r4 {:08X} r5 {:08X}): {}", st.name, ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, what);
    }
  }
  for (size_t i = 0; i < ranges.size(); ++i) std::memcpy(GuestPtr(base, ranges[i].address), theirs[i].data(), ranges[i].bytes);
  ctx = answer;
  MaybeReport();
}

// One override: the original (switch off), the check, or ours (false from
// ours = not handled: the original runs).
template <class Ours>
void Run(bool enabled, bool checking, Stats& st, PPCContext& ctx, uint8_t* base, PPCFunc* original,
         const Ranges& ranges, Ours ours) {
  if (!enabled) {
    original(ctx, base);
  } else if (checking) {
    Checked(st, ctx, base, original, ranges, ours);
  } else {
    ctx.fpscr.disableFlushMode();  // like the originals' first floating-point instruction
    if (!ours(ctx)) original(ctx, base);
  }
}

}  // namespace decomp
