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
// WHAT IS STILL THE GAME'S CODE: only the rare COMMAND-RING path of D3D's
//   SetTexture / SetVertexShader / SetPixelShader (the object they replace
//   still in use by the GPU: Busy()); those calls run the game's function.
//   Everything else is inlined below (2026-11-13, findings/60 s.11): the three
//   setters, the address-mode setter sub_8242D810 with D3D's two sampler
//   leaves sub_8230C338 / sub_8230C4E0, and the common material state
//   sub_8242DA28 with its blend (sub_8242D8C8 + sub_8230AAC8 / AB58 / ABE8 /
//   AC78), alpha (sub_8242D980 + sub_8230AA90 / AF50 / AEF0), the +84 word
//   (sub_8230AA30 / sub_82430970), the shadow spheres (sub_8237F378) and
//   SetPixelShaderConstantF (sub_82312A78).
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
#include <cmath>
#include <climits>
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
REXCVAR_DEFINE_BOOL(debug_native_simple_check_stack, false, "CrashMoM",
                    "Debug: with debug_native_simple_check, compare the guest stack below the caller's frame too "
                    "(every saved register, return address and temporary the original and its helpers leave)");
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

// --- the helpers, inlined (2026-11-13, findings/60 s.11) ---------------------------
// Each one below is the game's function written out: what it stores, in its
// order, INCLUDING what it leaves on the guest stack (its saved registers,
// return address, back chain and temporaries), because the common state
// copies leftover stack words into PS constants. `r1` = the stack pointer
// the original had when it called the helper.

inline uint64_t L64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  return __builtin_bswap64(v);
}
inline uint8_t LB(uint8_t* base, uint32_t a) { return *GuestPtr(base, a); }
inline void SB(uint8_t* base, uint32_t a, uint8_t v) { *GuestPtr(base, a) = v; }
inline uint16_t LH(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint16_t(p[0] << 8 | p[1]);
}
// rlwinm / rlwimi masks, IBM bit numbers; mb > me = the wrapped mask.
constexpr uint32_t MaskW(int mb, int me) { return mb <= me ? Mask(mb, me) : (Mask(mb, 31) | Mask(0, me)); }
inline uint32_t Rlwinm(uint32_t rs, int sh, int mb, int me) { return Rotl(rs, sh) & MaskW(mb, me); }
inline uint32_t Rlwimi(uint32_t ra, uint32_t rs, int sh, int mb, int me) {
  const uint32_t m = MaskW(mb, me);
  return (Rotl(rs, sh) & m) | (ra & ~m);
}
// A word moved with lfs + stfs (MoveFloatWord), from a value.
inline uint32_t RoundTrip(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  volatile double d = f;
  const float g = float(d);
  uint32_t out;
  std::memcpy(&out, &g, 4);
  return out;
}
// savegpr from register `first` (0x8245C108 + 4 x (first - 20)): r_k at
// r1 - 8 x (32 - k) - 8 ... r31 at r1 - 16, then the return address at r1 - 8.
// `regs` = the values of r_first..r31.
void SaveGprs(uint8_t* base, uint32_t r1, int first, const uint64_t* regs, uint32_t lr) {
  for (int k = first; k <= 31; ++k) S64(base, r1 - 8 * (32 - k) - 8, regs[k - first]);
  S(base, r1 - 8, lr);
}

// The set-up's registers r20-r31 at a call (r20-r26 = its caller's: it
// doesn't use them).
struct Regs {
  uint64_t r[12];
};
Regs SetupRegs(const PPCContext& c, const Saved& saved) {
  return Regs{{c.r20.u64, c.r21.u64, c.r22.u64, c.r23.u64, c.r24.u64, c.r25.u64, c.r26.u64, 1, 0, saved.r29,
               saved.r30, saved.r31}};
}

// Would this D3D setter take its command-ring path (the object it replaces
// still in use by the GPU)? Then the game's own function runs (see
// TakesRingPath below).
bool Busy(uint8_t* base, uint32_t dev, uint32_t old) {
  return old && L(base, dev + 10908) == 0 && (L(base, dev + 10912) & L(base, old)) != 0;
}

