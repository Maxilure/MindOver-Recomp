// =============================================================================
// native/recorder.cpp -- see recorder.h for the why and how
// =============================================================================
//
// Every offset read here is named and explained in guest.h. How each call's
// arguments were worked out (disassembly of the game's functions) is in
// docs/findings/09-native-first-screens.md (2D screens) and
// docs/findings/10-native-3d-world.md (meshes and 3D state),
// docs/findings/11-native-characters.md (characters, lit materials),
// docs/findings/13-native-reflections.md (the reflect material),
// docs/findings/14-native-water-particles.md (particles, water),
// docs/findings/15-native-depth-of-field.md (depth of field).
// =============================================================================

#include "recorder.h"
#include "../guest_memory.h"
#include "spotter.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "../pddi/intercept.h"
#include "ab_capture.h"
#include "guest.h"
#include "scan.h"

namespace native {

// frame.h: a mesh vertex's layout from its PDDI format flags, in the order
// the game's declaration builder (0x8243CF28) lays the components out.
VertexLayout MeshVertexLayout(uint32_t flags, uint32_t stride) {
  VertexLayout l;
  l.stride = uint8_t(std::min<uint32_t>(stride, 255));
  uint32_t offset = 12;              // position, 3 floats
  if (flags & 0x100) offset += 12;   // second position
  if (flags & 0x80) offset += 4;     // blend indices
  if (flags & 0x10) {                // normal
    l.normal = uint8_t(offset);
    offset += 12;
  }
  if (flags & 0x20) {                // colour 0
    l.colour = uint8_t(offset);
    offset += 4;
  }
  if (flags & 0x4000) {              // colours 0 and 1
    if (l.colour == VertexLayout::kAbsent) l.colour = uint8_t(offset);
    offset += 8;
  }
  if (flags & 0x40) offset += 4;     // colour 1
  if (flags & 0xF) {                 // texture coordinate sets (the first one is used)
    l.uv = uint8_t(offset);
    offset += 8 * (flags & 0xF);
  }
  if (flags & 0x200) offset += 4;    // two single floats
  if (flags & 0x400) offset += 4;
  if (flags & 0x800) {               // binormal
    l.binormal = uint8_t(offset);
    offset += 12;
  }
  if (flags & 0x1000) {              // tangent
    l.tangent = uint8_t(offset);
  }
  return l;
}

namespace recorder {

namespace {

using namespace native::guest;

// Recorder state. Main thread only (see recorder.h).
struct State {
  bool recording = false;
  Frame frame;
  uint64_t frame_counter = 0;
  // The D3D surfaces bound right now (render target 0, depth/stencil).
  Targets targets;
  // Inside xnContext::EndFrame: its resolve is the frontbuffer one (or its
  // resolves: while tiling, one per strip, e.g. during the movies).
  bool in_end_frame = false;
  bool frontbuffer_seen = false;  // this EndFrame already resolved into it
  // Surfaces the game renders through in strips, and the full size they
  // stand for (the union of the tiling rectangles), kept across frames.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> tiled_sizes;
  // Clears requested by resolves, held back until the run of resolves ends
  // (FlushResolveClears): see OnD3DResolve.
  std::vector<ClearCommand> resolve_clears;
  // Back-face stencil (xnContext::v95/v96/v97 only tell D3D, see guest.h):
  // Xbox compare function and fail / depth-fail / pass ops.
  bool stencil_two_sided = false;
  uint8_t back_func = 7, back_ops[3] = {0, 0, 0};

  // Between BeginPrims (or BeginIndexedPrims) and EndPrims: the draw being
  // built, where its vertices are in guest memory, and the stream (whose
  // index pointer is read at EndPrims).
  bool pending = false;
  DrawCommand pending_draw;
  uint32_t pending_vertices = 0;  // guest address of vertex 0
  uint32_t pending_stream = 0;

  // The MaterialBlock of the draw being read (ReadMaterial fills it for the
  // lit, shadow, particle, water and depth of field materials); CommitDraw
  // appends it to the frame with the draw.
  MaterialBlock block{};

  // Inside xnContext::DrawPrimBuffer: the context and material its
  // xnPrimBuffer::Draw calls (one per material pass) draw with.
  uint32_t mesh_context = 0;
  uint32_t mesh_material = 0;

