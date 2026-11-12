// =============================================================================
// decomp/simple_setup.cpp -- xnSimpleShader::SetupDraw (sub_82441EA8) as C++
// =============================================================================
//
// WHAT THIS IS
//   The simple material's draw set-up: the material most of the world, the
//   HUD and the menus use (findings/09). Before every draw with it, PDDI's
//   Xbox back end calls this (xnShader vtable slot 14) to put the material's
//   state into Microsoft's D3D device: which shaders, which texture, its
//   colours, its fade, a few shader switches. The native renderer then reads
//   those values back from D3D's copy (native/recorder.cpp), so this rewrite
//   must write EXACTLY what the original writes, byte for byte: second step
//   of findings/60's option 1 ("native back end, bit-identical"), after the
//   matrix helpers and UpdateTransforms (decomp/matrices.cpp).
//
// HOW IT WAS READ (2026-11-13): xexdis of 0x82441EA8-0x82442490 next to
//   Ghidra's pseudo-C (tools/ghidra_decompile.sh), every helper it calls
//   disassembled and sized, the virtual calls' targets read from the vtables
//   (tools/rtti_vtables.py and the image).
//
// WHAT THE ORIGINAL DOES, IN ORDER (r31 = the shader; offsets on it)
//    1. +92 texture (an xnTexture, may be null). "Palettized" = its +276 == 1
//       (sub_8243AE08); without a texture: no.
//    2. State cache (+20): if its +8 count is above 1, it becomes 1 and the
//       15 words from +16 are cleared.
//    3. cache +12 = the texture; with one, sampler 0 gets its D3D texture
//       (sub_8243AD80: the first non-null of texture +20 / +92 / +16; its
//       device = *(texture +12) +400) through D3D's SetTexture sub_823066D0
//       with dirty bit 1<<31.
//    4. The VERTEX shader: table 0x82597010 [0] (+56 byte set) or [1] ->
//       cache +240 -> D3D SetVertexShader sub_82312E70(cache +4 = the device).
//    5. The PIXEL shader: palettized ? [5]/[6] : [2]/[3] (by the +56 byte,
//       read again after step 4) -> cache +236 -> SetPixelShader sub_82312BB8.
//    6. PS bools b80 = (+24 == 2), b81 = (+24 == 3) (SetPixelShaderConstantB
//       sub_82312B58, inlined: device +10128 + 4 x (reg / 32), bit reg % 32,
//       u64 at device +32 |= 1<<56).
//    7. PS c66-c69 (device +7040..+7100) = the colour words +64, +68, +76,
//       +72 as ARGB bytes / 255 in (R, G, B, A) order, u64 device +8 |= 1<<47
//       per row; PS c70 (+7104) = the raw words +80..+92, |= 1<<46.
//    8. PS c4 (+6048): x = (context slot 99: the float at context +364) x
//       +88; y z w = three words of its OWN stack frame it never writes
//       (r1+100/104/108: left over from earlier calls, like UpdateTransforms'
//       c8.w): copied from the same guest addresses. |= 1<<63.
//    9. PS c22 (+6336): x = texture ? 1.0 : -1.0; y = the r1+108 leftover;
//       z w = the 8-byte stack temporary at r1+112 the original used to
//       convert (+64 >> 8) & 0xFF: z = 0, w = that byte as a raw word.
//       |= 1<<58.
//   10. PS c52 (+6816): x y z = the camera position (*(context +384) +1572..)
//       through a stack copy (sub_824339A0 writes r1+160..168), w = the
//       r1+172 leftover. |= 1<<51.
//   11. Sampler 0's filter = +48 (sub_8242D760, inlined: cache +76 remembers
//       it; else three 3-bit fields of D3D's fetch constant word at device
//       +1152 from table 0x824EE804, u64 device +24 |= 1<<31 each), its
//       address mode = +52 (sub_8242D810: D3D leaves, called).
//   12. The common material state sub_8242DA28 (blend, alpha, depth bias,
//       shadow spheres; called).
//   13. Palettized: sampler 8 = the texture's current palette (sub_8243AE20,
//       inlined: texture +4 x (+88 + 6); dirty 1<<23); samplers 0 and 8
//       filtered "point" (sub_8242D810(cache, 0/8, 0)); PS c124 (+7808) =
//       (width, height, 0, 0) from the texture's slots 5 / 6 (+288 / +292),
//       |= 1<<35. Not palettized: sampler 8 cleared (SetTexture(device, 8,
//       null, 1<<23)).
//
// WHAT IS STILL THE GAME'S CODE: D3D's SetTexture / SetVertexShader /
//   SetPixelShader, sub_8242D810 and the common state sub_8242DA28 are called
//   through their usual entries (the PDDI intercept wrappers where they have
//   one): next steps of findings/60 s.10.
//
// EXACTNESS
//   * Floats: a byte / 255 is fcfid + frsp (exact) and fmuls (one rounding):
//     float(byte) * k in C++, contraction off. Words the original moves with
//     lfs / stfs go through float -> double -> float like the recompiled
//     code does (a signalling NaN comes out quiet there too).
//   * Stack: the original's frame is r1 - 368. Leftover words are read from
//     there; the helpers we still call get r1 = that frame, so their own
//     frames land where the original's did.
//   * Registers: r3 as the original leaves it (the last helper's, or the
//     device); r1, r27-r31 and lr put back as its epilogue does. Scratch
//     registers aren't reproduced: none of the 36 slot-14 virtual call sites
//     in 0x823F0000-0x82460000 (`lwz rX,56(..)` + `bctrl`) reads one after
//     the call before writing it (findings/60 s.10).
//   * The helpers get the registers the original had at each call: they
//     save r27-r31 and their return address below its frame, and later code
//     copies such leftover words (the common state's PS c201-c208 rows): r27
//     = 1, r28 = 0, r29 = palettized, r30 = the caller's / the shader table
//     0xFFFFFFFF82597010 (lis + addi, sign-extended) / (+68 >> 8) & 0xFF,
//     r31 = the shader, lr = the address after the original's `bl`. The
//     prologue's writes (saved f15-f31, r27-r31, lr, the back chain) and the
//     original's own temporaries are written too, as it leaves them.
//   * The original runs instead when a frame is being TRACED (inlined D3D
//     calls would be missing from the trace) or when a virtual slot holds
//     another function than the one read here.
//
// SWITCHES: --native_simple_setup (default on); --debug_native_simple_check
//   = original + ours on every call, compared byte for byte (decomp/check.h).
// =============================================================================