// D3D SetTexture sub_823066D0(dev, sampler, texture, dirty), the path
// without the ring (a busy old texture: Busy). Returns its r3.
uint64_t SetTexture(uint8_t* base, const Regs& regs, uint32_t r1, uint32_t lr, uint32_t dev, uint32_t smp,
                    uint32_t tex, uint64_t dirty) {
  SaveGprs(base, r1, 20, regs.r, lr);  // bl 0x8245c108
  S(base, r1 - 192, r1);               // stwu r1,-192(r1)
  uint64_t r3 = dev;
  const uint32_t slot = 4 * (smp + 3136);
  const uint32_t old = L(base, dev + slot);
  if (tex) {
    uint32_t r27 = L(base, tex + 48);
    uint32_t r7 = L(base, tex + 32);
    uint32_t r9 = Rlwinm(r27, 12, 20, 31);
    const uint32_t r26 = L(base, tex + 44);
    const uint32_t r11 = dev + 24 * (smp + 48);
    uint32_t r25 = L(base, tex + 28);
    const uint32_t r24 = L(base, tex + 36);
    uint32_t r23 = L(base, tex + 40);
    uint32_t r10 = Rlwinm(r7, 12, 20, 31);
    r9 += 512;
    r10 += 512;
    const uint32_t r30 = r7 & 0x1FFFFFFF;
    uint32_t r8 = Rlwinm(r9, 0, 19, 19);
    uint32_t r3w = Rlwinm(r10, 0, 19, 19);
    const uint32_t r22 = L(base, r11 + 0);
    const uint32_t r9a = dev + smp;
    const uint32_t r4 = L(base, r11 + 16);
    r7 = Rlwinm(r27, 0, 3, 22);
    r27 = L(base, r11 + 4);
    r10 = r26;
    const uint32_t r21 = L(base, r11 + 12);
    const uint32_t r20 = L(base, r11 + 20);
    r7 = r8 + r7;
    r3w = r3w + r30;
    S(base, r11 + 8, r24);
    r10 = Rlwimi(r10, r4, 0, 30, 21);
    r8 = LB(base, r9a + 11950);
    r25 = Rlwimi(r25, r22, 0, 10, 21);
    r3w = Rlwimi(r3w, r27, 0, 20, 20);
    S(base, r11 + 16, r10);
    r23 = Rlwimi(r23, r21, 0, 1, 12);
    S(base, r11 + 0, r25);
    r7 = Rlwimi(r7, r20, 0, 23, 31);
    S(base, r11 + 4, r3w);
    const uint32_t r4b = Rlwinm(r26, 30, 28, 31);
    S(base, r11 + 12, r23);
    S(base, r11 + 20, r7);
    if (r4b > r8) r8 = Rlwinm(r26, 30, 28, 31);
    r7 = L(base, tex + 44);
    r10 = Rlwimi(r10, r8, 2, 26, 29);
    uint32_t r9b = LB(base, r9a + 11976);
    r8 = Rlwinm(r7, 26, 28, 31);
    S(base, r11 + 16, r10);
    if (r8 < r9b) r9b = r8;
    const uint64_t d = L64(base, dev + 24);
    r10 = Rlwimi(r10, r9b, 6, 22, 25);
    S(base, r11 + 16, r10);
    S64(base, dev + 24, d | dirty);
    r3 = r3w;
  }
  S(base, dev + slot, tex);
  if (old) {
    const uint32_t done = L(base, dev + 10908);
    if (done) S(base, old + 8, done);
    // (+10912 & the old object's word: the ring path, never reached here.)
  }
  return r3;
}