  // Things the native renderer can't draw yet, each reported once.
  std::unordered_set<std::string> reported;
} g;

void ReportOnce(const std::string& what) {
  if (g.reported.insert(what).second) {
    REXLOG_INFO("NativeRenderer: not drawn natively yet: {}", what);
    scan::NewGap(what);  // with --trace: record this spot (scan.h)
  }
}

void ReadFetchConstant(const uint8_t* base, uint32_t d3d_texture, GuestTexture& out) {
  for (uint32_t i = 0; i < 6; ++i) {
    out.fetch[i] = Load32(base, d3d_texture + d3d_texture::kFetchConstant + 4 * i);
  }
}

// An xnTexture's D3D texture, and its palette if it's 8-bit palettized
// (guest.h, texture). False if it has no plain 2D D3D texture.
bool ReadXnTexture(const uint8_t* base, uint32_t xn_texture, GuestTexture& texture,
                   GuestTexture& palette) {
  const uint32_t d3d = Load32(base, xn_texture + texture::kD3DTexture);
  if (!d3d) {
    ReportOnce("texture without a D3D 2D texture (cube/volume/render target?)");
    return false;
  }
  ReadFetchConstant(base, d3d, texture);
  if (Load32(base, xn_texture + texture::kPaletteMode) == 1) {
    const uint32_t index = Load32(base, xn_texture + texture::kPaletteIndex) & 15;
    if (const uint32_t pal = Load32(base, xn_texture + texture::kPalettes + 4 * index)) {
      ReadFetchConstant(base, pal, palette);
    }
  }
  return true;
}

// The texture on sampler `s` as D3D's copy of the fetch constants holds it
// (guest.h, device::SamplerFetch): for textures the material doesn't keep a
// pointer to (the water's animation frames, the context's reflection
// picture) or that the game made this frame (depth and colour resolves).
// That copy holds PHYSICAL addresses; the address is swapped for a virtual
// one our texture cache can read, where there is one (resolved textures are
// looked up by physical address anyway, render_targets.h). False (and `out`
// empty) if nothing valid is bound: fetch constant type 2 = texture.
bool ReadDeviceTexture(const uint8_t* base, uint32_t dev, uint32_t s, GuestTexture& out) {
  for (uint32_t i = 0; i < 6; ++i) {
    out.fetch[i] = Load32(base, dev + device::SamplerFetch(s) + 4 * i);
  }
  if ((out.fetch[0] & 3) != 2 || !out.present()) {
    out = GuestTexture{};
    return false;
  }
  if (const uint32_t address = ReadableAddressOfPhysical(out.fetch[1] & 0xFFFFF000u)) {
    out.fetch[1] = address | (out.fetch[1] & 0xFFFu);
  }
  // The mip levels' address (word 5, bits 12-31) likewise (texture_cache.h).
  if (out.fetch[5] >> 12) {
    if (const uint32_t address = ReadableAddressOfPhysical(out.fetch[5] & 0xFFFFF000u)) {
      out.fetch[5] = address | (out.fetch[5] & 0xFFFu);
    }
  }
  return true;
}

// How sampler `s` samples, as D3D programmed it (guest.h, device): the
// fields of xenos::xe_gpu_texture_fetch_t (frame.h, SamplerState):
//   word 0: bits 10-12 / 13-15 address mode X / Y
//   word 3: bits 19-20 magnification filter, 21-22 minification, 23-24 mip
//           (xenos::TextureFilter: 0 point, 1 linear, 2 "base map" = the
//           base level only), 25-27 anisotropy (xenos::AnisoFilter: 0 off,
//           1-5 = at most 1, 2, 4, 8, 16 samples)
//   word 4: bits 2-5 the most detailed mip level allowed, 12-21 the level
//           bias (signed, 1/32 of a level)
// As the emulator does (the SDK's VulkanTextureCache::GetSamplerParameters),
// anisotropic filtering implies linear minification, magnification and mips.
SamplerState ReadSampler(const uint8_t* base, uint32_t dev, uint32_t s) {
  const uint32_t word0 = Load32(base, dev + device::SamplerFetch(s));
  const uint32_t word3 = Load32(base, dev + device::SamplerFetch(s) + 12);
  const uint32_t word4 = Load32(base, dev + device::SamplerFetch(s) + 16);
  SamplerState state;
  state.address_u = uint8_t((word0 >> 10) & 7);
  state.address_v = uint8_t((word0 >> 13) & 7);
  state.linear = ((word3 >> 19) & 3) == 1;
  state.min_linear = ((word3 >> 21) & 3) == 1;
  const uint32_t mip_filter = (word3 >> 23) & 3;
  state.mip = mip_filter == 1 ? 2 : mip_filter == 0 ? 1 : 0;
  const uint32_t aniso = (word3 >> 25) & 7;
  state.anisotropy = aniso >= 1 && aniso <= 5 ? uint8_t(1u << (aniso - 1)) : 0;
  if (state.anisotropy) {
    state.linear = state.min_linear = true;
    state.mip = 2;
  }
  state.min_level = uint8_t((word4 >> 2) & 15);
  state.lod_bias = int16_t(int32_t(word4 << 10) >> 22);  // bits 12-21, sign-extended
  return state;
}

// A pixel shader boolean constant (guest.h, device::kPixelBools).
bool PixelBool(const uint8_t* base, uint32_t dev, uint32_t n) {
  return ((Load32(base, dev + device::kPixelBools + 4 * (n / 32)) >> (n % 32)) & 1) != 0;
}

// `count` float4 shader constants from D3D's copy, starting at byte offset
// `constant` of the device (device::VsConstant / PsConstant).
void ReadConstants(const uint8_t* base, uint32_t dev, uint32_t constant, float* out,
                   uint32_t count = 1) {
  for (uint32_t i = 0; i < 4 * count; ++i) {
    out[i] = LoadFloat(base, dev + constant + 4 * i);
  }
}

// The unlit simple material's proximity lights and Fx shadows (docs/findings/20).
// Its pixel shaders (simpleshaderunlitfp, palsimpleshaderunlitfp) have the
// same sections as the lit one: c29.x = number of proximity lights, c30-c41
// their positions / colours / parameters, c200.x = number of Fx shadows,
// c201-c208 their positions. Most draws have both off; the game switches a
// light on for effects, e.g. the green flash when the Ratcicle roars (the
// game's light slot 4, set from 0x8237F300 only in those frames). Only then
// does the draw get a block: returns false (nothing to read) otherwise.
// Only world, point_*, shadow_spheres and counts.yz of the block are used
// (shaders/simple.frag with kFlagLights).
bool ReadSimpleLights(const uint8_t* base, uint32_t dev, LitParams& p) {
  auto count = [&](uint32_t reg, uint32_t max) {
    const float f = LoadFloat(base, dev + device::PsConstant(reg));
    return f > 0.0f ? std::min(uint32_t(f + 0.5f), max) : 0u;
  };
  const uint32_t lights = count(29, 4);
  const uint32_t shadows = count(200, 8);
  if (!lights && !shadows) {
    return false;
  }
  p = LitParams{};
  ReadConstants(base, dev, device::VsConstant(12), p.world, 4);  // "matWorld"
  p.counts[1] = lights;
  ReadConstants(base, dev, device::PsConstant(30), &p.point_position[0][0], 4);
  ReadConstants(base, dev, device::PsConstant(34), &p.point_colour[0][0], 4);
  ReadConstants(base, dev, device::PsConstant(38), &p.point_params[0][0], 4);
  p.counts[2] = shadows;
  ReadConstants(base, dev, device::PsConstant(201), &p.shadow_spheres[0][0], 8);
  return true;
}

// The lighting constants of the lit materials (frame.h, LitParams), from
// D3D's copy, right after the game set up the draw. Which register holds
// what comes from the game's shaders: the lit simple vertex shader's debug
// file names c0-c16, and the constant tables of the pixel shaders
// (simpleshaderlitfp, characterrimfp, reflectfp) name the rest
// (docs/findings/11 and 13).
void ReadLitParams(const uint8_t* base, uint32_t dev, const DrawCommand& d, LitParams& p) {
  const bool character = d.material == Material::kCharacter;
  const bool reflect = d.material == Material::kReflect;
  p = LitParams{};
  std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
  ReadConstants(base, dev, device::VsConstant(12), p.world, 4);
  ReadConstants(base, dev, device::VsConstant(0), p.rotation, 4);
  // The camera: the lit vertex shader gets it in c8, the reflect one in c9
  // (its debug file says so), the character's pixel shader in c128.
  ReadConstants(base, dev,
                character ? device::PsConstant(128)
                          : device::VsConstant(reflect ? 9 : 8),
                p.eye);
  ReadConstants(base, dev, device::PsConstant(8), p.ambient);
  for (uint32_t i = 0; i < 4; ++i) {
    ReadConstants(base, dev, device::PsConstant(9 + 2 * i), p.light_direction[i]);
    ReadConstants(base, dev, device::PsConstant(10 + 2 * i), p.light_colour[i]);
    p.counts[0] |= PixelBool(base, dev, i) ? 1u << i : 0u;
  }
  ReadConstants(base, dev, device::PsConstant(64), p.surface_diffuse);
  ReadConstants(base, dev, device::PsConstant(65), p.surface_specular);
  ReadConstants(base, dev, device::PsConstant(66), p.surface_emissive);
  ReadConstants(base, dev, device::PsConstant(67), p.surface_ambient);
  ReadConstants(base, dev, device::PsConstant(68), p.specular_power);
  // Counts are floats the shaders compare with 1, 2, 3...
  auto count = [&](uint32_t reg, uint32_t max) {
    const float f = LoadFloat(base, dev + device::PsConstant(reg));
    return f > 0.0f ? std::min(uint32_t(f + 0.5f), max) : 0u;
  };
  if (!character && !reflect) {  // only the lit simple shader has point lights
    p.counts[1] = count(29, 4);
    ReadConstants(base, dev, device::PsConstant(30), &p.point_position[0][0], 4);
    ReadConstants(base, dev, device::PsConstant(34), &p.point_colour[0][0], 4);
    ReadConstants(base, dev, device::PsConstant(38), &p.point_params[0][0], 4);
  }
  if (!reflect) {  // the reflect shader has no Fx shadows either
    p.counts[2] = count(200, 8);
    ReadConstants(base, dev, device::PsConstant(201), &p.shadow_spheres[0][0], 8);
  }
  if (reflect) {
    // Its reflection tint ("EnvColour"), and whether it uses the vertex
    // colour (guest.h, shader::kReflectShaderVtable).
    ReadConstants(base, dev, device::PsConstant(100), p.rim_colour);
    p.counts[3] |= PixelBool(base, dev, shader::kReflectHasVertexColourBool) ? kLitVertexColour
                                                                             : 0u;
  }
  if (character) {
    ReadConstants(base, dev, device::PsConstant(100), p.rim_colour);
    p.character[0] = LoadFloat(base, dev + device::PsConstant(98));   // pulse
    p.character[1] = LoadFloat(base, dev + device::PsConstant(99));   // blend weight
    p.character[2] = LoadFloat(base, dev + device::PsConstant(101));  // rim blend
    p.character[3] = LoadFloat(base, dev + device::PsConstant(102));  // rim amount
  }
  std::memcpy(p.fog_colour, d.fog_colour, sizeof(d.fog_colour));
  p.fog_fade_alpha[0] = d.fog_start;
  p.fog_fade_alpha[1] = d.fog_end;
  p.fog_fade_alpha[2] = d.fade;
  p.fog_fade_alpha[3] = d.alpha_ref;
  p.counts[3] |= (d.textured ? kLitTextured : 0) |
                (d.textured && d.palette.present() ? kLitPalettized : 0) |
                (d.normal_palette.present() ? kLitNormalPalettized : 0) |
                (d.fog ? kLitFog : 0) | (d.fade_rgb ? kLitFadeRgb : 0) |
                (d.alpha_test ? kLitAlphaTest : 0) |
                (d.blend_map.present() ? kLitBlendMap : 0) |
                (character && !d.normal_map.present() ? kLitNoNormalMap : 0);
  p.more[0] = d.alpha_func;
}

// The particle material's constants (frame.h, ParticleParams; registers from
// its pixel shader's constant table, docs/findings/14). The flags that
// depend on the vertex layout are added by CommitDraw.
void ReadParticleParams(const uint8_t* base, uint32_t dev, const DrawCommand& d,
                        ParticleParams& p) {
  p = ParticleParams{};
  std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
  p.fade_falloff_near_far[0] = d.fade;
  p.fade_falloff_near_far[1] = LoadFloat(base, dev + device::PsConstant(4));   // Falloff
  p.fade_falloff_near_far[2] = LoadFloat(base, dev + device::PsConstant(73));  // NearFar
  p.fade_falloff_near_far[3] = LoadFloat(base, dev + device::PsConstant(73) + 4);
  std::memcpy(p.fog_colour, d.fog_colour, sizeof(d.fog_colour));
  p.fog_colour[3] = d.alpha_ref;
  p.fog_range_texture_size[0] = d.fog_start;
  p.fog_range_texture_size[1] = d.fog_end;
  p.flags[0] = (d.textured ? kParticleTextured : 0) |
               (d.textured && d.palette.present() ? kParticlePalettized : 0) |
               (d.fade_rgb ? kParticleFadeRgb : 0) | (d.fog ? kParticleFog : 0) |
               (d.alpha_test ? kParticleAlphaTest : 0) |
               (d.depth_copy.present() ? kParticleSoft : 0);
  p.flags[1] = d.alpha_func;
}

// The water material's constants (frame.h, WaterParams; guest.h,
// shader::kWaterSpecularShaderVtable for where each comes from).
void ReadWaterParams(const uint8_t* base, uint32_t dev, const DrawCommand& d, WaterParams& p) {
  p = WaterParams{};
  std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
  ReadConstants(base, dev, device::VsConstant(12), p.world, 4);
  ReadConstants(base, dev, device::PsConstant(128), p.camera);
  ReadConstants(base, dev, device::PsConstant(129), p.view_direction);
  ReadConstants(base, dev, device::PsConstant(102), p.reflect_colour);
  p.bumpiness_blend_pixel[0] = LoadFloat(base, dev + device::PsConstant(100));  // ReflectBumpiness
  p.bumpiness_blend_pixel[1] = LoadFloat(base, dev + device::PsConstant(101));  // WaterBumpiness
  p.bumpiness_blend_pixel[2] = LoadFloat(base, dev + device::PsConstant(25));   // Blend
  p.bumpiness_blend_pixel[3] = LoadFloat(base, dev + device::PsConstant(23));   // PixelHeight
  ReadConstants(base, dev, device::PsConstant(73), p.near_far);
  std::memcpy(p.fog_colour, d.fog_colour, sizeof(d.fog_colour));
  p.fog_fade[0] = d.fog_start;
  p.fog_fade[1] = d.fog_end;
  p.fog_fade[2] = d.fade;
  // Fx shadows: the count is a float the shader compares with 1, 2, 3...
  const float shadows = LoadFloat(base, dev + device::PsConstant(200));
  p.flags[1] = shadows > 0.0f ? std::min(uint32_t(shadows + 0.5f), 8u) : 0u;
  ReadConstants(base, dev, device::PsConstant(201), &p.shadow_spheres[0][0], 8);
  // The reflection needs its picture and its mask; with either missing the
  // GPU would fetch zeros (an invalid fetch constant), which switches the
  // reflection off in the shader's own arithmetic.
  const bool reflection =
      !PixelBool(base, dev, shader::kWaterDisableReflectionBool) &&
      d.device_textures[shader::kWaterReflectMapSampler].present() &&
      d.device_textures[shader::kWaterReflectMaskSampler].present();
  p.flags[0] = (reflection ? kWaterReflection : 0) | (d.fog ? kWaterFog : 0);
}

// The bump mega material's constants and switches (frame.h, BumpMegaParams;
// guest.h, shader::kBumpMegaShaderVtable for where each comes from). A
// switch whose texture isn't bound is turned off here: the GPU would fetch
// zeros for it, which (in the shader's arithmetic) leaves the surface as
// if the effect were off, except a see-through look without its copy, which
// would darken it; we draw it opaque instead.
void ReadBumpMegaParams(const uint8_t* base, uint32_t dev, const DrawCommand& d,
                        BumpMegaParams& p) {
  p = BumpMegaParams{};
  std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
  ReadConstants(base, dev, device::VsConstant(12), p.world, 4);    // matWorld
  ReadConstants(base, dev, device::VsConstant(0), p.rotation, 4);  // matRot
  ReadConstants(base, dev, device::PsConstant(128), p.camera);     // CameraPos
  ReadConstants(base, dev, device::PsConstant(100), p.rim_colour);
  ReadConstants(base, dev, device::PsConstant(101), p.reflect_colour);
  p.bump[0] = LoadFloat(base, dev + device::PsConstant(103));  // RefractBumpiness
  p.bump[1] = LoadFloat(base, dev + device::PsConstant(104));  // ReflectBumpiness
  p.bump[2] = LoadFloat(base, dev + device::PsConstant(105));  // TileWidth
  p.bump[3] = LoadFloat(base, dev + device::PsConstant(106));  // RefractBrightness
  p.palette_sizes[0] = LoadFloat(base, dev + device::PsConstant(112));  // BilinearScale
  p.palette_sizes[1] = LoadFloat(base, dev + device::PsConstant(112) + 4);
  p.palette_sizes[2] = LoadFloat(base, dev + device::PsConstant(113));  // BilinearScale2
  p.palette_sizes[3] = LoadFloat(base, dev + device::PsConstant(113) + 4);
  std::memcpy(p.fog_colour, d.fog_colour, sizeof(d.fog_colour));
  p.fog_fade[0] = d.fog_start;
  p.fog_fade[1] = d.fog_end;
  p.fog_fade[2] = d.fade;
  p.fog_fade[3] = d.alpha_ref;
  p.flags[2] = d.alpha_test ? d.alpha_func : 7u;
  // Fx shadows: the count is a float the shader compares with 1, 2, 3...
  const float shadows = LoadFloat(base, dev + device::PsConstant(200));
  p.flags[1] = shadows > 0.0f ? std::min(uint32_t(shadows + 0.5f), 8u) : 0u;
  ReadConstants(base, dev, device::PsConstant(201), &p.shadow_spheres[0][0], 8);
  const auto& t = d.device_textures;
  auto on = [&](uint32_t b) { return PixelBool(base, dev, b); };
  const bool mask = on(shader::kBumpMegaReflectMaskBool);
  const bool reflect = on(shader::kBumpMegaReflectBool) &&
                       t[shader::kBumpMegaReflectSampler].present() &&
                       (!mask || t[shader::kBumpMegaMaskSampler].present());
  const bool refract = on(shader::kBumpMegaRefractBool) &&
                       t[shader::kBumpMegaBackgroundSampler].present();
  p.flags[0] =
      (on(shader::kBumpMegaRimBool) ? kBumpMegaRim : 0) | (refract ? kBumpMegaRefract : 0) |
      (reflect ? kBumpMegaReflect : 0) | (reflect && mask ? kBumpMegaReflectMask : 0) |
      (on(shader::kBumpMegaPaletteBool) && t[shader::kBumpMegaPaletteSampler].present()
           ? kBumpMegaPalette
           : 0) |
      (on(shader::kBumpMegaPalette2Bool) && t[shader::kBumpMegaPalette2Sampler].present()
           ? kBumpMegaPalette2
           : 0) |
      (on(shader::kBumpMegaRefractModulatedBool) ? kBumpMegaRefractModulated : 0) |
      (d.fog ? kBumpMegaFog : 0) |
      (!t[shader::kBumpMegaNormalSampler].present() ? kBumpMegaNoNormalMap : 0);
  // (What the vertex lacks is added by CommitDraw, once the layout is known.)
}

// The depth of field's constants (frame.h, DofParams; guest.h,
// shader::kDofShaderVtable for where each comes from).
void ReadDofParams(const uint8_t* base, uint32_t dev, const DrawCommand& d, DofParams& p) {
  p = DofParams{};
  std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
  p.pixel_size_max_radius[0] = LoadFloat(base, dev + device::PsConstant(1));      // PixelSizeHigh
  p.pixel_size_max_radius[1] = LoadFloat(base, dev + device::PsConstant(1) + 4);
  p.pixel_size_max_radius[2] = LoadFloat(base, dev + device::PsConstant(5));      // MaxRadius
  p.pixel_size_max_radius[3] = LoadFloat(base, dev + device::PsConstant(5) + 4);
  p.planes[0] = LoadFloat(base, dev + device::PsConstant(2));  // FocalPlaneDistance
  p.planes[1] = LoadFloat(base, dev + device::PsConstant(3));  // NearBlurPlaneDistance
  p.planes[2] = LoadFloat(base, dev + device::PsConstant(4));  // FarBlurPlaneDistance
  ReadConstants(base, dev, device::PsConstant(73), p.near_far);
  p.flags[0] = PixelBool(base, dev, shader::kDofEnableNearBlurBool) ? kDofNearBlur : 0u;
}

// A D3D surface's description, from the GPU registers the surface object
// keeps (guest.h, d3d_surface).
void ReadSurface(const uint8_t* base, uint32_t surface, Surface& out) {
  const uint32_t surface_info = Load32(base, surface + d3d_surface::kSurfaceInfo);
  const uint32_t target_info = Load32(base, surface + d3d_surface::kTargetInfo);
  const uint32_t size = Load32(base, surface + d3d_surface::kSize);
  out.format = out.depth ? uint8_t((target_info >> 16) & 1) : uint8_t((target_info >> 16) & 0xF);
  out.msaa = uint8_t((surface_info >> 16) & 3);
  out.width = ((size >> 18) & 0x1FFF) + 1;
  out.height = ((size >> 3) & 0x1FFF) + 1;
  // A surface the game renders in strips (predicated tiling) stands for the
  // whole tiled area.
  auto tiled = g.tiled_sizes.find(surface);
  if (tiled != g.tiled_sizes.end()) {
    out.width = std::max(out.width, tiled->second.first);
    out.height = std::max(out.height, tiled->second.second);
  }
}

// Describes a D3D surface for the frame, once per frame (guest.h, d3d_surface).
void NoteSurface(const uint8_t* base, uint32_t surface, bool depth) {
  if (!surface || g.frame.FindSurface(surface)) {
    return;
  }
  Surface s;
  s.guest = surface;
  s.depth = depth;
  ReadSurface(base, surface, s);
  g.frame.surfaces.push_back(s);
}

// Adds the held-back resolve clears to the frame (before the next draw or
// clear, or at the end of the frame).
void FlushResolveClears() {
  for (ClearCommand& clear : g.resolve_clears) {
    g.frame.commands.push_back({Command::Type::kClear, uint32_t(g.frame.clears.size())});
    g.frame.clears.push_back(std::move(clear));
  }
  g.resolve_clears.clear();
}

// Appends a finished draw to the frame, with its MaterialBlock (g.block, read
// by ReadMaterial) if it has one. The vertex layout is known only now, so the
// "component missing" flags of the block materials are added here.
void CommitDraw(DrawCommand& d) {
  if (d.block != DrawCommand::kNoBlock) {
    const VertexLayout& l = d.layout;
    if (d.material == Material::kLitSimple || d.material == Material::kCharacter ||
        d.material == Material::kReflect) {
      g.block.lit.counts[3] |=
          (l.colour == VertexLayout::kAbsent ? kLitNoColour : 0) |
          (l.uv == VertexLayout::kAbsent ? kLitNoUv : 0) |
          (l.normal == VertexLayout::kAbsent ? kLitNoNormal : 0) |
          (l.tangent == VertexLayout::kAbsent || l.binormal == VertexLayout::kAbsent
               ? kLitNoTangents
               : 0);
    } else if (d.material == Material::kParticle) {
      g.block.particle.flags[0] |= (l.colour == VertexLayout::kAbsent ? kParticleNoColour : 0) |
                                   (l.uv == VertexLayout::kAbsent ? kParticleNoUv : 0) |
                                   (spotter::Highlighting() == spotter::Highlight::kParticles
                                        ? kParticleHighlight
                                        : 0) |  // F11 (spotter.h)
                                   spotter::ParticleExperimentFlags();  // F12
    } else if (d.material == Material::kWater) {
      g.block.water.flags[0] |= (l.colour == VertexLayout::kAbsent ? kWaterNoColour : 0) |
                                (l.uv == VertexLayout::kAbsent ? kWaterNoUv : 0);
    } else if (d.material == Material::kBumpMega) {
      // Without tangents the normal map can't be applied: the plain normal.
      g.block.bump_mega.flags[0] |=
          (l.colour == VertexLayout::kAbsent ? kBumpMegaNoColour : 0) |
          (l.uv == VertexLayout::kAbsent ? kBumpMegaNoUv : 0) |
          (l.tangent == VertexLayout::kAbsent || l.binormal == VertexLayout::kAbsent
               ? kBumpMegaNoNormalMap
               : 0) |
          (spotter::Highlighting() == spotter::Highlight::kBumpMega ? kBumpMegaHighlight
                                                                     : 0);  // F11 (spotter.h)
    }
    d.block = uint32_t(g.frame.blocks.size());
    g.frame.blocks.push_back(g.block);
  }
  FlushResolveClears();
  g.frame.commands.push_back({Command::Type::kDraw, uint32_t(g.frame.draws.size())});
  g.frame.draws.push_back(d);
}

// --- What a draw looks like, read right after the game applied it ---------
//
// Both kinds of geometry (immediate batches and meshes) go through the same
// material setup in the game, so they share these two readers.

// The material: which of our shaders, its matrix / colours / textures /
// blending, from D3D's copy of the shader constants and the xn shaders'
// state cache (guest.h). False if we can't draw this material yet (reported).
bool ReadMaterial(const uint8_t* base, uint32_t xn_context, uint32_t material,
                  DrawCommand& d) {
  // The materials we can draw: simple, unlit (menus, HUD, most of the 3D
  // world) or lit (some characters and objects), Bink (movies), the
  // character material (Crash and co.), the soft shadow under them,
  // reflective surfaces, particles, water, the depth of field pass, the fx
  // grid (screen-wide glows such as the double mojo flash) and the bump mega
  // material (normal-mapped, see-through surfaces: TK blocks, ice).
  const uint32_t vtable = Load32(base, material);
  const bool bink = vtable == shader::kBinkShaderVtable;
  const bool character = vtable == shader::kCharacterRimShaderVtable;
  // The dig material (xnUndergroundShader) is the simple one with a forced
  // blend mode, which the state cache below already reflects (guest.h).
  const bool simple = vtable == shader::kSimpleShaderVtable ||
                      vtable == shader::kUndergroundShaderVtable;
  const bool shadow = vtable == shader::kShadowShaderVtable;
  const bool reflect = vtable == shader::kReflectShaderVtable;
  const bool particle = vtable == shader::kParticleShaderVtable;
  const bool water = vtable == shader::kWaterSpecularShaderVtable;
  const bool dof = vtable == shader::kDofShaderVtable;
  const bool fx_grid = vtable == shader::kFxGridShaderVtable;
  const bool bump_mega = vtable == shader::kBumpMegaShaderVtable;
  if (!bink && !character && !simple && !shadow && !reflect && !particle && !water && !dof &&
      !fx_grid && !bump_mega) {
    const char* cls = pddi::ClassOfVtable(vtable);
    ReportOnce(std::string("material class ") + (cls ? cls : "?"));
    return false;
  }
  const bool lit = simple && Load8(base, material + shader::kLit) != 0;
  d.material = bink        ? Material::kBink
               : character ? Material::kCharacter
               : shadow    ? Material::kShadow
               : reflect   ? Material::kReflect
               : particle  ? Material::kParticle
               : water     ? Material::kWater
               : dof       ? Material::kDof
               : fx_grid   ? Material::kFxGrid
               : bump_mega ? Material::kBumpMega
               : lit       ? Material::kLitSimple
                           : Material::kSimple;

  // Shader constants the game just set, from D3D's copy (guest.h).
  const uint32_t dev = Load32(base, xn_context + context::kDevice);
  for (uint32_t i = 0; i < 16; ++i) {
    d.mvp[i] = LoadFloat(base, dev + device::VsConstant(4) + 4 * i);
  }
  for (uint32_t i = 0; i < 3; ++i) {
    d.tint[i] = LoadFloat(base, dev + device::VsConstant(100) + 4 * i);
  }
  d.fade = LoadFloat(base, dev + device::PsConstant(2));
  d.textured = LoadFloat(base, dev + device::PsConstant(20)) > 0.0f;
  // Fog: on/off from the context, the values the shader reads (c70-c72).
  // (The fx grid's shaders have no fog.)
  if (!bink && !fx_grid) {
    const uint32_t fog = Load32(base, xn_context + context::kFogState);
    d.fog = Load8(base, fog + 4) != 0;
    if (d.fog) {
      for (uint32_t i = 0; i < 3; ++i) {
        d.fog_colour[i] = LoadFloat(base, dev + device::PsConstant(70) + 4 * i);
      }
      d.fog_start = LoadFloat(base, dev + device::PsConstant(71));
      d.fog_end = LoadFloat(base, dev + device::PsConstant(72));
    }
  }

  if (bink) {
    // The movie frame: 3 planes, and the colour matrix the draw setup just
    // loaded into pixel shader c0..c3 (a fixed table in the game's data).
    // Positions are screen pixels (its vertex shader, "passthru", has no
    // matrix), so mvp stays unused.
    d.textured = true;
    GuestTexture* planes[3] = {&d.texture, &d.chroma[0], &d.chroma[1]};
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t plane = Load32(base, material + shader::kBinkPlanes + 4 * i);
      const uint32_t d3d = plane ? Load32(base, plane + texture::kD3DTexture) : 0;
      if (!d3d) {
        return false;  // not all planes there yet: nothing sensible to draw
      }
      ReadFetchConstant(base, d3d, *planes[i]);
    }
    for (uint32_t i = 0; i < 16; ++i) {
      d.colour_matrix[i] = LoadFloat(base, dev + device::PsConstant(0) + 4 * i);
    }
  }

