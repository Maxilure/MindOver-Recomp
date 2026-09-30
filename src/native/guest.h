// =============================================================================
// native/guest.h -- reading the game's memory and objects from host code
// =============================================================================
//
// The game (PowerPC, big-endian) keeps all its data in "guest" memory: a 4 GB
// block the runtime maps into our process. A guest address A lives at host
// address `base + A`, where `base` is the pointer every recompiled function
// receives (and kernel_memory()->virtual_membase() elsewhere). Every value in
// there is big-endian, so each read swaps the bytes.
//
// This file also names the offsets inside the game's renderer objects that
// the native renderer reads (xnContext, xnShader, xnTexture, the D3D device).
// Each one comes with how we know it: they were read off the disassembly of
// the functions that use them (docs/findings/09-native-first-screens.md).
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>

namespace native::guest {

// --- Big-endian loads from a host pointer into guest memory ------------------

inline uint32_t Load32(const uint8_t* host) {
  uint32_t v;
  std::memcpy(&v, host, sizeof(v));
  return __builtin_bswap32(v);
}
inline uint8_t Load8(const uint8_t* host) { return *host; }
inline float LoadFloat(const uint8_t* host) {
  uint32_t v = Load32(host);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

// Same, from a guest address (base = guest memory base).
inline uint32_t Load32(const uint8_t* base, uint32_t address) { return Load32(base + address); }
inline uint8_t Load8(const uint8_t* base, uint32_t address) { return base[address]; }
inline float LoadFloat(const uint8_t* base, uint32_t address) { return LoadFloat(base + address); }

// The guest memory base (what recompiled functions receive as `base`), for
// code that isn't handed it.
const uint8_t* Base();

// The PHYSICAL address behind a guest address, for comparing addresses that
// come from different places. D3D objects hold virtual addresses; D3D's own
// copy of the GPU fetch constants (device::SamplerFetch) holds the physical
// ones the GPU uses. The Xbox 360 maps physical memory at fixed windows:
// 0xA0000000, 0xC0000000 and 0xE0000000 (the last one 4 KB further on:
// virtual 0xF4B9D000 is physical 0x14B9E000, seen in D3D's SetTexture).
// Anything else is returned as is.
inline uint32_t PhysicalAddress(uint32_t address) {
  if (address >= 0xE0000000u) return (address & 0x1FFFFFFFu) + 0x1000u;
  if (address >= 0xA0000000u) return address & 0x1FFFFFFFu;
  return address;
}

// Host pointer of a guest range if all of it is mapped and readable, else
// null. For data whose address comes from game state we don't fully control
// (texture memory), so a bad pointer can never fault inside our code.
// Physical memory (0xA0000000 and up) is always readable: it's read through
// the SDK's raw physical view, as the emulated GPU reads it.
const uint8_t* TranslateReadable(uint32_t address, uint32_t size);

// The reverse of PhysicalAddress: a VIRTUAL address through which the
// physical address `physical` can be read, or 0. Physical memory shows up in
// all three windows (0xA0000000, 0xC0000000, 0xE0000000 - 0x1000). (Each is
// tried in turn; since TranslateReadable reads physical memory through the
// raw view, the first one now always works.) Needed for textures known only from D3D's copy of the fetch
// constants (device::SamplerFetch), which holds physical addresses: our
// texture cache reads guest memory by virtual address (docs/findings/14).
uint32_t ReadableAddressOfPhysical(uint32_t physical);

// --- Offsets inside the game's renderer objects -------------------------------
// Class names come from the game's RTTI (docs/findings/08). "D3D" is
// Microsoft's Direct3D for the Xbox 360, statically linked into the game.

// xnContext: the drawing API object (vtable 0x82016744).
namespace context {
// pddiBaseContext's current state block. +4 clear colour (D3DCOLOR 0xAARRGGBB),
// +8 clear depth, +12 clear stencil. xnContext::Clear (0x824306A0) passes
// these to D3D Clear (depth as 1 - value: the game uses reversed Z).
constexpr uint32_t kState = 136;
// Render-state block, written by the xnContext setters (0x82430990...), which
// pass each value on to D3D (through the tables below where noted):
//   +4 cull mode (PDDI: 0 none, 1, 2; kCullTable)     SetCullMode? 0x82430990
//   +8 depth test on (byte)                             SetZTest? 0x82430A40
//   +12 depth compare (PDDI enum; kDepthCompareTable)   SetZCompare 0x82430A58
//   +16 depth write on (byte)                           SetZWrite? 0x82430A78
//   +24..+27 colour write on, one byte per R, G, B, A   pddiBaseContext::v58 0x8243ECF0
constexpr uint32_t kRenderState = 140;
// Fog block, written by SetFogEnable (0x82430C48) / SetFog (0x82430C98):
// +4 on (byte; also D3D's pixel shader bool b69), +8 colour (D3DCOLOR),
// +12 start, +16 end (distances). SetFog also copies them into the pixel
// shader constants the simple material reads: c70 colour, c71.x start,
// c72.x end.
constexpr uint32_t kFogState = 148;
// Stencil-state block (front faces), written by the xnContext stencil setters
// 0x82430AA0-0x82430B38 (through pddiBaseContext::v68 0x8243ED50 / v76
// 0x8243ED60 for compare and ops):
//   +4 enable (byte)          +8 compare (PDDI enum; kStencilCompareTable)
//   +12 reference             +16 read mask           +20 write mask
//   +24 / +28 / +32 ops on stencil fail / depth fail / pass (kStencilOpTable)
// The back-face (two-sided) stencil calls xnContext::v95 (ops), v96 (compare)
// and v97 (two-sided on/off) only reach D3D; the recorder keeps their values.
// The hub uses this for blob shadows: shadow volumes mark the stencil, then
// a full-screen 25% black quad darkens only the marked pixels.
constexpr uint32_t kStencilState = 152;
// The state cache shared by all xn shaders (the same object as their +20,
// shader::kStateCache): +232 = the material being drawn is two-sided, in
// which case xnContext::SetCullMode leaves D3D's cull mode at "none".
constexpr uint32_t kShaderStateCache = 380;
constexpr uint32_t kDefaultShader = 392;  // used by BeginPrims when passed null
constexpr uint32_t kDevice = 400;         // the D3D device (0x40005200 in practice)
// The D3D viewport, as xnContext::v122 (0x824319A8) computes it and hands it
// to D3D's SetViewport (0x8230D5D8): +404 x, +408 y, +412 width, +416
// height (integer pixels of the current render target), +420 MinZ, +424 MaxZ
// (floats). The constructor sets MinZ = 1, MaxZ = 0 (0x82432158): THIS is
// where the game's reversed depth comes from. Its projection matrices produce
// ordinary D3D depth (0 near, 1 far) and the viewport flips it, which is why
// Clear hands D3D 1 - depth and the compare table turns "less" into "greater".
constexpr uint32_t kViewport = 404;
constexpr uint32_t kPrimStream = 428;     // the immediate-mode xnPrimBufferStream

// PDDI cull mode -> D3D cull mode (table at 0x824EDCA0). D3D values:
// 0 none, 2 = cull clockwise triangles, 6 = cull counter-clockwise ones
// (as seen on screen). They're the GPU's PA_SU_SC_MODE_CNTL bits (2 = cull
// back faces, 4 = clockwise is the front), so they also decide which side is
// the front for two-sided stencil (native_renderer.cpp, ToFacing).
constexpr uint8_t kCullTable[4] = {0, 6, 2, 0};
// PDDI depth compare -> Xbox GPU compare function (table at 0x824EDCE4).
// Xbox numbering: 0 never, 1 less, 2 equal, 3 less-equal, 4 greater,
// 5 not-equal, 6 greater-equal, 7 always. PDDI's "less" (2) becomes
// "greater" because of the flipped viewport above.
constexpr uint8_t kDepthCompareTable[8] = {0, 7, 4, 6, 1, 3, 2, 5};
// PDDI stencil compare -> Xbox compare function (table at 0x824EDCC4): not
// reversed (stencil values have nothing to do with the depth direction).
constexpr uint8_t kStencilCompareTable[8] = {0, 7, 1, 3, 4, 6, 2, 5};
// PDDI stencil op -> Xbox stencil op (table at 0x824EDCAC). Xbox numbering:
// 0 keep, 1 zero, 2 replace, 3/4 increment/decrement clamped, 5 invert,
// 6/7 increment/decrement wrapping: the same as Vulkan's VkStencilOp.
constexpr uint8_t kStencilOpTable[6] = {0, 1, 2, 6, 7, 5};
}  // namespace context

// The immediate-mode vertex stream (filled by 0x824372E0, the stream's
// "begin", which BeginPrims calls). The game writes vertices straight into
// D3D's command buffer through these pointers between BeginPrims and EndPrims.
// The component pointers are set by "begin" in this order after the position
// (3 floats): normal (flag 0x10, 3 floats), colour (0x20, or 0x4000 with
// +60 colours), the texture coordinate sets (+64 of them, 2 floats each),
// binormal (0x800, 3 floats), tangent (0x1000, 3 floats).
namespace stream {
constexpr uint32_t kStride = 0;       // bytes per vertex
constexpr uint32_t kPosition = 4;     // write pointer; at "begin" = vertex 0
constexpr uint32_t kNormal = 8;       // 0 if the format has no normal
constexpr uint32_t kBinormal = 12;    // 0 if none
constexpr uint32_t kTangent = 16;     // 0 if none
constexpr uint32_t kColour = 20;      // 0 if no colour
constexpr uint32_t kUv = 24;          // 0 if no texture coordinates
// Indexed geometry (xnContext::BeginIndexedPrims, 0x824307F8): where the
// game writes its 16-bit indices, big-endian. D3D's BeginIndexedVertices
// (0x82477248, index format 1 = 16-bit) hands this pointer out; it stays
// valid until the next "begin" zeroes it. 0 for plain BeginPrims.
constexpr uint32_t kIndices = 32;
constexpr uint32_t kFormat = 56;      // PDDI vertex format flags (0x2021 = colour + 1 UV)
constexpr uint32_t kCount = 72;       // vertices announced by BeginPrims
}  // namespace stream

// The D3D device. D3D keeps a copy of every shader constant (float4) at
// +1920 + 16 * register: vertex shader c0..c255, then pixel shader c0..c255
// (found in D3D's SetVertexShaderConstantF, 0x82312998).
namespace device {
constexpr uint32_t kConstants = 1920;
constexpr uint32_t VsConstant(uint32_t reg) { return kConstants + 16 * reg; }
constexpr uint32_t PsConstant(uint32_t reg) { return kConstants + 16 * (256 + reg); }
// D3D's copy of the GPU texture fetch constants, 6 words per sampler at
// +1152 + 24 * sampler. SetTexture (0x823066D0) merges the texture's words
// with the sampler state (address modes, filters) kept here.
constexpr uint32_t SamplerFetch(uint32_t sampler) { return 1152 + 24 * sampler; }
// Pixel shader BOOLEAN constants b0..b127 (the GPU numbers them b128..b255,
// the vertex shader's come first), one bit each: b(n) is bit n % 32 (bit 0 =
// lowest) of the word at +10128 + 4 * (n / 32). From D3D's
// SetPixelShaderConstantB (0x82312B58). Used by the game's pixel shaders to
// switch whole sections on and off: b0-b3 directional lights, b10 reflect's
// "HasVertexColour" / water's "IsDisableReflection", b11 refraction,
// b12/b13 palettized textures (character), b69 fog, b80/b81 additive /
// subtractive blending.
constexpr uint32_t kPixelBools = 10128;
}  // namespace device

// xnShader (every material class derives from it).
namespace shader {
constexpr uint32_t kSimpleShaderVtable = 0x82017E64;  // xnSimpleShader
constexpr uint32_t kBinkShaderVtable = 0x820188D4;    // xnBinkShader (movies)
// A state cache shared by the xn shaders: the states last applied
// (xnShader's common-state function 0x8242DA28 fills it). +204 blend on,
// +208 blend op (Xenos enum), +212 source factor, +216 destination factor
// (PDDI numbering: D3D gets kBlendFactorTable[value]), +220 alpha test on,
// +224 alpha compare (Xenos enum).
constexpr uint32_t kStateCache = 20;
// PDDI blend factor -> Xenos blend factor (xenos::BlendFactor), the table at
// 0x824EE7D8 that 0x8242D94C / 0x8242D96C index before handing the factors
// to D3D (0x8230ABE8 / 0x8230AC78 write them into RB_BLENDCONTROL). Found
// with the shadow material (findings/12): 0, 1, 6, 7 map to themselves,
// which is why alpha blending looked right while multiply blends didn't.
//   PDDI: 0 zero, 1 one, 2 src colour, 3 1-src colour, 4 dst colour,
//         5 1-dst colour, 6 src alpha, 7 1-src alpha, 8 dst alpha,
//         9 1-dst alpha, 10 src alpha saturate
constexpr uint8_t kBlendFactorTable[11] = {0, 1, 4, 5, 8, 9, 6, 7, 10, 11, 16};
constexpr uint32_t kBlendMode = 24;      // PDDI blend mode (2 additive, 3 subtractive)
constexpr uint32_t kAlphaRef = 44;       // float 0..1 (D3D gets it x255)
constexpr uint32_t kLit = 56;            // byte: lit (vertex lighting) variant
// byte: two-sided material. xnShader's common-state function (0x8242DA28)
// sets D3D's cull mode to "none" for it, else re-applies the context's.
constexpr uint32_t kTwoSided = 84;
// xnSimpleShader only: its texture (an xnTexture, or 0). Set by the "TEX"
// parameter setter 0x82442490; read by the draw setup 0x82441EA8.
constexpr uint32_t kSimpleTexture = 92;
// xnBinkShader only: 3 xnTextures at +92/+96/+100, the movie's Y, Cr and Cb
// planes, bound to samplers 0/1/2 by its draw setup 0x82446C58.
constexpr uint32_t kBinkPlanes = 92;

// xnCharacterRimShader (vtable 0x82016294): Crash and the other characters.
// Its draw setup (vtable slot 14, 0x8242CD10) binds these xnTextures:
// +92 the diffuse texture (sampler 0), +96 the normal map (sampler 1),
// +104 a second texture the diffuse one is blended towards (sampler 3).
// If +136 is non-zero it also binds a texture the context holds (sampler 2,
// a copy of the screen) and turns on the pixel shader's refraction (bool
// b11): a see-through, glassy look. It then writes its colours straight into
// D3D's pixel shader constants (c2 fade, c64-c68 surface, c98-c105 pulse,
// blend, rim, refraction) and sets bools b11-b13.
constexpr uint32_t kCharacterRimShaderVtable = 0x82016294;
constexpr uint32_t kCharacterDiffuse = 92;
constexpr uint32_t kCharacterNormalMap = 96;
constexpr uint32_t kCharacterBlendMap = 104;
constexpr uint32_t kCharacterRefract = 136;

// xnShadowShader (vtable 0x82018C4C): the soft shadow under the characters
// (findings/12). Its draw setup (0x82448E90) resolves the depth/stencil
// surface into a texture (the context's 0x824311F0 names it), binds a second
// texture object aliasing the same memory as k_8_8_8_8 on sampler 0 (so the
// stencil byte reads as one colour channel), and loads its blur taps into
// pixel shader c2-c26 (offsets) and c30-c54 (weights) from a table in the
// game's data; the shader uses the first 9 of each.
constexpr uint32_t kShadowShaderVtable = 0x82018C4C;

// xnUndergroundShader (vtable 0x82016CEC): the brown dirt circle around Crash
// while he digs (found in a playtest, 2026-09-26). Its draw setup (slot 14,
// 0x82435390) is a line-for-line copy of xnSimpleShader's (0x82441EA8): same
// shader files (the exe lists the six simple .out names a second time, then
// "underground"), same offsets (+56 lit, +92 texture, +64..+92 colours into
// PS c64-c68, fade c2, textured c20). The one difference: AFTER the common
// states it forces blending on through the state cache's blend setter
// 0x8242D8C8 (enable, op add, PDDI src, dst), picked by the byte at +105:
//   0 -> zero, zero     1 -> one, one (additive)
//   2 -> dst alpha, 1 - dst alpha (mix by the alpha already in the picture)
//   3 -> zero, src alpha             other -> dst alpha, one
// The recorder reads the blend from the state cache after the setup, so the
// simple material's reader covers it as is.
constexpr uint32_t kUndergroundShaderVtable = 0x82016CEC;

// xnReflectShader (vtable 0x82018FAC): reflective, metal-looking surfaces
// (e.g. N. Gin's lab on Wumpa Island). Its draw setup
// (slot 14, 0x8244B430) binds:
//   +96  the diffuse xnTexture (sampler 0; palettized -> its palette too)
//   +100 the reflection xnTexture (sampler 1): a "sphere map", a picture of
//        the surroundings looked up by the surface's direction (setter 0x8244B2B8)
// then shaders from its own table (0x82597380: [0] reflectvp, [1] reflectfp,
// [2] palreflectfp), the lit materials' surface colours into PS c64-c68
// (+64..+92, as xnSimpleShader), fade PS c2.x, and:
//   +104 a D3DCOLOR 0xAARRGGBB -> PS c100 (the shader's "EnvColour", the
//        reflection's tint; constructor 0x8244B350 sets 0x80808080)
//   +92  a word -> PS bool b10 ("HasVertexColour", set by SetPixelShaderConstantB)
// It sets no b80/b81 (its shader has no additive fade) and no c20 (it always
// samples its texture). The shaders are described in shaders/reflect.frag.
constexpr uint32_t kReflectShaderVtable = 0x82018FAC;
constexpr uint32_t kReflectDiffuse = 96;
constexpr uint32_t kReflectEnvironment = 100;
constexpr uint32_t kReflectHasVertexColourBool = 10;  // PS bool b10

// xnParticleShader (vtable 0x82018B54): sparks, glows, splashes, dust
// (docs/findings/14). Drawn as immediate triangles (BeginPrims: position,
// colour, one UV). Its draw setup (slot 14, 0x82448158):
//   +92 its xnTexture on sampler 0 (palettized -> the palette on sampler 8
//       and the texture size in PS c112, like the simple material)
//   sampler 1: the scene's DEPTH, resolved into a texture by the context's
//       0x824310A0 (once per frame, on first use: a depth resolve, flags 0x14),
//       point-sampled: "soft particles" fade out where they meet the scene
//   shaders from its table 0x825971F8: [0] particleshadervp,
//       [1] particleshaderfp, [2] palparticleshaderfp
//   PS c2.x fade (the context's global fade x the material's +88),
//   PS c4 "Falloff" (4 floats from the game's data at 0x824F049C: .x = the
//       depth distance over which particles fade in), PS c20.x textured,
//   PS bools b80 / b81 "Additive" / "Subtractive" from the blend mode (+24).
// PS c73 "NearFar" (read with the depth, never set by the setup itself) turns
// a stored depth into a distance. The palette variant differs only by its
// four-texel palette lookup.
constexpr uint32_t kParticleShaderVtable = 0x82018B54;
constexpr uint32_t kParticleTexture = 92;
constexpr uint32_t kParticleDepthSampler = 1;  // D3D sampler of the depth copy

// xnWaterSpecularShader (vtable 0x82018E64): water surfaces (Wumpa Island's
// river, the waterfalls of the second area). Drawn as indexed immediate
// geometry (BeginIndexedPrims, format 0x2031: position, normal, colour, UV).
// Its draw setup (slot 14, 0x8244A1D0) binds NINE textures, one per sampler,
// which the pixel shader's constant table names:
//   0 DiffuseTex        +92
//   1 DistortMap        +96   (sampled at the UV and at the screen position)
//   2 BackGroundMap     the scene drawn so far: the context's 0x82430F28
//                       resolves colour target 0 right here (what the water
//                       shows through itself, "refraction")
//   3 DepthMap          the scene's depth (0x824310A0, as for particles)
//   4 NormalMap         +100
//   5 NormalTex0        animated normal map, frame i \  a 15-frame loop from
//   6 NormalTex1        frame i + 1               /  the table at 0x8258E420
//                       (getter 0x82431440), i from the game's clock
//   7 ReflectMap        the context's +1084 (a D3D texture: the planar
//                       reflection picture the frame renders first)
//   8 ReflectMask       +104
// Samplers 1-8 get fixed address/filter modes, sampler 0 the material's. We
// read all nine from D3D's copy of the fetch constants (what the GPU samples).
// Shaders from the table 0x82597378: [0] waterspecularvp, [1] waterspecularfp.
// Constants (names from the pixel shader's constant table):
//   PS c2.x Fade, c23.x PixelHeight (1 / screen height; c22.x = 1 / width),
//   c25.x Blend (between the two animation frames),
//   c64-c67 the surface colours +64..+76 (unused by this pixel shader),
//   c100 ReflectBumpiness (+112..+124, .x used), c101 WaterBumpiness
//   (+116..+128, .x used), c102 ReflectColor (+108, 0xAARRGGBB -> RGBA),
//   c128 CameraPos / c129 ViewDirection (set by the context, not here),
//   c200-c208 Fx shadows, c70-c72 fog, c73 NearFar,
//   bool b10 IsDisableReflection (+132), b69 fog;
//   VS c128.x a time (declared by the vertex shader, unused).
constexpr uint32_t kWaterSpecularShaderVtable = 0x82018E64;
constexpr uint32_t kWaterSamplers = 9;
constexpr uint32_t kWaterBackgroundSampler = 2;
constexpr uint32_t kWaterDepthSampler = 3;
constexpr uint32_t kWaterReflectMapSampler = 7;
constexpr uint32_t kWaterReflectMaskSampler = 8;
constexpr uint32_t kWaterDisableReflectionBool = 10;  // PS bool b10

// xnDOFShader (vtable 0x82018D2C): depth of field, the blur of what's far
// from the camera's focus (docs/findings/15). Not a material on scenery: the
// post-processing pass draws ONE full-screen quad with it, on the post
// surface, after the scene texture was drawn back there and before the HUD.
// FramebufferFxExt::v30 (0x82440A50) is where the game hands over the frame's
// settings: it stores them into this shader, the one at FramebufferFxExt+92
// -> +16, then draws the quad (0x824406A8: BeginPrims, triangle strip,
// format 0x2021, 4 vertices = the whole screen, UV 0..1):
//   +92  byte: near blur on = NearBlurPlaneDistance > 0.001 (0x82009CC4)
//   +96  FocalPlaneDistance      (v30's f1; default 1)
//   +100 NearBlurPlaneDistance   (f2; default 0)
//   +104 FarBlurPlaneDistance    (f3; default 1000)
//   +108 MaxRadius, two floats   (r7 as a pair; default 1, 2)
// (Defaults from the constructor 0x824494F0.) Its draw setup (slot 14,
// 0x824495D8):
//   sampler 0 "Framebuffer": the post surface drawn so far, resolved by the
//       context's 0x82430F28 right here (the scene, sharp)
//   sampler 1 "Depthbuffer": the scene's depth (0x824310A0, the copy the
//       particles already made this frame)
//   both sampled with the fixed modes the state cache calls at the end set
//   shaders from its table 0x82597358: [0] DOFvp, [1] DOF (pixel)
//   PS c0 = 0.25 / screen size (unused by the pixel shader), c1
//   "PixelSizeHigh" = 1 / screen size (x = 1/width), and four overlapping
//   windows onto +96..+123: c2 = +96.. (FocalPlaneDistance, .x used),
//   c3 = +100.. (NearBlurPlaneDistance), c4 = +104.. (FarBlurPlaneDistance),
//   c5 = +108.. (MaxRadius, .xy used); bool b10 "bEnableNearBlur" = +92.
//   PS c73 "NearFar" turns a stored depth into a distance (set by the
//   context, as for particles and water).
// Names from the pixel shader's constant table; the vertex shader's debug
// file shows it outputs the UV and the clip position.
constexpr uint32_t kDofShaderVtable = 0x82018D2C;
constexpr uint32_t kDofFramebufferSampler = 0;
constexpr uint32_t kDofDepthSampler = 1;
constexpr uint32_t kDofEnableNearBlurBool = 10;  // PS bool b10

// xnFxGridShader (vtable 0x82016B1C): full-screen "grid" effects, e.g. the
// brightness flash while the double mojo power-up is collecting mojo
// (docs/findings/18). Found from a user report + an F10 trace
// (tools/play.sh --trace): in the post pass, after the scene was drawn back
// onto the post surface, the game resolves that surface into a texture
// (0x8242FEC4), then draws ~40 immediate batches of 160 vertices (format
// 0x2021: position, colour, uv; caller 0x8237CAC8) covering the screen, each
// through this material, BEFORE the depth of field and the HUD.
// Its draw setup (slot 14, 0x82434A08):
//   sampler 0 = that copy of the post surface (the texture 0x8242FED8
//       returns; the setup ORs 0x80000000 into SetTexture's flags)
//   shaders from its table 0x8258E468: [0] fxgridvp, [1] fxgrid (pixel)
//   PS bool b10 = the game's global 0x8258E474 (add or multiply, below)
//   then the common state (0x8242DA28: blend from the state cache; the
//   flash blends src alpha / 1 - src alpha)
// fxgridvp (debug file): position x matMVP (c4-c7), colour and uv passed on
// unchanged (no tint). fxgrid (microcode only, --dump_shaders):
//   copy = tex2D(s0, uv)
//   rgb  = b10 ? copy.rgb + colour.rgb : copy.rgb * colour.rgb
//   a    = colour.a
// With src-alpha blending over the surface it just copied, that's "the
// picture + colour x alpha": an even glow (add) or a tint (multiply).
constexpr uint32_t kFxGridShaderVtable = 0x82016B1C;
constexpr uint32_t kFxGridScreenSampler = 0;
constexpr uint32_t kFxGridAddBool = 10;  // PS bool b10: add the colour (else multiply)

// xnBumpMegaShader (vtable 0x82016E1C): the "do everything" normal-mapped
// surface (the TK blocks, see-through ice; docs/findings/20).
// Found by the area scan (scan.h) in playtests; register names from
// its pixel shader's constant table (tools/shader_constants.py).
// Its draw setup (slot 14, 0x82436308) binds, as the GPU samples them:
//   s0 DiffuseTex (+92), s1 NormalMap (+96), s2 ReflectMap, s3 ReflectMask,
//   s4 BackGroundMap = the picture so far, resolved INSIDE the setup by the
//       context's 0x82430F28 (all three tiling strips, like water's copy),
//   s7 Palette / s8 Palette2 (the diffuse's / normal map's palettes when
//       palettized; null otherwise)
// and PS bools b10 IsEnableRim, b11 IsEnableRefract, b12 IsEnableReflect,
// b13 IsUseReflectMask, b14 IsPaletteTexture, b15 IsPaletteTexture2, b16
// IsRefractColourModulated (b10-b12 and b16 from the shader at +164/+168/
// +172/+176, the others worked out from the textures), then the common
// state (blend from the state cache: the first one met blends src alpha /
// 1 - src alpha). Constants: PS c2 Fade, c70-c72 fog, c100 RimColor, c101
// ReflectColor, c103 RefractBumpiness, c104 ReflectBumpiness, c105
// TileWidth, c106 RefractBrightness, c112/c113 BilinearScale(2) (the
// palettized textures' sizes), c128 CameraPos, c200-c208 Fx shadows; VS
// (bumpmegavp, debug file) c0 matRot, c4 matMVP, c12 matWorld.
// Meshes, format with normal, colour, uv, binormal and tangent (60 bytes).
constexpr uint32_t kBumpMegaShaderVtable = 0x82016E1C;
constexpr uint32_t kBumpMegaDiffuseSampler = 0;
constexpr uint32_t kBumpMegaNormalSampler = 1;
constexpr uint32_t kBumpMegaReflectSampler = 2;
constexpr uint32_t kBumpMegaMaskSampler = 3;
constexpr uint32_t kBumpMegaBackgroundSampler = 4;
constexpr uint32_t kBumpMegaPaletteSampler = 7;
constexpr uint32_t kBumpMegaPalette2Sampler = 8;
constexpr uint32_t kBumpMegaRimBool = 10;
constexpr uint32_t kBumpMegaRefractBool = 11;
constexpr uint32_t kBumpMegaReflectBool = 12;
constexpr uint32_t kBumpMegaReflectMaskBool = 13;
constexpr uint32_t kBumpMegaPaletteBool = 14;
constexpr uint32_t kBumpMegaPalette2Bool = 15;
constexpr uint32_t kBumpMegaRefractModulatedBool = 16;
}  // namespace shader

// xnTexture (vtable 0x820171EC).
namespace texture {
constexpr uint32_t kD3DTexture = 20;      // the D3D 2D texture object
constexpr uint32_t kPalettes = 24;        // 16 D3D palette textures (a ring)...
constexpr uint32_t kPaletteIndex = 88;    // ...and the one in use
constexpr uint32_t kPaletteMode = 276;    // 1 = 8-bit palettized (0x8243AE08)
}  // namespace texture

// A D3D texture object: a 28-byte resource header, then the GPU texture fetch
// constant (6 words: format, size, tiling, address...). D3D's SetTexture
// (0x823066D0) copies those words from +28 to the GPU.
namespace d3d_texture {
constexpr uint32_t kFetchConstant = 28;
}

// xnPrimBuffer (vtable 0x82017F64): a static mesh, i.e. a D3D vertex buffer
// plus an optional 16-bit index buffer, filled once at load time through
// its Lock/Unlock methods. Offsets read off its methods: the constructor
// 0x82443D98, the setup 0x82443E20 (which builds the vertex declaration from
// the PDDI format flags with 0x8243CF28) and Draw 0x82443BD0.
namespace prim_buffer {
constexpr uint32_t kDevice = 12;         // the D3D device
constexpr uint32_t kPrimType = 16;       // Xbox GPU primitive type: 1 points,
                                         // 2 lines, 3 line strip, 4 triangles, 6 strip
constexpr uint32_t kFormat = 20;         // PDDI vertex format flags (see VertexLayout)
constexpr uint32_t kStride = 24;         // bytes per vertex
constexpr uint32_t kVertexCount = 28;    // vertices drawn when there are no indices
constexpr uint32_t kIndexCapacity = 32;  // indices the index buffer holds
constexpr uint32_t kIndexCount = 36;     // indices drawn; 0 = not indexed
constexpr uint32_t kCountOverride = 40;  // if non-zero, Draw draws this many instead
constexpr uint32_t kIndexBuffer = 44;    // D3D index buffer (0 = none)
constexpr uint32_t kVertexBuffer = 48;   // D3D vertex buffer
constexpr uint32_t kDeclaration = 52;    // D3D vertex declaration
}  // namespace prim_buffer

// A D3D surface object (a render target in EDRAM, D3D's SetRenderTarget
// 0x8230D708): a 24-byte resource header, then the GPU registers that
// describe it. Read off D3D's code and checked against the 4 surfaces of a hub
// frame (docs/findings/10):
//   +24 RB_SURFACE_INFO: bits 0-13 pitch in pixels, 16-17 MSAA samples
//       (0 = 1x, 1 = 2x, 2 = 4x)
//   +28 RB_COLOR_INFO (colour surfaces: bits 0-11 EDRAM base in tiles, 16-19
//       format) or RB_DEPTH_INFO (depth: bit 16 format, 0 = 24-bit fixed,
//       1 = 24-bit float)
//   +36 size: (width - 1) << 18 | (height - 1) << 3
//   +40 the D3DFORMAT (0x18280186 = A8R8G8B8, 0x1A220197 = D24FS8)
//   +44 bytes of EDRAM it covers (width x height x 4 x samples)
// The hub's main surface is only 480 x 720 (2x MSAA): the EDRAM tile the
// game renders its 1280 x 720 picture through in 3 strips (predicated
// tiling, findings/06). The recorder sizes such surfaces by the tiling
// rectangles instead (D3D's BeginTiling 0x823193D0).
namespace d3d_surface {
constexpr uint32_t kSurfaceInfo = 24;
constexpr uint32_t kTargetInfo = 28;
constexpr uint32_t kSize = 36;
}  // namespace d3d_surface

// A D3D vertex buffer object: a 24-byte resource header, then the GPU's
// 2-word VERTEX fetch constant, which D3D's SetStreamSource (0x8230CE58)
// hands to the GPU: word 0 = address (virtual here) | 3 (the "vertex" type),
// word 1 = size in 32-bit words << 2 | endian mode (how the GPU swaps bytes).
namespace d3d_vertex_buffer {
constexpr uint32_t kFetchConstant = 24;
}

// A D3D index buffer object: a 24-byte resource header, then the address of
// its indices (virtual) and their size in bytes (D3D's creation function
// 0x823071B8 and its Lock 0x82307268). The game's are always 16-bit.
namespace d3d_index_buffer {
constexpr uint32_t kAddress = 24;
constexpr uint32_t kSize = 28;
}

}  // namespace native::guest