#include "simple_setup.h"

#include <cstdint>
#include <cstring>

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

// The game's functions this one still calls (their usual entries: the
// intercept wrappers for the D3D ones).
extern "C" REX_FUNC(sub_823066D0);  // D3D SetTexture(device, sampler, texture, dirty bit)
extern "C" REX_FUNC(sub_82312E70);  // D3D SetVertexShader(device, shader)
extern "C" REX_FUNC(sub_82312BB8);  // D3D SetPixelShader(device, shader)
extern "C" REX_FUNC(sub_8242D810);  // state cache: sampler address mode
extern "C" REX_FUNC(sub_8242DA28);  // xnShader: the common material state

REXCVAR_DEFINE_BOOL(native_simple_setup, true, "CrashMoM",
                    "The simple material's draw set-up (xnSimpleShader::SetupDraw) runs as our C++ (same "
                    "results); false = the recompiled original");
REXCVAR_DEFINE_BOOL(debug_native_simple_check, false, "CrashMoM",
                    "Debug: run the original simple-material set-up AND ours on every call and compare what they "
                    "write byte for byte (slow; log lines \"Native check:\")");

namespace decomp_simple {
namespace {

// --- the original's layout --------------------------------------------------------
constexpr uint32_t kFrame = 368;            // its stack frame
constexpr uint32_t kShaderTables = 0x82597010;  // vertex / pixel shader words
constexpr uint32_t kOneOver255 = 0x8200154C;    // 1/255 (the constant it loads)
constexpr uint32_t kPlusOne = 0x82002650;       // 1.0f
constexpr uint32_t kMinusOne = 0x82003950;      // -1.0f
constexpr uint32_t kZero = 0x8200074C;          // 0.0f
constexpr uint32_t kFilterTable = 0x824EE804;   // sub_8242D760's filter -> fetch bits
// Virtual slots and the functions read in them (else: the original runs).
constexpr uint32_t kContextSlot99 = 396, kContextSlot99Fn = 0x82432F30;  // lfs f1,364(r3)
constexpr uint32_t kTextureWidthSlot = 20, kTextureWidthFn = 0x8243B370;    // lwz r3,288(r3)
constexpr uint32_t kTextureHeightSlot = 24, kTextureHeightFn = 0x8243B378;  // lwz r3,292(r3)
// How much of the device and the state cache it (and its helpers) may write:
// the device up to SetTexture's ring pointer (+13528), the cache to +240.
constexpr uint32_t kDeviceBytes = 13536, kCacheBytes = 256;
constexpr uint32_t kGpuFrom = 10896, kGpuTo = 11600;  // words other threads / the GPU side move (check only)
// The guest stack the original and its helpers use below the caller's r1
// (368 + the deepest helper chain; generous).
constexpr uint32_t kStackBytes = 4096;

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
inline void Or64(uint8_t* base, uint32_t a, uint64_t mask) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  v = __builtin_bswap64(__builtin_bswap64(v) | mask);
  std::memcpy(GuestPtr(base, a), &v, 8);
}
// A word moved with lfs + stfs: float -> double -> float, as the recompiled
// code does (volatile so the compiler can't drop the round trip).
inline void MoveFloatWord(uint8_t* base, uint32_t to, uint32_t from) {
  volatile double d = LF(base, from);
  SF(base, to, float(d));
}
// rlwimi's mask, IBM bit numbers (0 = most significant).
constexpr uint32_t Mask(int mb, int me) { return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)); }
inline uint32_t Rotl(uint32_t v, int n) { return n ? (v << n) | (v >> (32 - n)) : v; }