  // Texture: the material's xnTexture -> its D3D texture -> fetch constant.
  // The character and reflect materials always sample their diffuse texture
  // (they have no "textured" switch like the simple one's c20).
  if (character || reflect) {
    d.textured = true;
  }
  if (shadow) {
    d.textured = false;  // its one texture is the stencil copy, below
    d.fog = false;
  }
  // The particle material always samples its texture (its pixel shader has
  // no c20 switch, though the setup writes one): white if it has none.
  if (particle) {
    d.textured = true;
  }
  // Water, the depth of field, the fx grid and bump mega take all their
  // textures from D3D's sampler copies (below).
  const uint32_t xn_texture =
      bink || shadow || water || dof || fx_grid || bump_mega
          ? 0
          : Load32(base, material + (character  ? shader::kCharacterDiffuse
                                     : reflect  ? shader::kReflectDiffuse
                                     : particle ? shader::kParticleTexture
                                                : shader::kSimpleTexture));
  if (d.textured && xn_texture) {
    ReadXnTexture(base, xn_texture, d.texture, d.palette);
  }
  d.textured = d.textured && d.texture.present();
  if (d.textured) {
    d.texture_sampler = ReadSampler(base, dev, 0);
  }
  if (character) {
    if (!d.textured) {
      return false;  // nothing sensible to draw without its diffuse texture
    }
    // Normal map (sampler 1) and the second texture (sampler 3), see guest.h.
    if (const uint32_t normal = Load32(base, material + shader::kCharacterNormalMap)) {
      ReadXnTexture(base, normal, d.normal_map, d.normal_palette);
      d.normal_sampler = ReadSampler(base, dev, 1);
    }
    if (const uint32_t blend = Load32(base, material + shader::kCharacterBlendMap)) {
      GuestTexture unused_palette;  // the shader never looks one up for it
      ReadXnTexture(base, blend, d.blend_map, unused_palette);
      d.blend_sampler = ReadSampler(base, dev, 3);
    }
    // Without a normal map the game's draw setup leaves sampler 1 as it
    // was (whatever the previous draw bound): we use a flat normal instead
    // (kLitNoNormalMap, set in ReadLitParams).
    if (Load32(base, material + shader::kCharacterRefract)) {
      // Needs a copy of the screen behind the character (sampler 2); drawn
      // without the see-through part for now.
      ReportOnce("refraction of the character material (drawn opaque)");
    }
  }