// The two D3D shader setters' constant blocks (SetVertexShader from
// 0x82312F68, SetPixelShader from 0x82312CA0): copies (memcpy sub_8245C1A0,
// whose `std r3,-8(r1)` leaves the destination below the setter's frame) and
// and / or patches into the device's fetch constants (+1152).
void ShaderBlocks(uint8_t* base, uint32_t dev, uint32_t block, uint32_t setter_r1) {
  uint32_t r31 = block + 20;
  const uint32_t r28 = dev + 1152;
  const uint32_t end = L(base, block + 16) + r31;
  // Skip the first entries (count word != 0 = 8 bytes each) up to an empty one.
  bool found = false;
  while (r31 < end) {
    const uint16_t n = LH(base, r31 + 2);
    r31 += 4;
    if (n == 0) {
      found = true;
      break;
    }
    r31 += 4;
  }
  (void)found;
  if (r31 >= end) return;
  // Copies: (offset, count words), then the words.
  for (;;) {
    const uint16_t n = LH(base, r31 + 2);
    const uint16_t off = LH(base, r31);
    r31 += 4;
    if (n == 0) break;
    const uint32_t bytes = uint32_t(n) << 2;
    const uint32_t to = off + r28;
    S64(base, setter_r1 - 8, to);  // memcpy's std r3,-8(r1)
    std::memmove(GuestPtr(base, to), GuestPtr(base, r31), bytes);
    r31 += bytes;
    if (r31 >= end) break;
  }
  // Patches: (offset, count), then (and, or) pairs; the count steps by 2.
  while (r31 < end) {
    const uint16_t n0 = LH(base, r31 + 2);
    const uint16_t off = LH(base, r31);
    r31 += 4;
    if (n0 == 0) return;
    uint32_t p = off + r28;
    uint32_t n = n0;
    do {
      const uint32_t w = L(base, p);
      n = (n + 0x10000 - 2) & 0xFFFF;
      const uint32_t a = L(base, r31), o = L(base, r31 + 4);
      r31 += 8;
      S(base, p, (w & a) | o);
      p += 4;
    } while (n != 0);
  }
}

// D3D SetVertexShader sub_82312E70(dev, shader), without the ring path.
void SetVertexShader(uint8_t* base, const Regs& regs, uint32_t r1, uint32_t lr, uint32_t dev, uint32_t vs) {
  SaveGprs(base, r1, 28, regs.r + 8, lr);  // bl 0x8245c128
  S(base, r1 - 128, r1);
  if (vs) Or64(base, dev + 16, 0x80000);
  S(base, dev + 12696, vs);
  SB(base, dev + 10942, LB(base, dev + 10942) & 0x7F);
  if (!vs) return;
  const uint32_t r11 = vs + 872;
  if (r11 == 0) return;
  const uint32_t r10 = L(base, r11 + 20);
  if (r10 == 0) return;
  const uint32_t block = r10 + r11;
  S64(base, dev + 0, L64(base, dev + 0) & ~L64(base, block + 0));
  if (L64(base, block + 8) != 0) Or64(base, dev + 32, uint64_t(1) << 56);
  ShaderBlocks(base, dev, block, r1 - 128);
}

// D3D SetPixelShader sub_82312BB8(dev, shader), without the ring path.
void SetPixelShader(uint8_t* base, const Regs& regs, uint32_t r1, uint32_t lr, uint32_t dev, uint32_t ps) {
  SaveGprs(base, r1, 28, regs.r + 8, lr);
  S(base, r1 - 128, r1);
  S(base, dev + 12692, ps);
  Or64(base, dev + 16, 0x100000);
  Or64(base, dev + 16, 0x20000);
  if (!ps) return;
  const uint32_t r10 = L(base, ps + 60);
  if (r10 == 0) return;
  const uint32_t block = r10 + ps + 40;
  S64(base, dev + 8, L64(base, dev + 8) & ~L64(base, block + 0));
  if (L64(base, block + 8) != 0) Or64(base, dev + 32, uint64_t(1) << 56);
  ShaderBlocks(base, dev, block, r1 - 128);
}