// The original's callee-saved registers while it runs: its helpers save them
// (and their return address) into the stack below its frame, and later code
// copies such leftover words around (the common state's PS c201-c208 rows):
// so each helper gets the values the original had at that call.
struct Saved {
  uint64_t r29 = 0;  // the palettized flag (from 0x82441EEC on)
  uint64_t r30 = 0;  // the caller's, then the shader table, then (+68 >> 8) & 0xFF
  uint64_t r31 = 0;  // the shader
};

// Calls one of the game's functions as the original did at `site_lr` (the
// address after its `bl`): r1 = the original's frame, r27 = 1, r28 = 0,
// r29-r31 as `saved`. Returns its r3. On the LIVE context (copying the whole
// PPCContext, vector registers included, per call cost more than the
// set-up's own work): Setup puts back what the original's epilogue restores
// (r1, r27-r31; the helpers restore their own callee-saved registers).
uint64_t Call(PPCFunc* fn, PPCContext& c, uint8_t* base, uint32_t frame, uint32_t site_lr, const Saved& saved,
              uint32_t r3, uint32_t r4, uint32_t r5 = 0, uint64_t r6 = 0) {
  c.r1.u64 = frame;
  c.r27.u64 = 1;
  c.r28.u64 = 0;
  c.r29.u64 = saved.r29;
  c.r30.u64 = saved.r30;
  c.r31.u64 = saved.r31;
  c.lr = site_lr;
  c.r3.u64 = r3;
  c.r4.u64 = r4;
  c.r5.u64 = r5;
  c.r6.u64 = r6;
  fn(c, base);
  return c.r3.u64;
}

inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
}

