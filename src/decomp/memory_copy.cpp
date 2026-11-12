// =============================================================================
// decomp/memory_copy.cpp -- the game's memcpy (sub_8245C1A0) as a host copy
// (findings/60, 2026-11-12)
// =============================================================================
//
// WHAT THE ORIGINAL DOES
//   sub_8245C1A0(r3 = destination, r4 = source, r5 = byte count) is the C
//   runtime's memcpy linked into the game: it saves r3, copies FORWARD
//   (bytes until the destination is 8-aligned, then 8-byte words, 32- and
//   128-byte blocks for long runs, the tail in words / bytes; `dcbt` /
//   `dcbtst` cache hints only, no `dcbz` = it never zeroes a cache line
//   outside the range) and returns the destination in r3. Called from
//   everywhere (the renderer copies every matrix through it, findings/60):
//   ~2-3% of the game's main thread uncapped in the recompiled version, which
//   moves one big-endian word at a time through the register file.
//
// WHY A HOST COPY IS EXACT
//   For buffers that DON'T OVERLAP the result of any forward copy is the
//   source bytes at the destination, whatever the block sizes: std::memcpy of
//   the guest bytes (byte order doesn't matter for a copy). OVERLAPPING calls
//   would depend on the original's block order (a forward copy over its own
//   source smears in 8- or 128-byte steps): those run the original. So do
//   ranges that cross 0xE0000000 (on Windows guest memory from there on sits
//   0x1000 further in host memory, src/guest_memory.h) or wrap past 4 GB.
//   The original's other effect, the r3 saved at -8(r1) below the stack
//   pointer, is scratch nobody reads (its own `ld r3,-8(r1)` reads it back).
//
//   CHECKED by --debug_native_memcpy_check: every handled call runs the
//   original too and compares the destination bytes and r3 (log "Memcpy
//   check:"); overlapping calls are counted there.
//
// --native_memcpy=false = the recompiled original.
// =============================================================================

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../guest_memory.h"

extern "C" REX_FUNC(__imp__sub_8245C1A0);

REXCVAR_DEFINE_BOOL(native_memcpy, true, "CrashMoM",
                    "The game's memcpy (sub_8245C1A0) copies with the host's memcpy (same bytes; overlapping "
                    "buffers keep the original); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_memcpy_check, false, "CrashMoM",
                    "Debug: run the game's memcpy AND ours on every call and compare the destination bytes "
                    "(slow; log lines \"Memcpy check:\")");

namespace {

// True when the call can be a plain host copy (see the header).
bool Plain(uint32_t dst, uint32_t src, uint32_t n) {
  if (n == 0) return true;
  const uint64_t d_end = uint64_t(dst) + n, s_end = uint64_t(src) + n;
  if (d_end > 0x100000000ull || s_end > 0x100000000ull) return false;  // wraps past 4 GB
  constexpr uint64_t kShift = 0xE0000000ull;  // where the Windows +0x1000 starts
  if ((dst < kShift && d_end > kShift) || (src < kShift && s_end > kShift)) return false;
  return d_end <= src || s_end <= dst;  // no overlap
}

struct CheckStats {
  uint64_t calls = 0, bytes = 0, overlapping = 0, differed = 0;
  std::chrono::steady_clock::time_point last{};
};

void Checked(PPCContext& ctx, uint8_t* base) {
  static CheckStats st;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (!Plain(dst, src, n)) {
    ++st.overlapping;
    __imp__sub_8245C1A0(ctx, base);
    return;
  }
  // Ours first into a host buffer (nothing written), then the original
  // in place: compare.
  std::vector<uint8_t> ours(GuestPtr(base, src), GuestPtr(base, src) + n);
  PPCContext answer = ctx;
  __imp__sub_8245C1A0(answer, base);
  ++st.calls;
  st.bytes += n;
  if ((n && std::memcmp(GuestPtr(base, dst), ours.data(), n) != 0) || answer.r3.u64 != ctx.r3.u64) {
    if (++st.differed <= 8) {
      REXLOG_WARN("Memcpy check: DIFFERS: dst {:08X} src {:08X} n {} (r3 original {:X} ours {:X})", dst, src, n,
                  answer.r3.u64, ctx.r3.u64);
    }
  }
  ctx = answer;
  const auto now = std::chrono::steady_clock::now();
  if (now - st.last > std::chrono::seconds(10)) {
    st.last = now;
    REXLOG_INFO("Memcpy check: {} calls, {} bytes compared, {} differed; {} overlapping (original)", st.calls,
                st.bytes, st.differed, st.overlapping);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_8245C1A0) {
  if (!REXCVAR_GET(native_memcpy)) {
    __imp__sub_8245C1A0(ctx, base);
    return;
  }
  if (REXCVAR_GET(debug_native_memcpy_check)) {
    Checked(ctx, base);
    return;
  }
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (!Plain(dst, src, n)) {
    __imp__sub_8245C1A0(ctx, base);
    return;
  }
  if (n) std::memcpy(GuestPtr(base, dst), GuestPtr(base, src), n);
  // r3 = the destination, as the original returns it (`ld r3,-8(r1)` of the
  // r3 it saved: the full 64-bit register).
}