  if (reflect) {
    // The reflection (sampler 1). Without it there's nothing to reflect: the
    // game's shader would sample whatever sampler 1 held; we skip the draw.
    const uint32_t environment = Load32(base, material + shader::kReflectEnvironment);
    GuestTexture unused_palette;  // the shader samples it as it is
    if (!environment || !ReadXnTexture(base, environment, d.reflection, unused_palette)) {
      ReportOnce("reflect material without a reflection texture");
      return false;
    }
    d.reflection_sampler = ReadSampler(base, dev, 1);
  }

  if (particle) {
    // The scene's depth (sampler 1): the setup just had the context resolve
    // it (once per frame), so it's a depth resolve's destination.
    if (ReadDeviceTexture(base, dev, shader::kParticleDepthSampler, d.depth_copy)) {
      d.depth_sampler = ReadSampler(base, dev, shader::kParticleDepthSampler);
    }
  }

  if (water) {
    // All nine, as the GPU will sample them (guest.h). The scene behind the
    // water and its depth are required: without them there's no sensible
    // picture (the colour resolve happened inside this very setup).
    for (uint32_t s = 0; s < shader::kWaterSamplers; ++s) {
      ReadDeviceTexture(base, dev, s, d.device_textures[s]);
      d.device_samplers[s] = ReadSampler(base, dev, s);
    }
    if (!d.device_textures[shader::kWaterBackgroundSampler].present() ||
        !d.device_textures[shader::kWaterDepthSampler].present()) {
      ReportOnce("water without the scene's colour or depth copy");
      return false;
    }
    d.textured = d.device_textures[0].present();
  }