// What the original's prologue writes: f15-f31 (savefpr 0x8245D3B4, r12 =
// r1 - 48), r27-r31 + the return address (savegpr 0x8245C124), the back
// chain (stwu r1,-368(r1)).
void Prologue(const PPCContext& ctx, uint8_t* base) {
  const uint32_t r1 = ctx.r1.u32;
  const PPCRegister* f = &ctx.f0;
  for (int k = 15; k <= 31; ++k) S64(base, r1 - 48 - 136 + 8 * (k - 15), f[k].u64);
  // (By name: PPCContext lists r3 before r0, so no pointer arithmetic here.)
  const uint64_t saved[5] = {ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  for (int k = 0; k < 5; ++k) S64(base, r1 - 48 + 8 * k, saved[k]);
  S(base, r1 - 8, uint32_t(ctx.lr));
  S(base, r1 - kFrame, r1);
}

// The four colour words -> one PS constant row: (R, G, B, A) / 255.
void ColourRow(uint8_t* base, uint32_t dev, uint32_t at, uint32_t argb, float k) {
  SF(base, at + 0, float((argb >> 16) & 0xFF) * k);
  SF(base, at + 4, float((argb >> 8) & 0xFF) * k);
  SF(base, at + 8, float(argb & 0xFF) * k);
  SF(base, at + 12, float(argb >> 24) * k);
  Or64(base, dev + 8, uint64_t(1) << 47);
}

// SetPixelShaderConstantB(device, reg, &value, 1) (sub_82312B58).
void SetPsBool(uint8_t* base, uint32_t dev, uint32_t reg, uint32_t value) {
  const uint32_t at = dev + 4 * (2532 + (reg >> 5));
  const uint32_t bit = 1u << (reg & 31);
  S(base, at, (L(base, at) & ~bit) | ((value & 1) << (reg & 31)));
  Or64(base, dev + 32, uint64_t(1) << 56);
}

// sub_8242D760(cache, sampler 0, filter): three fields of the fetch word.
void SetFilter0(uint8_t* base, uint32_t cache, uint32_t filter) {
  if (L(base, cache + 76) == filter) return;
  S(base, cache + 76, filter);
  const uint32_t v = L(base, kFilterTable + 4 * filter);
  const struct {
    int sh, mb, me;
  } fields[3] = {{10, 19, 21}, {13, 16, 18}, {16, 13, 15}};
  for (const auto& f : fields) {
    const uint32_t dev = L(base, cache + 4);
    const uint32_t m = Mask(f.mb, f.me);
    S(base, dev + 1152, (Rotl(v, f.sh) & m) | (L(base, dev + 1152) & ~m));
    Or64(base, dev + 24, uint64_t(1) << 31);
  }
}

// The whole set-up. False = not handled (the original runs; nothing written).
bool Setup(PPCContext& ctx, uint8_t* base) {
  if (pddi::trace::Recording()) return false;
  const uint32_t sh = ctx.r3.u32;
  const uint32_t context = L(base, sh + 16);
  const uint32_t tex0 = L(base, sh + 92);
  if (L(base, L(base, context) + kContextSlot99) != kContextSlot99Fn) return false;
  if (tex0) {
    const uint32_t vt = L(base, tex0);
    if (L(base, vt + kTextureWidthSlot) != kTextureWidthFn || L(base, vt + kTextureHeightSlot) != kTextureHeightFn) {
      return false;
    }
  }
  const uint32_t frame = ctx.r1.u32 - kFrame;
  uint64_t r3 = 0;
  Prologue(ctx, base);
  struct {
    PPCRegister r1, r27, r28, r29, r30, r31;
    uint64_t lr;
  } const entry{ctx.r1, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31, ctx.lr};
  Saved saved;
  saved.r30 = ctx.r30.u64;  // not set yet at the first call
  saved.r31 = ctx.r3.u64;

  // 1-2.
  const bool palettized = tex0 && L(base, tex0 + 276) == 1;
  saved.r29 = palettized ? 1 : 0;
  uint32_t cache = L(base, sh + 20);
  if (L(base, cache + 8) > 1) {
    S(base, cache + 8, 1);
    for (uint32_t k = 0; k < 15; ++k) S(base, cache + 16 + 4 * k, 0);
  }
  // 3.
  uint32_t tex = L(base, sh + 92);
  S(base, L(base, sh + 20) + 12, tex);
  if (tex) {
    uint32_t d3d = L(base, tex + 20);
    if (!d3d) d3d = L(base, tex + 92);
    if (!d3d) d3d = L(base, tex + 16);
    if (d3d) {
      r3 = Call(sub_823066D0, ctx, base, frame, 0x82441F40, saved, L(base, L(base, tex + 12) + 400), 0, d3d,
                uint64_t(1) << 31);
    }
  }
  // 4.
  uint32_t shader = L(base, kShaderTables + (*GuestPtr(base, sh + 56) ? 0 : 4));
  cache = L(base, sh + 20);
  S(base, cache + 240, shader);
  saved.r30 = 0xFFFFFFFF82597010ull;  // lis + addi: sign-extended in the 64-bit register
  Call(sub_82312E70, ctx, base, frame, 0x82441F70, saved, L(base, cache + 4), shader);
  // 5.
  const bool b56 = *GuestPtr(base, sh + 56) != 0;
  shader = L(base, kShaderTables + (palettized ? (b56 ? 20 : 24) : (b56 ? 8 : 12)));
  cache = L(base, sh + 20);
  S(base, cache + 236, shader);
  Call(sub_82312BB8, ctx, base, frame, 0x82441FBC, saved, L(base, cache + 4), shader);
  saved.r30 = (L(base, sh + 68) >> 8) & 0xFF;  // rlwinm r30,r11,24,24,31 at 0x82441FF8
  // 6.
  const uint32_t dev = L(base, context + 400);
  const uint32_t mode = L(base, sh + 24);
  SetPsBool(base, dev, 80, mode == 2);
  SetPsBool(base, dev, 81, mode == 3);
  // 7.
  const float k = LF(base, kOneOver255);
  ColourRow(base, dev, dev + 7040, L(base, sh + 64), k);
  ColourRow(base, dev, dev + 7056, L(base, sh + 68), k);
  ColourRow(base, dev, dev + 7072, L(base, sh + 76), k);
  ColourRow(base, dev, dev + 7088, L(base, sh + 72), k);
  for (uint32_t i = 0; i < 4; ++i) MoveFloatWord(base, dev + 7104 + 4 * i, sh + 80 + 4 * i);
  Or64(base, dev + 8, uint64_t(1) << 46);
  // 8.
  SF(base, dev + 6048, LF(base, context + 364) * LF(base, sh + 88));
  MoveFloatWord(base, dev + 6052, frame + 100);
  MoveFloatWord(base, dev + 6056, frame + 104);
  MoveFloatWord(base, dev + 6060, frame + 108);
  Or64(base, dev + 8, uint64_t(1) << 63);
  // 9. (z, w: the r1+112 temporary = the 64-bit integer (+64 >> 8) & 0xFF)
  SF(base, dev + 6336, LF(base, L(base, sh + 92) ? kPlusOne : kMinusOne));
  MoveFloatWord(base, dev + 6340, frame + 108);
  {
    volatile double z = 0.0f;
    SF(base, dev + 6344, float(z));
    const uint32_t byte = (L(base, sh + 64) >> 8) & 0xFF;
    float w;
    std::memcpy(&w, &byte, 4);
    volatile double wd = w;
    SF(base, dev + 6348, float(wd));
  }
  Or64(base, dev + 8, uint64_t(1) << 58);
  // 10.
  const uint32_t camera = L(base, context + 384);
  for (uint32_t i = 0; i < 3; ++i) MoveFloatWord(base, dev + 6816 + 4 * i, camera + 1572 + 4 * i);
  MoveFloatWord(base, dev + 6828, frame + 172);
  Or64(base, dev + 8, uint64_t(1) << 51);
  // 11.
  SetFilter0(base, L(base, sh + 20), L(base, sh + 48));
  r3 = Call(sub_8242D810, ctx, base, frame, 0x82442398, saved, L(base, sh + 20), 0, L(base, sh + 52));
  // 12.
  r3 = Call(sub_8242DA28, ctx, base, frame, 0x824423A0, saved, sh, 0);
  // 13.
  if (palettized) {
    tex = L(base, sh + 92);
    const uint32_t palette = L(base, tex + 4 * (L(base, tex + 88) + 6));
    Call(sub_823066D0, ctx, base, frame, 0x824423B4, saved, L(base, L(base, tex + 12) + 400), 8, palette,
         uint64_t(1) << 23);
    Call(sub_8242D810, ctx, base, frame, 0x824423C4, saved, L(base, sh + 20), 0, 0);
    Call(sub_8242D810, ctx, base, frame, 0x824423D4, saved, L(base, sh + 20), 8, 0);
    tex = L(base, sh + 92);
    const int32_t height = int32_t(L(base, tex + 292));
    const int32_t width = int32_t(L(base, tex + 288));
    const float zero = LF(base, kZero);
    SF(base, dev + 7820, zero);
    SF(base, dev + 7816, zero);
    SF(base, dev + 7812, float(height));
    SF(base, dev + 7808, float(width));
    Or64(base, dev + 8, uint64_t(1) << 35);
    r3 = dev;
  } else {
    r3 = Call(sub_823066D0, ctx, base, frame, 0x8244247C, saved, dev, 8, 0, 0x00800000);
  }
  // The original's own stack temporaries, as it leaves them (later code at
  // this depth reads leftovers: see the header): the 64-bit integers it
  // converted to floats, and the PS bool words.
  {
    const uint32_t c64 = L(base, sh + 64), c68 = L(base, sh + 68), c72 = L(base, sh + 72);
    S(base, frame + 80, mode == 2);
    S(base, frame + 84, (c64 >> 16) & 0xFF);
    S(base, frame + 88, mode == 3);
    S(base, frame + 92, (c68 >> 8) & 0xFF);
    S64(base, frame + 112, (c64 >> 8) & 0xFF);
    S64(base, frame + 120, c64 & 0xFF);
    S64(base, frame + 128, c64 >> 24);
    S64(base, frame + 136, (c72 >> 16) & 0xFF);
    S64(base, frame + 144, palettized ? uint64_t(int64_t(int32_t(L(base, L(base, sh + 92) + 288)))) : (c72 & 0xFF));
    // (+160..+168: the camera copy, written in step 10 below's place)
    for (uint32_t i = 0; i < 3; ++i) MoveFloatWord(base, frame + 160 + 4 * i, camera + 1572 + 4 * i);
  }
  // The original's epilogue: r1 and r27-r31 back (restgpr / its stack
  // pointer), lr = the caller's return address.
  ctx.r1 = entry.r1;
  ctx.r27 = entry.r27;
  ctx.r28 = entry.r28;
  ctx.r29 = entry.r29;
  ctx.r30 = entry.r30;
  ctx.r31 = entry.r31;
  ctx.lr = entry.lr;
  ctx.r3.u64 = r3;
  return true;
}

decomp::Stats g_stats{"simple set-up"};
uint64_t g_not_checked = 0;

// Would one of the D3D setters it calls take its COMMAND RING path? When the
// object a setter replaces (sampler 0 / 8's texture, the vertex / pixel
// shader) is still in use by the GPU (device +10908 == 0 and +10912 & the
// object's first word), the setter writes a fence into D3D's command ring and
// may kick it to the GPU (sub_82307D08: allocations outside the device).
// Done twice (original + ours, the check) that froze the game: such calls
// aren't checked, only run (ours), and counted.
bool TakesRingPath(uint8_t* base, uint32_t dev) {
  if (L(base, dev + 10908) != 0) return false;
  const uint32_t busy = L(base, dev + 10912);
  const uint32_t objects[4] = {L(base, dev + 4 * 3136), L(base, dev + 4 * (3136 + 8)), L(base, dev + 12696),
                               L(base, dev + 12692)};
  for (const uint32_t o : objects) {
    if (o && (busy & L(base, o))) return true;
  }
  return false;
}

}  // namespace