// D3D's two sampler leaves under sub_8242D810: sub_8230C338 (`second` =
// false) and sub_8230C4E0 (true). Same code, other bit fields. They save
// the address-mode setter's r29-r31 + lr below its stack pointer `r1`.
void SamplerLeaf(uint8_t* base, bool second, uint32_t r1, uint32_t lr, const uint64_t* r29_31, uint32_t dev,
                 uint32_t smp, uint32_t v) {
  SaveGprs(base, r1, 29, r29_31, lr);  // bl 0x8245c12c (no frame of its own)
  const uint32_t r11 = dev + 24 * (smp + 48);
  const uint32_t r10a = dev + smp;
  uint32_t r9 = uint32_t(LB(base, r10a + 11924)) << 2;
  const uint32_t r7 = Rlwinm(v, 30, 2, 31);
  uint32_t r6 = L(base, r11 + 16);
  uint32_t r31 = L(base, r11 + 12);
  uint32_t r29 = r6;
  r6 = second ? Rlwinm(r6, 21, 31, 31) : Rlwinm(r6, 22, 31, 31);
  const uint32_t r8a = L(base, 0x82001500 + r9);
  r29 = second ? Rlwimi(r29, r7, 10, 21, 21) : Rlwimi(r29, r7, 11, 20, 20);
  r9 = r6 | r7;
  S(base, r11 + 16, r29);
  r9 = r9 - 1;
  r9 = r8a & ~r9;
  const uint64_t dirty = (uint64_t(1) << 63) >> (smp + 32);
  r9 = second ? Rlwinm(r9, 6, 0, 25) : Rlwinm(r9, 4, 0, 27);
  r9 |= r7;
  r9 |= v;
  if (second) {
    r31 = Rlwimi(r31, r9, 19, 11, 12);
    r31 = Rlwimi(r31, r9, 19, 4, 6);
  } else {
    r31 = Rlwimi(r31, r9, 21, 9, 10);
    r31 = Rlwimi(r31, r9, 21, 4, 6);
  }
  S(base, r11 + 12, r31);
  uint32_t r9b = LB(base, r10a + 12002);
  uint32_t r6b = r31;
  const uint32_t r7b = r31;
  uint32_t r10 = Rlwinm(r9b, 30, 2, 31);
  r6b = Rlwimi(r6b, r7b, 31, 13, 31);
  r10 = r10 - 1;
  r6b = Rlwimi(r6b, r7b, 31, 1, 11);
  r9b = r9b & ~r10;
  const uint32_t r7c = Rlwinm(r6b, 13, 20, 31);
  r10 = r7c & r10;
  r10 = r10 + r9b;
  r10 = Rlwimi(r10, r29, 0, 0, 29);
  S(base, r11 + 16, r10);
  Or64(base, dev + 24, dirty);
}

// sub_8242D810(cache, sampler, address mode). `regs` = the caller's r28-r31.
// Returns its r3.
uint64_t SetAddressMode(uint8_t* base, const uint64_t* r28_31, uint32_t r1, uint32_t lr, uint32_t cache,
                        uint32_t smp, uint32_t v) {
  SaveGprs(base, r1, 28, r28_31, lr);
  S(base, r1 - 128, r1);
  const uint32_t at = cache + 4 * (smp + 35);
  if (L(base, at) == v) return cache;
  S(base, at, v);
  constexpr uint32_t kTable = 0x824EE71C;
  const uint32_t row = kTable + 12 * v;
  const uint64_t own[3] = {0xFFFFFFFF824EE71Cull, cache, smp};  // its r29 (lis + addi), r30, r31
  SamplerLeaf(base, false, r1 - 128, 0x8242D85C, own, L(base, cache + 4), smp, L(base, row));
  SamplerLeaf(base, true, r1 - 128, 0x8242D870, own, L(base, cache + 4), smp, L(base, row + 4));
  const uint32_t dev = L(base, cache + 4);
  const uint32_t r11 = dev + 24 * smp;
  S(base, r11 + 1164, Rlwimi(L(base, r11 + 1164), L(base, row + 8), 23, 7, 8));
  const uint64_t old = L64(base, dev + 24);
  S64(base, dev + 24, ((uint64_t(1) << 63) >> (smp + 32)) | old);
  return old;
}