  if (dof) {
    // The picture so far (sampler 0, resolved inside this setup) and the
    // scene's depth (sampler 1), as the GPU will sample them (guest.h).
    // Without both there's nothing to blur: the draw is skipped, which
    // leaves the picture sharp.
    d.fog = false;
    if (!ReadDeviceTexture(base, dev, shader::kDofFramebufferSampler, d.texture) ||
        !ReadDeviceTexture(base, dev, shader::kDofDepthSampler, d.depth_copy)) {
      ReportOnce("depth of field without the picture's or the depth's copy");
      return false;
    }
    d.textured = true;
    d.palette = GuestTexture{};
    d.texture_sampler = ReadSampler(base, dev, shader::kDofFramebufferSampler);
    d.depth_sampler = ReadSampler(base, dev, shader::kDofDepthSampler);
  }

  if (bump_mega) {
    // All nine samplers as the GPU will sample them (guest.h); the diffuse
    // texture is required (nothing sensible to draw without it). Which of
    // the others matter is decided in ReadBumpMegaParams.
    for (uint32_t s = 0; s < d.device_textures.size(); ++s) {
      ReadDeviceTexture(base, dev, s, d.device_textures[s]);
      d.device_samplers[s] = ReadSampler(base, dev, s);
    }
    if (!d.device_textures[shader::kBumpMegaDiffuseSampler].present()) {
      ReportOnce("bump mega material without its diffuse texture");
      return false;
    }
    d.textured = true;
  }

  if (fx_grid) {
    // The copy of the picture so far (sampler 0, resolved just before the
    // grid is drawn) and add-or-multiply (PS b10), guest.h. Its shaders have
    // no tint, fade or fog: the simple vertex shader we reuse gets a white
    // tint. Without the copy the draw is skipped (no glow).
    d.tint[0] = d.tint[1] = d.tint[2] = 1.0f;
    d.fade = 1.0f;
    if (!ReadDeviceTexture(base, dev, shader::kFxGridScreenSampler, d.texture)) {
      ReportOnce("fx grid without the picture's copy");
      return false;
    }
    d.textured = true;
    d.palette = GuestTexture{};
    d.texture_sampler = ReadSampler(base, dev, shader::kFxGridScreenSampler);
    d.fx_add = PixelBool(base, dev, shader::kFxGridAddBool);
  }

  // Blending and alpha test, from the xn shaders' state cache (guest.h).
  const uint32_t cache = Load32(base, material + shader::kStateCache);
  // The factors are kept in PDDI's numbering; D3D gets them through the
  // game's table (guest.h, shader::kBlendFactorTable).
  auto factor = [](uint32_t pddi) {
    return pddi < std::size(shader::kBlendFactorTable) ? shader::kBlendFactorTable[pddi]
                                                       : uint8_t(1);
  };
  d.blend.enable = Load8(base, cache + 204) != 0;
  d.blend.op = uint8_t(Load32(base, cache + 208));
  d.blend.src = factor(Load32(base, cache + 212));
  d.blend.dst = factor(Load32(base, cache + 216));
  d.alpha_test = Load8(base, cache + 220) != 0;
  d.alpha_func = uint8_t(Load32(base, cache + 224) & 7);
  d.alpha_ref = LoadFloat(base, material + shader::kAlphaRef);
  const uint32_t blend_mode = Load32(base, material + shader::kBlendMode);
  // (The reflect, water, fx grid and bump mega shaders have no additive /
  // subtractive fade: their setups set no b80/b81.)
  d.fade_rgb =
      !reflect && !water && !fx_grid && !bump_mega && (blend_mode == 2 || blend_mode == 3);