void SetupDraw(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t sh = ctx.r3.u32;
  // Ranges for the check: the device and the state cache.
  decomp::Ranges ranges{};
  bool checking = REXCVAR_GET(debug_native_simple_check);
  if (checking && TakesRingPath(base, L(base, L(base, sh + 16) + 400))) {
    checking = false;
    if ((++g_not_checked & 0xFFF) == 1) {
      REXLOG_INFO("Native check: simple set-up: {} calls not checked (a D3D setter's command-ring path)", g_not_checked);
    }
  }
  if (checking) {
    const uint32_t cache = L(base, sh + 20);
    // The device in two parts around +10896..+11600: words OTHER threads and
    // the GPU side change while it runs (putting back a snapshot of them
    // froze the game: the main thread then waited in D3D's swap for a fence
    // that never came). Seen moving there: +10904 D3D's allocator lock
    // (sub_82308798, owner from r13), +10988, +11540 (a counter, +-1 between
    // the two runs), +11556 (a pointer 1.8 MB ahead). The set-up and its
    // helpers only READ in that window (+10908 / +10912: the setters' "still
    // in use" test). Then the cache, and the stack below the caller's frame:
    // put back, not compared (the helpers' own frames, whose leftovers the
    // common state reads).
    const uint32_t dev = L(base, L(base, sh + 16) + 400);
    ranges = decomp::Ranges{{{dev, kGpuFrom},
                             {dev + kGpuTo, kDeviceBytes - kGpuTo},
                             {cache, kCacheBytes},
                             {ctx.r1.u32 - kStackBytes, kStackBytes, false}},
                            4};
  }
  decomp::Run(REXCVAR_GET(native_simple_setup), checking, g_stats, ctx, base,
              original, ranges, [&](PPCContext& c) { return Setup(c, base); });
}

}  // namespace decomp_simple