// D3D's blend / alpha leaves under the common state.
uint32_t Fold(uint32_t r11) {
  uint32_t r10 = r11 & 0x1010;
  const uint32_t r9 = Rlwinm(r11, 4, 12, 27);
  r11 &= 0xFFFF;
  r10 |= r9;
  r10 = Rlwinm(r10, 12, 0, 15);
  r10 = Rlwinm(r10, 0, 12, 10);
  r10 = Rlwinm(r10, 0, 4, 2);
  return r10 | r11;
}
void WriteBlend(uint8_t* base, uint32_t dev, uint32_t v) {
  S(base, dev + 10588, v);
  S(base, dev + 10552, v);
  S(base, dev + 10584, v);
  S(base, dev + 10592, v);
  Or64(base, dev + 16, 0x407);
}
void BlendEnable(uint8_t* base, uint32_t dev, uint32_t on) {  // sub_8230AAC8
  const uint32_t w = Rlwimi(L(base, dev + 11852), on, 31, 0, 0);
  S(base, dev + 11852, w);
  uint32_t r11 = L(base, dev + 11848);
  if (!(w & 0x40000000)) r11 = Fold(r11);
  if (int32_t(on) == 0) r11 = 0x00010001;
  WriteBlend(base, dev, r11);
}
// sub_8230AB58 (5, 24, 26) / sub_8230ABE8 (0, 27, 31) / sub_8230AC78 (8, 19, 23).
void BlendField(uint8_t* base, uint32_t dev, uint32_t v, int sh, int mb, int me) {
  const uint32_t r10 = Rlwimi(L(base, dev + 11848), v, sh, mb, me);
  const uint32_t r11 = L(base, dev + 11852);
  S(base, dev + 11848, r10);
  if (!(r11 & 0x80000000)) return;
  uint32_t w = L(base, dev + 11848);
  if (!(r11 & 0x40000000)) w = Fold(w);
  WriteBlend(base, dev, w);
}
void SetAlphaRef(uint8_t* base, uint32_t r1, uint32_t dev, uint32_t v) {  // sub_8230AEF0
  S64(base, r1 - 16, v);  // std r11,-16(r1) (the zero-extended value)
  volatile double d = double(uint64_t(v));
  const float f = float(d) * LF(base, kOneOver255);
  SF(base, dev + 10500, f);
  Or64(base, dev + 16, 0x08000000);
}
void SetWord10568(uint8_t* base, uint32_t dev, uint32_t v) {  // sub_8230AA30
  S(base, dev + 10568, Rlwimi(v, L(base, dev + 10568), 0, 0, 28));
  Or64(base, dev + 16, 64);
}
// The PPC fctidz as the recompiled code computes it.
int64_t Fctidz(double x) {
  if (std::isnan(x)) return int64_t(0x8000000000000000ull);
  if (x > double(LLONG_MAX)) return LLONG_MAX;
  if (x < -9223372036854775808.0) return int64_t(0x8000000000000000ull);
  return int64_t(x);
}