  // The lit materials' lights, surface colours... (CommitDraw stores them).
  if (lit || character || reflect) {
    ReadLitParams(base, dev, d, g.block.lit);
    d.block = 0;  // "has a block"; the real index is given by CommitDraw
  } else if (simple && ReadSimpleLights(base, dev, g.block.lit)) {
    d.block = 0;  // proximity lights / Fx shadows on (simple.frag, kFlagLights)
  }
  if (particle) {
    ReadParticleParams(base, dev, d, g.block.particle);
    d.block = 0;
  }
  if (water) {
    ReadWaterParams(base, dev, d, g.block.water);
    d.block = 0;
  }
  if (dof) {
    ReadDofParams(base, dev, d, g.block.dof);
    d.block = 0;
  }
  if (bump_mega) {
    ReadBumpMegaParams(base, dev, d, g.block.bump_mega);
    d.block = 0;
  }
  if (shadow) {
    // The stencil copy on sampler 0, as D3D programmed the GPU (its address
    // is physical there; the render targets compare addresses physically),
    // and the blur taps (guest.h, shader::kShadowShaderVtable).
    for (uint32_t i = 0; i < 6; ++i) {
      d.stencil_copy.fetch[i] = Load32(base, dev + device::SamplerFetch(0) + 4 * i);
    }
    d.stencil_sampler = ReadSampler(base, dev, 0);
    ShadowParams& p = g.block.shadow;
    p = ShadowParams{};
    std::memcpy(p.mvp, d.mvp, sizeof(p.mvp));
    ReadConstants(base, dev, device::PsConstant(2), &p.offsets[0][0], 9);
    ReadConstants(base, dev, device::PsConstant(30), &p.weights[0][0], 9);
    d.block = 0;
  }
  return true;
}

// The context's fixed-function state: depth, culling, colour mask, viewport
// (guest.h, context::kRenderState / kViewport). Culling follows the rule of
// xnShader's common-state function (0x8242DA28): none for a two-sided
// material, else the context's mode.
void ReadRenderState(const uint8_t* base, uint32_t xn_context, uint32_t material,
                     DrawCommand& d) {
  const uint32_t rs = Load32(base, xn_context + context::kRenderState);
  d.depth_test = Load8(base, rs + 8) != 0;
  d.depth_func = context::kDepthCompareTable[Load32(base, rs + 12) & 7];
  d.depth_write = Load8(base, rs + 16) != 0;
  const bool two_sided = material && Load8(base, material + shader::kTwoSided) != 0;
  d.cull = two_sided ? 0 : context::kCullTable[Load32(base, rs + 4) & 3];
  d.colour_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    d.colour_mask |= Load8(base, rs + 24 + i) ? uint8_t(1u << i) : 0;
  }
  // Stencil (guest.h, context::kStencilState).
  const uint32_t st = Load32(base, xn_context + context::kStencilState);
  Stencil& sten = d.stencil;
  sten.enable = Load8(base, st + 4) != 0;
  sten.func = context::kStencilCompareTable[Load32(base, st + 8) & 7];
  sten.reference = uint8_t(Load32(base, st + 12));
  sten.read_mask = uint8_t(Load32(base, st + 16));
  sten.write_mask = uint8_t(Load32(base, st + 20));
  auto op = [&](uint32_t offset) {
    const uint32_t pddi = Load32(base, st + offset);
    return pddi < 6 ? context::kStencilOpTable[pddi] : uint8_t(0);
  };
  sten.fail = op(24);
  sten.depth_fail = op(28);
  sten.pass = op(32);
  sten.two_sided = g.stencil_two_sided;
  if (sten.two_sided) {
    sten.back_func = g.back_func;
    sten.back_fail = g.back_ops[0];
    sten.back_depth_fail = g.back_ops[1];
    sten.back_pass = g.back_ops[2];
  } else {
    sten.back_func = sten.func;
    sten.back_fail = sten.fail;
    sten.back_depth_fail = sten.depth_fail;
    sten.back_pass = sten.pass;
  }
  const uint32_t vp = xn_context + context::kViewport;
  d.viewport.x = float(int32_t(Load32(base, vp)));
  d.viewport.y = float(int32_t(Load32(base, vp + 4)));
  d.viewport.width = float(int32_t(Load32(base, vp + 8)));
  d.viewport.height = float(int32_t(Load32(base, vp + 12)));
  d.viewport.min_z = LoadFloat(base, vp + 16);
  d.viewport.max_z = LoadFloat(base, vp + 20);
}

// --- D3D::SetRenderTarget (0x8230DD40) -----------------------------------------
// r4 = render target index (0..3), r5 = the D3D surface (0 = none). Every
// path of the game goes through here: xnContext::BeginFrame (via
// 0x824334A8), xnContext::v94 (render to texture) and the post-processing
// setup 0x82431600 (which calls D3D directly). Only target 0 is drawn by us.
void OnD3DSetRenderTarget(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t index = ctx.r4.u32, surface = ctx.r5.u32;
  original(ctx, base);
  if (index == 0) {
    g.targets.colour = surface;
    if (g.recording) {
      NoteSurface(base, surface, false);
    }
  }
}

// --- D3D::SetDepthStencilSurface (0x8230DA68) ------------------------------------
// r4 = the D3D depth/stencil surface (0 = none).
void OnD3DSetDepthStencil(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t surface = ctx.r4.u32;
  original(ctx, base);
  g.targets.depth = surface;
  if (g.recording) {
    NoteSurface(base, surface, true);
  }
}

// --- D3D::Clear (0x82322808) ---------------------------------------------------------
// r4 = rectangle count, r5 = rectangles (D3DRECT: x0, y0, x1, y1), r6 = flags
// (0x1..0x8 render targets 0..3, 0x10 depth, 0x20 stencil; xnContext::Clear
// 0x824306A0 maps PDDI's colour/depth/stencil to 0xF/0x10/0x20), r7 = colour
// (D3DCOLOR), f1 = depth as stored (the game passes 1 - its value: reversed Z),
// r9 = stencil. Hooked at D3D level because the game also calls it directly
// (0x82431358, 0x82431600: stencil-only clears of the tiling strips).
void OnD3DClear(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g.recording) {
    const uint32_t flags = ctx.r6.u32;
    ClearCommand clear;
    clear.colour = (flags & 1) != 0;
    clear.depth = (flags & 0x10) != 0;
    clear.stencil = (flags & 0x20) != 0;
    if (clear.colour || clear.depth || clear.stencil) {
      const uint32_t argb = ctx.r7.u32;
      clear.colour_value[0] = float((argb >> 16) & 0xFF) / 255.0f;
      clear.colour_value[1] = float((argb >> 8) & 0xFF) / 255.0f;
      clear.colour_value[2] = float(argb & 0xFF) / 255.0f;
      clear.colour_value[3] = float(argb >> 24) / 255.0f;
      clear.depth_value = float(ctx.f1.f64);
      clear.stencil_value = ctx.r9.u32 & 0xFF;
      const uint32_t count = std::min<uint32_t>(ctx.r4.u32, 16);
      for (uint32_t i = 0; i < count && ctx.r5.u32; ++i) {
        const uint32_t r = ctx.r5.u32 + 16 * i;
        clear.rects.push_back({int32_t(Load32(base, r)), int32_t(Load32(base, r + 4)),
                               int32_t(Load32(base, r + 8)), int32_t(Load32(base, r + 12))});
      }
      clear.targets = g.targets;
      FlushResolveClears();
      g.frame.commands.push_back({Command::Type::kClear, uint32_t(g.frame.clears.size())});
      g.frame.clears.push_back(std::move(clear));
    }
  }
  original(ctx, base);
}

