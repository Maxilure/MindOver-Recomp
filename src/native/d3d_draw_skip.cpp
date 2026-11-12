// =============================================================================
// native/d3d_draw_skip.cpp -- see d3d_draw_skip.h
// =============================================================================
#include "d3d_draw_skip.h"

#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../pddi/intercept.h"

REXCVAR_DEFINE_BOOL(d3d_draw_skip, true, "CrashMoM",
                    "Native Only: the game's Xbox Direct3D draw calls (command buffer building) are "
                    "skipped while the emulated GPU draws nothing; false = they run (comparisons)");

// D3D's allocator for GPU-read data (vertices / indices of immediate draws):
// r3 device, r4 size in 32-bit words, r5 alignment 16 -> guest address or 0.
extern "C" REX_FUNC(sub_82308B60);

namespace d3d_draw_skip {
namespace {

std::atomic<bool> g_emulated_idle{false};
// The last BeginVertices / BeginIndexedVertices was skipped: its EndVertices
// must be too (and only then). Main thread only (the game's renderer).
bool g_begin_skipped = false;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// D3D's allocator, called like the original Begin* does; registers put back.
uint32_t Allocate(PPCContext& ctx, uint8_t* base, uint32_t device, uint32_t dwords) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = device;
  ctx.r4.u64 = dwords;
  ctx.r5.u64 = 16;
  sub_82308B60(ctx, base);
  const uint32_t address = ctx.r3.u32;
  ctx = saved;
  return address;
}

// Logged once per kind (a session log shows the skip is working).
void Note(const char* what, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("D3D draw skip: {} (Native Only: the game's D3D draws build no GPU commands)", what);
}

// EndVertices 0x82477238: commits the packet the Begin wrote.
void OnEndVertices(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g_begin_skipped) {
    g_begin_skipped = false;
    return;
  }
  original(ctx, base);
}

}  // namespace

void SetEmulatedIdle(bool idle) { g_emulated_idle.store(idle, std::memory_order_relaxed); }

bool Active() {
  return g_emulated_idle.load(std::memory_order_relaxed) && REXCVAR_GET(d3d_draw_skip);
}

bool BeginVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  // The original: (count x stride) bytes -> words (mullw + >> 2).
  const uint32_t vertices = Allocate(ctx, base, device, (count * stride) >> 2);
  if (!vertices) return false;  // out of memory: let the original deal with it
  static bool said = false;
  Note("first skipped immediate draw", said);
  g_begin_skipped = true;
  ctx.r3.u64 = vertices;
  return true;
}

bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32, vertex_count = ctx.r6.u32, index_count = ctx.r7.u32;
  const uint32_t flags = ctx.r8.u32, stride = ctx.r9.u32;
  const uint32_t index_out = ctx.r10.u32;
  const uint32_t vertex_out = Load32(base, ctx.r1.u32 + 0x54);  // 9th argument (caller's area)
  const uint32_t vertex_words = (vertex_count * stride) >> 2;
  // 16-bit indices: two per word (rounded up); 32-bit: one.
  const uint32_t index_words = (flags & 4) ? index_count : (index_count + 1) >> 1;
  const uint32_t vertices = Allocate(ctx, base, device, vertex_words);
  const uint32_t indices = vertices ? Allocate(ctx, base, device, index_words) : 0;
  if (!vertices || !indices) return false;  // (the original reports E_OUTOFMEMORY)
  static bool said = false;
  Note("first skipped indexed immediate draw", said);
  g_begin_skipped = true;
  if (vertex_out) Store32(base, vertex_out, vertices);
  if (index_out) Store32(base, index_out, indices);
  ctx.r3.u64 = 0;  // S_OK
  return true;
}

bool Draw(PPCContext&, uint8_t*) {
  static bool said = false;
  Note("first skipped mesh draw", said);
  return true;  // nothing: the caller ignores the result
}

void Install() { pddi::SetHandler(pddi::kFn_82477238, &OnEndVertices); }
void Uninstall() { pddi::SetHandler(pddi::kFn_82477238, nullptr); }

}  // namespace d3d_draw_skip