// xnShader's common material state sub_8242DA28(shader), called by the set-up
// at r1 = its frame (`frame`). Returns its r3 (the device).
uint64_t CommonState(uint8_t* base, uint32_t frame, const Saved& saved, uint32_t sh) {
  const uint64_t r28_31[4] = {0, saved.r29, saved.r30, saved.r31};
  SaveGprs(base, frame, 28, r28_31, 0x824423A0);
  const uint32_t r1 = frame - 240;
  S(base, r1, frame);
  const uint32_t context = L(base, sh + 16);
  const float f13 = LF(base, sh + 88) * LF(base, context + 364);  // slot 99 (checked in Setup)
  const int32_t mode = int32_t(L(base, sh + 24));
  constexpr uint32_t kRows = 0x824EE758;
  // bge after fcmpu: taken unless f13 < 1.0 (a NaN takes it too).
  const uint32_t row = (!(f13 < LF(base, kPlusOne)) || mode == 2 || mode == 3) ? kRows + (uint32_t(mode) << 4)
                                                                                 : kRows + 16;
  const uint32_t a4 = LB(base, row), a5 = L(base, row + 4), a6 = L(base, row + 8), a7 = L(base, row + 12);
  // Its r28-r31 while it calls: the set-up's r28 / r29, r30 = the shader, r31
  // = the set-up's r31 (the shader too).
  const uint64_t c28_31[4] = {0, saved.r29, sh, saved.r31};
  // sub_8242D8C8(cache, a4..a7): blend.
  {
    const uint32_t cache = L(base, sh + 20);
    SaveGprs(base, r1, 28, c28_31, 0x8242DAC8);
    S(base, r1 - 128, r1);
    const uint32_t b = a4 & 0xFF;
    if (LB(base, cache + 204) != b) {
      SB(base, cache + 204, uint8_t(a4));
      BlendEnable(base, L(base, cache + 4), b);
    }
    if (LB(base, cache + 204)) {
      constexpr uint32_t kBlend = 0x824EE7D8;
      if (L(base, cache + 208) != a5) {
        S(base, cache + 208, a5);
        BlendField(base, L(base, cache + 4), a5, 5, 24, 26);
      }
      if (int32_t(a6) != int32_t(L(base, cache + 212))) {
        S(base, cache + 212, a6);
        BlendField(base, L(base, cache + 4), L(base, kBlend + 4 * a6), 0, 27, 31);
      }
      if (int32_t(a7) != int32_t(L(base, cache + 216))) {
        S(base, cache + 216, a7);
        BlendField(base, L(base, cache + 4), L(base, kBlend + 4 * a7), 8, 19, 23);
      }
    }
  }
  // sub_8242D980(cache, +36 byte, +40, 0): alpha test.
  {
    const uint32_t cache = L(base, sh + 20);
    const uint32_t b = LB(base, sh + 36);
    const uint32_t v5 = L(base, sh + 40);
    SaveGprs(base, r1, 29, c28_31 + 1, 0x8242DADC);
    const uint32_t d_r1 = r1 - 112;
    S(base, d_r1, r1);
    if (LB(base, cache + 220) != b) {
      SB(base, cache + 220, uint8_t(b));
      const uint32_t dev = L(base, cache + 4);  // sub_8230AA90
      S(base, dev + 10556, Rlwimi(L(base, dev + 10556), b, 3, 28, 28));
      Or64(base, dev + 16, 512);
      Or64(base, dev + 16, 0x40000);
    }
    if (LB(base, cache + 220) && int32_t(L(base, cache + 224)) != int32_t(v5)) {
      S(base, cache + 224, v5);
      const uint32_t dev = L(base, cache + 4);  // sub_8230AF50
      S(base, dev + 10556, Rlwimi(L(base, 0x824EE80C + 4 * v5), L(base, dev + 10556), 0, 0, 28));
      Or64(base, dev + 16, 512);
    }
    if (LB(base, cache + 220) && L(base, cache + 228) != 0) {
      S(base, cache + 228, 0);
      SetAlphaRef(base, d_r1, L(base, cache + 4), 0);
    }
  }
  // Depth bias? (+44 x 1/255 -> an integer) -> sub_8230AEF0 on the camera's object.
  {
    const float f12 = LF(base, sh + 44) * LF(base, 0x82001550);
    const int64_t i = Fctidz(f12);
    S64(base, r1 + 88, uint64_t(i));
    SetAlphaRef(base, r1, L(base, L(base, context + 384) + 16), uint32_t(uint64_t(i)));
  }
  // +84 byte -> cache +232: sub_8230AA30(dev, 0) or sub_82430970(cache +0).
  {
    const uint32_t cache = L(base, sh + 20);
    const uint8_t v = LB(base, sh + 84);
    if (LB(base, cache + 232) != v) {
      SB(base, cache + 232, v);
      if (v) {
        SetWord10568(base, L(base, cache + 4), 0);
      } else {
        const uint32_t obj = L(base, cache + 0);
        const uint32_t r11 = L(base, obj + 140);
        SetWord10568(base, L(base, obj + 400), L(base, 0x824EDCA0 + 4 * L(base, r11 + 4)));
      }
    }
  }
  // The shadow spheres: up to 8 records (0x82572068 + 32 i, on if byte +25),
  // their +8..+16 packed into its stack from r1+96, then PS c202 = (count,
  // three stack words) and 8 rows from r1+96 -> PS c203-c210 (sub_82312A78).
  uint32_t r31 = r1 + 104;
  int32_t count = 0;
  for (uint32_t k = 0; k < 8; ++k) {
    const uint32_t rec = 0x82572068 + 32 * k;
    if (!LB(base, rec + 25)) continue;
    ++count;
    S(base, r31 - 8, RoundTrip(L(base, rec + 8)));
    S(base, r31 - 4, RoundTrip(L(base, rec + 12)));
    S(base, r31 + 0, RoundTrip(L(base, rec + 16)));
    r31 += 12;
  }
  S64(base, r1 + 88, uint64_t(int64_t(count)));
  const uint32_t dev = L(base, context + 400);
  {
    volatile double d = double(int64_t(count));
    S(base, dev + 9228, RoundTrip(L(base, r1 + 92)));
    S(base, dev + 9220, RoundTrip(L(base, r1 + 84)));
    S(base, dev + 9224, RoundTrip(L(base, r1 + 88)));
    SF(base, dev + 9216, float(d));
    Or64(base, dev + 8, 8192);
  }
  S64(base, r1 - 8, r31);  // sub_82312A78's std r31,-8(r1)
  std::memmove(GuestPtr(base, dev + (201 + 376) * 16), GuestPtr(base, r1 + 96), 8 * 16);
  Or64(base, dev + 8, 14336);
  return dev;
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
      const uint32_t tdev = L(base, L(base, tex + 12) + 400);
      if (Busy(base, tdev, L(base, tdev + 4 * 3136))) {
        r3 = Call(sub_823066D0, ctx, base, frame, 0x82441F40, saved, tdev, 0, d3d, uint64_t(1) << 31);
      } else {
        r3 = SetTexture(base, SetupRegs(ctx, saved), frame, 0x82441F40, tdev, 0, d3d, uint64_t(1) << 31);
      }
    }
  }
  // 4.
  uint32_t shader = L(base, kShaderTables + (*GuestPtr(base, sh + 56) ? 0 : 4));
  cache = L(base, sh + 20);
  S(base, cache + 240, shader);
  saved.r30 = 0xFFFFFFFF82597010ull;  // lis + addi: sign-extended in the 64-bit register
  {
    const uint32_t d = L(base, cache + 4);
    if (Busy(base, d, L(base, d + 12696))) {
      Call(sub_82312E70, ctx, base, frame, 0x82441F70, saved, d, shader);
    } else {
      SetVertexShader(base, SetupRegs(ctx, saved), frame, 0x82441F70, d, shader);
    }
  }
  // 5.
  const bool b56 = *GuestPtr(base, sh + 56) != 0;
  shader = L(base, kShaderTables + (palettized ? (b56 ? 20 : 24) : (b56 ? 8 : 12)));
  cache = L(base, sh + 20);
  S(base, cache + 236, shader);
  {
    const uint32_t d = L(base, cache + 4);
    if (Busy(base, d, L(base, d + 12692))) {
      Call(sub_82312BB8, ctx, base, frame, 0x82441FBC, saved, d, shader);
    } else {
      SetPixelShader(base, SetupRegs(ctx, saved), frame, 0x82441FBC, d, shader);
    }
  }
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
  const uint64_t r28_31[4] = {0, saved.r29, saved.r30, saved.r31};
  r3 = SetAddressMode(base, r28_31, frame, 0x82442398, L(base, sh + 20), 0, L(base, sh + 52));
  // 12.
  r3 = CommonState(base, frame, saved, sh);
  // 13.
  if (palettized) {
    tex = L(base, sh + 92);
    const uint32_t palette = L(base, tex + 4 * (L(base, tex + 88) + 6));
    const uint32_t tdev = L(base, L(base, tex + 12) + 400);
    if (Busy(base, tdev, L(base, tdev + 4 * (3136 + 8)))) {
      Call(sub_823066D0, ctx, base, frame, 0x824423B4, saved, tdev, 8, palette, uint64_t(1) << 23);
    } else {
      SetTexture(base, SetupRegs(ctx, saved), frame, 0x824423B4, tdev, 8, palette, uint64_t(1) << 23);
    }
    SetAddressMode(base, r28_31, frame, 0x824423C4, L(base, sh + 20), 0, 0);
    SetAddressMode(base, r28_31, frame, 0x824423D4, L(base, sh + 20), 8, 0);
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
    if (Busy(base, dev, L(base, dev + 4 * (3136 + 8)))) {
      r3 = Call(sub_823066D0, ctx, base, frame, 0x8244247C, saved, dev, 8, 0, 0x00800000);
    } else {
      r3 = SetTexture(base, SetupRegs(ctx, saved), frame, 0x8244247C, dev, 8, 0, 0x00800000);
    }
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
                             {ctx.r1.u32 - kStackBytes, kStackBytes, REXCVAR_GET(debug_native_simple_check_stack)}},
                            4};
  }
  decomp::Run(REXCVAR_GET(native_simple_setup), checking, g_stats, ctx, base,
              original, ranges, [&](PPCContext& c) { return Setup(c, base); });
}

}  // namespace decomp_simple