// --- D3D::Resolve (0x8231EE00) ---------------------------------------------------------
// Copies a rectangle of a surface (EDRAM) into a texture. Arguments as in the
// Xbox 360 D3D API: r4 = flags (bits 0-1 which render target, 0x4 = the
// depth/stencil surface instead, 0x100 / 0x200 = clear the render target /
// depth afterwards), r5 = source rectangle (D3DRECT, 0 = whole surface),
// r6 = destination D3D texture, r7 = destination point (x, y; 0 = the
// rectangle's own corner), r10 = clear colour (4 floats), f1 = clear depth.
void OnD3DResolve(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  // Debug: the same frame from both renderers (ab_capture.h) marks the
  // frontbuffer before the emulated GPU gets to resolve into it (once per
  // frame, before the first strip).
  if (g.in_end_frame && ctx.r6.u32 && !g.frontbuffer_seen) {
    g.frontbuffer_seen = true;
    GuestTexture frontbuffer;
    ReadFetchConstant(base, ctx.r6.u32, frontbuffer);
    ab_capture::BeforeFrontbufferResolve(base, frontbuffer);
  }
  if (g.recording) {
    const uint32_t flags = ctx.r4.u32;
    ResolveCommand r;
    r.depth = (flags & 4) != 0;
    r.sample_select = uint8_t((flags >> 4) & 7);  // frame.h
    r.source = r.depth ? g.targets.depth : g.targets.colour;
    r.frontbuffer = g.in_end_frame;
    const Surface* surface = g.frame.FindSurface(r.source);
    if ((flags & 3) != 0 && !r.depth) {
      ReportOnce("resolve of render target " + std::to_string(flags & 3));
    } else if (surface && ctx.r6.u32) {
      if (ctx.r5.u32) {
        r.x0 = int32_t(Load32(base, ctx.r5.u32));
        r.y0 = int32_t(Load32(base, ctx.r5.u32 + 4));
        r.x1 = int32_t(Load32(base, ctx.r5.u32 + 8));
        r.y1 = int32_t(Load32(base, ctx.r5.u32 + 12));
      } else {
        r.x1 = int32_t(surface->width);
        r.y1 = int32_t(surface->height);
      }
      if (ctx.r7.u32) {
        r.dest_x = int32_t(Load32(base, ctx.r7.u32));
        r.dest_y = int32_t(Load32(base, ctx.r7.u32 + 4));
      } else {
        r.dest_x = r.x0;
        r.dest_y = r.y0;
      }
      ReadFetchConstant(base, ctx.r6.u32, r.dest);
      g.frame.commands.push_back({Command::Type::kResolve, uint32_t(g.frame.resolves.size())});
      g.frame.resolves.push_back(r);
    }
    // The clears a resolve can do afterwards (the tiling end uses them).
    // Held back until the run of resolves is over: at the end of tiling the
    // game resolves strip 0 and clears it, then strip 1, which overlaps strip
    // 0 by 32 pixels... On the Xbox each strip is a separate render, so a
    // strip's clear can't touch the next strip's pixels; on our side it's
    // one image, and clearing right away would black out the overlaps.
    if (flags & 0x300) {
      ClearCommand clear;
      clear.colour = (flags & 0x100) != 0;
      clear.depth = clear.stencil = (flags & 0x200) != 0;
      if (clear.colour && ctx.r10.u32) {
        for (uint32_t i = 0; i < 4; ++i) {
          clear.colour_value[i] = LoadFloat(base, ctx.r10.u32 + 4 * i);
        }
      }
      clear.depth_value = float(ctx.f1.f64);
      if (ctx.r5.u32) {
        clear.rects.push_back({r.x0, r.y0, r.x1, r.y1});
      }
      clear.targets = g.targets;
      g.resolve_clears.push_back(std::move(clear));
    }
  }
  original(ctx, base);
}

// --- D3D::BeginTiling (0x823193D0) -------------------------------------------------
// r5 = number of strips, r6 = their rectangles (D3DRECT x0, y0, x1, y1 in
// screen pixels). The surfaces bound now are rendered through strip by
// strip; on our side they're one full-size image, as big as the strips'
// union (ReadSurface).
void OnD3DBeginTiling(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t count = std::min<uint32_t>(ctx.r5.u32, 16), rects = ctx.r6.u32;
  original(ctx, base);
  if (!rects || !count) {
    return;
  }
  uint32_t width = 0, height = 0;
  for (uint32_t i = 0; i < count; ++i) {
    width = std::max(width, Load32(base, rects + 16 * i + 8));
    height = std::max(height, Load32(base, rects + 16 * i + 12));
  }
  for (uint32_t surface : {g.targets.colour, g.targets.depth}) {
    if (!surface) {
      continue;
    }
    g.tiled_sizes[surface] = {width, height};
    // Already described this frame (bound before BeginTiling): update it.
    for (Surface& s : g.frame.surfaces) {
      if (s.guest == surface) {
        s.width = std::max(s.width, width);
        s.height = std::max(s.height, height);
      }
    }
  }
}

// --- Two-sided (back-face) stencil: xnContext::v95 / v96 / v97 ----------------------
// v95 (0x82430B98): r4/r5/r6 = PDDI ops on fail / depth fail / pass for
// counter-clockwise triangles, and turns two-sided stencil on. v96
// (0x82430C00): r4 = their PDDI compare. v97 (0x82430C18): r4 = two-sided
// on/off. All three go straight to D3D, so we keep the values here.
void OnStencilBackOps(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t ops[3] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  original(ctx, base);
  for (int i = 0; i < 3; ++i) {
    g.back_ops[i] = ops[i] < 6 ? context::kStencilOpTable[ops[i]] : 0;
  }
  g.stencil_two_sided = true;
}
void OnStencilBackCompare(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  g.back_func = context::kStencilCompareTable[ctx.r4.u32 & 7];
  original(ctx, base);
}
void OnStencilTwoSided(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  g.stencil_two_sided = (ctx.r4.u32 & 0xFF) != 0;
  original(ctx, base);
}

// --- xnContext::EndFrame (0x82431F40) -----------------------------------------------
// Ends the tiling and resolves the finished picture into the frontbuffer
// (the resolve at 0x82431FB4): that surface is what the native renderer shows.
void OnEndFrame(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  g.in_end_frame = true;
  g.frontbuffer_seen = false;
  original(ctx, base);
  g.in_end_frame = false;
}

// --- xnContext::BeginPrims (0x82430780) / BeginIndexedPrims (0x824307F8) -------
// r4 = material (xnShader*, 0 = the context's default), r5 = PDDI primitive
// type, r6 = vertex format flags, r7 = vertex count; BeginIndexedPrims also
// r8 = index count (it's BeginPrims plus an index list: the game's
// CPU-skinned characters, drawn by 0x823F3130 once per material pass).
// Both return the stream the game then writes vertices (and indices)
// through. Inside, the original applies the material (D3D shaders, textures,
// blend states, shader constants) and uploads the matrices, so right after
// it returns, everything this draw will use can be read back.
void BeginImmediate(PPCContext& ctx, uint8_t* base, PPCFunc* original, bool indexed) {
  const uint32_t xn_context = ctx.r3.u32;
  uint32_t material = ctx.r4.u32;
  const uint32_t prim_type = ctx.r5.u32;
  const uint32_t index_count = indexed ? ctx.r8.u32 : 0;
  original(ctx, base);
  if (!g.recording) {
    return;
  }
  g.pending = false;
  if (!material) {
    material = Load32(base, xn_context + context::kDefaultShader);
  }
  const uint32_t prim_stream = Load32(base, xn_context + context::kPrimStream);
  const uint32_t vertices = Load32(base, prim_stream + stream::kPosition);
  if (!material || !prim_stream || !vertices) {
    return;
  }
  if (prim_type > uint32_t(Topology::kPointList)) {
    ReportOnce("primitive type " + std::to_string(prim_type));
    return;
  }

  DrawCommand& d = g.pending_draw;
  d = DrawCommand{};
  if (!ReadMaterial(base, xn_context, material, d)) {
    return;
  }
  ReadRenderState(base, xn_context, material, d);
  d.topology = Topology(prim_type);
  d.targets = g.targets;

  // Vertex layout, from the stream's own component pointers (guest.h,
  // stream): each points at that component of vertex 0.
  const uint32_t stride = Load32(base, prim_stream + stream::kStride);
  auto offset_of = [&](uint32_t pointer_field) {
    const uint32_t pointer = Load32(base, prim_stream + pointer_field);
    return pointer > vertices && pointer - vertices < stride ? uint8_t(pointer - vertices)
                                                             : VertexLayout::kAbsent;
  };
  d.layout.stride = uint8_t(std::min<uint32_t>(stride, 255));
  d.layout.normal = offset_of(stream::kNormal);
  d.layout.colour = offset_of(stream::kColour);
  d.layout.uv = offset_of(stream::kUv);
  d.layout.binormal = offset_of(stream::kBinormal);
  d.layout.tangent = offset_of(stream::kTangent);
  d.vertex_count = Load32(base, prim_stream + stream::kCount);
  d.index_count = index_count;
  g.pending_vertices = vertices;
  g.pending_stream = prim_stream;
  // Whole 32-bit words per vertex (frame.h, VertexLayout), sane counts.
  g.pending = d.vertex_count > 0 && d.vertex_count <= 65536 && stride >= 12 && stride <= 252 &&
              stride % 4 == 0 && d.index_count <= 3 * 65536;
}
void OnBeginPrims(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  BeginImmediate(ctx, base, original, false);
}
void OnBeginIndexedPrims(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  BeginImmediate(ctx, base, original, true);
}

// --- xnContext::EndPrims (0x82430870) ----------------------------------------
// The game has written its vertices (and indices): copy them before the
// original closes the stream (the data stays in D3D's command buffer until
// the GPU is done, but we copy first anyway). Each 32-bit word is
// byte-swapped, which turns every component into host order (frame.h).
void OnEndPrims(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g.recording && g.pending) {
    DrawCommand& d = g.pending_draw;
    // F12's "erase" experiments (spotter.h): zero this particle draw's vertex
    // colours in the game's own data, so the emulated GPU draws it invisibly
    // too (ours copies the zeros below).
    const spotter::ParticleErase erase = spotter::ParticleEraseMode();
    if (erase != spotter::ParticleErase::kNone && d.material == Material::kParticle &&
        d.layout.colour != VertexLayout::kAbsent) {
      const bool additive = d.blend.enable && d.blend.dst == 1;  // ONE: adds
      const bool hit = erase == spotter::ParticleErase::kBig        ? d.vertex_count >= 300
                       : erase == spotter::ParticleErase::kAdditive ? additive
                                                                     : !additive;
      if (hit) {
        for (uint32_t v = 0; v < d.vertex_count; ++v) {
          std::memset(GuestPtr(base, g.pending_vertices + v * d.layout.stride + d.layout.colour), 0, 4);
        }
      }
    }
    const uint32_t words = d.layout.stride / 4 * d.vertex_count;
    d.vertex_offset = uint32_t(g.frame.vertex_data.size() * 4);
    g.frame.vertex_data.resize(g.frame.vertex_data.size() + words);
    uint32_t* out = g.frame.vertex_data.data() + d.vertex_offset / 4;
    const uint8_t* in = GuestPtr(base, g.pending_vertices);
    for (uint32_t i = 0; i < words; ++i) {
      out[i] = Load32(in + 4 * i);
    }
    bool ok = true;
    if (d.index_count) {
      // 16-bit big-endian indices. One pointing past this draw's vertices
      // would read another draw's data (or past the buffer): made degenerate.
      const uint32_t indices = Load32(base, g.pending_stream + stream::kIndices);
      const uint8_t* src = indices ? TranslateReadable(indices, d.index_count * 2) : nullptr;
      if (src) {
        d.first_index = uint32_t(g.frame.indices.size());
        g.frame.indices.resize(d.first_index + d.index_count);
        uint16_t* dst = g.frame.indices.data() + d.first_index;
        for (uint32_t i = 0; i < d.index_count; ++i) {
          const uint16_t index = uint16_t(src[2 * i] << 8 | src[2 * i + 1]);
          dst[i] = index < d.vertex_count ? index : 0;
        }
      } else {
        ReportOnce("indexed geometry without readable indices");
        ok = false;
      }
    }
    if (ok) {
      CommitDraw(d);
    } else {
      g.frame.vertex_data.resize(d.vertex_offset / 4);
    }
  }
  g.pending = false;
  original(ctx, base);
}

// --- xnContext::DrawPrimBuffer (0x82430878) -----------------------------------
// r4 = material (0 = the context's default), r5 = the xnPrimBuffer (a mesh).
// For each of the material's passes it applies that pass (D3D shaders,
// constants, textures...) and calls the mesh's Draw (vtable slot 15,
// 0x82443BD0). We only remember the context and material here; the draw
// itself is recorded in OnPrimBufferDraw, where the pass is fully set up.
void OnDrawPrimBuffer(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  uint32_t material = ctx.r4.u32;
  if (!material) {
    material = Load32(base, ctx.r3.u32 + context::kDefaultShader);
  }
  const uint32_t saved_context = g.mesh_context, saved_material = g.mesh_material;
  g.mesh_context = ctx.r3.u32;
  g.mesh_material = material;
  original(ctx, base);
  g.mesh_context = saved_context;
  g.mesh_material = saved_material;
}

// --- xnPrimBuffer::Draw (0x82443BD0) ------------------------------------------
// r3 = the xnPrimBuffer. The original binds its buffers (SetIndices,
// SetVertexDeclaration, SetStreamSource) and draws all its indices (or
// vertices), or the count at +40 if that's set. We note the buffers'
// addresses; the buffer cache uploads them (once) at the end of the frame.
void OnPrimBufferDraw(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g.recording) {
    const uint32_t pb = ctx.r3.u32;
    if (!g.mesh_material) {
      ReportOnce("xnPrimBuffer::Draw outside xnContext::DrawPrimBuffer");
    } else {
      DrawCommand d;
      if (ReadMaterial(base, g.mesh_context, g.mesh_material, d)) {
        ReadRenderState(base, g.mesh_context, g.mesh_material, d);
        d.targets = g.targets;
        d.mesh = true;
        // Xbox GPU primitive type -> ours (the only ones PDDI creates).
        bool ok = true;
        switch (Load32(base, pb + prim_buffer::kPrimType)) {
          case 1: d.topology = Topology::kPointList; break;
          case 2: d.topology = Topology::kLineList; break;
          case 3: d.topology = Topology::kLineStrip; break;
          case 4: d.topology = Topology::kTriangleList; break;
          case 6: d.topology = Topology::kTriangleStrip; break;
          default:
            ReportOnce("mesh primitive type " +
                       std::to_string(Load32(base, pb + prim_buffer::kPrimType)));
            ok = false;
        }
        const uint32_t format = Load32(base, pb + prim_buffer::kFormat);
        const uint32_t stride = Load32(base, pb + prim_buffer::kStride);
        d.layout = MeshVertexLayout(format, stride);
        // The vertex buffer's fetch constant (guest.h, d3d_vertex_buffer).
        const uint32_t vb = Load32(base, pb + prim_buffer::kVertexBuffer);
        if (vb) {
          const uint32_t word0 = Load32(base, vb + d3d_vertex_buffer::kFetchConstant);
          const uint32_t word1 = Load32(base, vb + d3d_vertex_buffer::kFetchConstant + 4);
          d.vertex_buffer.address = word0 & ~3u;
          d.vertex_buffer.size = ((word1 >> 2) & 0xFFFFFF) * 4;
          d.vertex_buffer.endian = uint8_t(word1 & 3);
        }
        const uint32_t count_override = Load32(base, pb + prim_buffer::kCountOverride);
        const uint32_t index_count = Load32(base, pb + prim_buffer::kIndexCount);
        const uint32_t ib = Load32(base, pb + prim_buffer::kIndexBuffer);
        if (int32_t(index_count) > 0 && ib) {
          d.index_buffer.address = Load32(base, ib + d3d_index_buffer::kAddress);
          d.index_buffer.size = Load32(base, ib + d3d_index_buffer::kSize);
          d.index_buffer.endian = 1;  // 16-bit indices, each byte-swapped
          d.index_count = count_override ? count_override : index_count;
          // Never read past the buffer, whatever the counts say.
          d.index_count = std::min(d.index_count, d.index_buffer.size / 2);
        } else {
          d.vertex_count =
              count_override ? count_override : Load32(base, pb + prim_buffer::kVertexCount);
        }
        if (stride >= 12 && stride <= 255 && d.vertex_buffer.present() &&
            (d.index_count || d.vertex_count) && ok) {
          CommitDraw(d);
        } else if (ok) {
          ReportOnce("a mesh with an unexpected layout or no vertex buffer");
        }
      }
    }
  }
  original(ctx, base);
}

// The functions the recorder listens to, and its handler for each.
struct Hook {
  pddi::FnId fn;
  pddi::Handler handler;
};
constexpr Hook kHooks[] = {
    {pddi::kFn_82430780, &OnBeginPrims},          {pddi::kFn_82430870, &OnEndPrims},
    {pddi::kFn_824307F8, &OnBeginIndexedPrims},   {pddi::kFn_82430878, &OnDrawPrimBuffer},
    {pddi::kFn_82443BD0, &OnPrimBufferDraw},      {pddi::kFn_82431F40, &OnEndFrame},
    {pddi::kFn_8230DD40, &OnD3DSetRenderTarget},  {pddi::kFn_8230DA68, &OnD3DSetDepthStencil},
    {pddi::kFn_82322808, &OnD3DClear},            {pddi::kFn_8231EE00, &OnD3DResolve},
    {pddi::kFn_823193D0, &OnD3DBeginTiling},
    {pddi::kFn_82430B98, &OnStencilBackOps},      {pddi::kFn_82430C00, &OnStencilBackCompare},
    {pddi::kFn_82430C18, &OnStencilTwoSided},
};

}  // namespace

void Install() {
  for (const Hook& hook : kHooks) {
    pddi::SetHandler(hook.fn, hook.handler);
  }
}

void Uninstall() {
  for (const Hook& hook : kHooks) {
    pddi::SetHandler(hook.fn, nullptr);
  }
}

void TakeFrame(Frame& out, bool record_next) {
  FlushResolveClears();
  std::swap(out, g.frame);
  out.number = g.frame_counter++;
  g.frame.Reset();
  g.frame.recorded = record_next;
  g.recording = record_next;
  g.pending = false;
  // The bound surfaces carry over into the next frame (BeginFrame re-binds
  // them anyway); describe them for it.
  if (record_next) {
    const uint8_t* base = guest::Base();
    NoteSurface(base, g.targets.colour, false);
    NoteSurface(base, g.targets.depth, true);
  }
}

}  // namespace recorder
}  // namespace native
