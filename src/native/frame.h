// =============================================================================
// native/frame.h -- one game frame, described for the native renderer
// =============================================================================
//
// The recorder (recorder.cpp) watches the game's renderer calls during a
// frame and writes down what they mean in plain terms: "clear the screen to
// this colour", "draw these 6 vertices with this texture, blended like
// this", "draw that mesh with this material". At the end of the frame the
// native renderer (native_renderer.cpp) turns that list into Vulkan commands.
//
// Keeping the two apart means the recorder runs in the middle of the game's
// own code and only copies data, while everything Vulkan happens in one place,
// once per frame.
//
// Milestone 3 (docs/04-native-renderer.md) covered what the loading screen,
// title and menus use: clears and immediate-mode geometry drawn with the
// game's "simple" material (xnSimpleShader), plain or palettized textures,
// plus the Bink material (xnBinkShader) that shows the intro movies.
//
// Milestone 4 adds the 3D world: STATIC MESHES (xnPrimBuffer), whose vertices
// stay in the game's own vertex/index buffers (a mesh is uploaded to the PC's
// GPU once and reused, see buffer_cache.h), and the states 3D drawing needs:
// depth test/write, culling, the viewport (which carries the game's reversed
// depth, guest.h), colour write masks.
//
// RENDER TARGETS AND RESOLVES (milestone 4). The Xbox 360 draws into EDRAM,
// 10 MB of memory inside the GPU, through D3D "surfaces" (render targets).
// EDRAM can't be sampled: to use a picture as a texture the game RESOLVES it
// (the GPU copies a rectangle of EDRAM into a texture in RAM). A hub frame
// does this ~30 times: the water reflection, depth for soft particles, the
// whole scene for depth of field, and at the very end the finished picture
// into the frontbuffer the TV shows. So a frame is a sequence of:
//   draws and clears into the current surfaces (colour + depth),
//   resolves of a surface's rectangle into a texture,
// and the picture to show is whichever surface EndFrame resolves into the
// frontbuffer. The native renderer keeps one Vulkan image per surface and one
// per resolved texture (render_targets in native_renderer.h).
//
// CHARACTERS (milestone 4, part 2; docs/findings/11). Crash and the other
// characters are skinned by the game's CPU every frame, so they don't come
// as meshes: the game streams the finished vertices plus a list of INDICES
// (which vertices make each triangle) through the immediate-mode stream
// (xnContext::BeginIndexedPrims). Their vertices carry normals, and Crash's
// also a tangent and binormal (for his normal map), so immediate geometry is
// now kept as the game wrote it (raw 32-bit words, byte-swapped) with a
// VertexLayout, exactly like meshes. Their materials light each pixel
// (up to 4 directional lights, point lights, "shadow spheres", rim light):
// more constants than push constants hold, so those draws get a LitParams
// block (below) in a per-frame uniform buffer.
//
// SHADOWS (findings/12): shadow volumes mark the stencil (plain draws), then
// the shadow material blurs a copy of the stencil into a soft dark patch.
// Resolves of the depth/stencil surface make that copy.
//
// REFLECTIONS (docs/findings/13): reflective surfaces (xnReflectShader) are lit
// like the lit materials and add a "sphere map" of their surroundings; they
// use the same LitParams block.
//
// PARTICLES AND WATER (docs/findings/14): both read the scene's DEPTH through
// a depth resolve (soft particles fade where they meet the scene; water
// checks it so its wobbly see-through look never pulls in things standing
// in front of it), and water also a colour resolve of the scene behind it
// and the planar reflection picture: textures the game makes mid-frame,
// which the render targets stand in for. Each has its own block
// (ParticleParams, WaterParams).
//
// DEPTH OF FIELD (docs/findings/15): one full-screen draw in the
// post-processing, reading a colour resolve of the picture so far and the
// scene's depth; its settings are in a DofParams block.
//
// FX GRID (docs/findings/18): full-screen batches in the post-processing
// that redraw a colour resolve of the picture so far, plus or times their
// vertex colours (the double mojo flash). Push constants, like the simple
// material.
//
// BUMP MEGA (docs/findings/19): bumpy meshes of the later areas: a normal
// map, a four-way shine, and optionally a reflection, a see-through look (a
// colour resolve of the picture so far, shifted by the bumps) and a rim
// glow. Textures like water's (D3D's sampler copy), a BumpMegaParams block.
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace native {

// Where the components of one vertex sit (byte offsets), for Vulkan's vertex
// input. kAbsent = the vertex doesn't have it; the shader then uses a default
// (the attribute still reads offset 0, harmlessly, because Vulkan wants every
// attribute the shader declares to be fed).
//
// Every component is made of 32-bit words (floats, or a D3DCOLOR), so the
// game's big-endian vertices become host vertices by swapping the bytes of
// each word, without knowing the layout: that's what both the buffer cache
// (meshes) and the recorder (immediate geometry) do. A D3DCOLOR 0xAARRGGBB
// then has the bytes B, G, R, A in memory: read as VK_FORMAT_B8G8R8A8_UNORM.
struct VertexLayout {
  static constexpr uint8_t kAbsent = 0xFF;
  uint8_t stride = 0;
  uint8_t normal = kAbsent;    // 3 floats
  uint8_t colour = kAbsent;    // D3DCOLOR (4 bytes)
  uint8_t uv = kAbsent;        // 2 floats (the first texture coordinate set)
  uint8_t binormal = kAbsent;  // 3 floats (normal-mapped characters)
  uint8_t tangent = kAbsent;   // 3 floats
  // Position is always 3 floats at offset 0.
};

// The layout of a mesh vertex from its PDDI vertex format flags, the way the
// game builds its D3D vertex declaration (0x8243CF28). Components in order:
//   position     3 floats (always)
//   0x100        a second position, 3 floats (morph target?)
//   0x80         blend indices, 4 bytes (skinning)
//   0x10         normal, 3 floats
//   0x20         colour 0, D3DCOLOR
//   0x4000       colours 0 and 1, 2 x D3DCOLOR
//   0x40         colour 1, D3DCOLOR
//   0x0F (count) texture coordinate sets, 2 floats each
//   0x200, 0x400 one float each ("point size" usage)
//   0x800        binormal, 3 floats
//   0x1000       tangent, 3 floats
// (0x2000 adds nothing to the declaration.) `stride` is the mesh's own.
VertexLayout MeshVertexLayout(uint32_t format_flags, uint32_t stride);

// A texture the game drew with: the 6 words of its GPU fetch constant, read
// from the D3D texture object (host byte order). The texture cache decodes
// size, format, tiling and address from these. All zero = no texture.
struct GuestTexture {
  std::array<uint32_t, 6> fetch{};
  bool present() const { return fetch[1] != 0; }  // word 1 holds the address
};

// A vertex or index buffer in guest memory (a mesh's data, see buffer_cache.h).
struct GuestBuffer {
  uint32_t address = 0;  // guest virtual address; 0 = none
  uint32_t size = 0;     // bytes
  // How to turn the big-endian data into host order: the GPU's endian mode
  // for vertex data (xenos::Endian: 0 none, 1 8in16, 2 8in32, 3 16in32),
  // or 1 (swap each 16-bit value) for 16-bit indices.
  uint8_t endian = 0;
  bool present() const { return address != 0 && size != 0; }
};

// PDDI primitive types (BeginPrims r5), as the game numbers them.
enum class Topology : uint8_t {
  kTriangleList = 0,
  kTriangleStrip = 1,
  kLineList = 2,
  kLineStrip = 3,
  kPointList = 4,
};

// Which of the game's materials (xn*Shader classes) a draw uses; each has
// its own pair of shaders on our side.
enum class Material : uint8_t {
  kSimple,     // xnSimpleShader: vertex colour x texture (shaders/simple.*)
  kBink,       // xnBinkShader: a movie frame, 3 planes Y/Cr/Cb -> RGB (shaders/bink.*)
  kLitSimple,  // xnSimpleShader, "lit" variant: lights instead of vertex colours (shaders/lit.*)
  kCharacter,  // xnCharacterRimShader: Crash and co. (shaders/character.*)
  kShadow,     // xnShadowShader: the soft shadow under characters (shaders/shadow.*)
  kReflect,    // xnReflectShader: lit + a reflection of the surroundings (shaders/reflect.*)
  kParticle,   // xnParticleShader: sprites that fade near the scene's depth (shaders/particle.*)
  kWater,      // xnWaterSpecularShader: water surfaces (shaders/water.*)
  kDof,        // xnDOFShader: depth of field, one full-screen pass (shaders/dof.*)
  kFxGrid,     // xnFxGridShader: screen-wide glow / tint grids (simple.vert + fxgrid.frag)
  kBumpMega,   // xnBumpMegaShader: normal-mapped, see-through surfaces (shaders/bumpmega.*)
};

// How a texture is sampled, from the sampler's half of the GPU fetch
// constant D3D programmed (guest.h, device::SamplerFetch; the fields of
// xenos::xe_gpu_texture_fetch_t, words 0, 3 and 4):
//   address_u/v   xenos::ClampMode per axis (0 repeat, 1 mirror, 2 clamp
//                 to edge...)
//   linear        magnification bilinear (else point)
//   min_linear    minification bilinear (else point)
//   mip           how mip levels are used (docs/findings/17): 0 base level
//                 only (the "base map" mode, or no mips), 1 the nearest
//                 level, 2 a blend of the two nearest ("trilinear")
//   anisotropy    anisotropic filtering: the most samples along a slanted
//                 surface, 1, 2, 4, 8 or 16 (0 = off); the 3D world uses 16
//   min_level     the most detailed level allowed (0 = full size)
//   lod_bias      shifts the level choice, in 1/32 of a level
struct SamplerState {
  uint8_t address_u = 2, address_v = 2;
  bool linear = true;
  bool min_linear = true;
  uint8_t mip = 0;
  uint8_t anisotropy = 0;
  uint8_t min_level = 0;
  int16_t lod_bias = 0;
  // The texture must be read one texel at a time (palettized indices, the
  // depth copy on a GPU that can't filter it): point, base level only.
  SamplerState Unfiltered() const {
    SamplerState s = *this;
    s.linear = s.min_linear = false;
    s.mip = 0;
    s.anisotropy = 0;
    return s;
  }
};

// Everything the lit materials (Material::kLitSimple, kCharacter, kReflect)
// need per draw, in the memory layout of a GLSL std140 uniform block
// (shaders/lit_params.glsl declares the same thing): only 16-byte vectors and
// 4x4 matrices, so the C++ and GLSL layouts agree without padding rules. It
// is copied into the frame's uniform buffer as is. The recorder fills it
// from D3D's copy of the shader constants (register numbers in brackets:
// VS = vertex shader, PS = pixel shader; the names are what the game's
// shaders call them, from their constant tables), except `texture_sizes`,
// which the renderer fills once it has the textures.
struct alignas(16) LitParams {
  float mvp[16];          // VS c4-c7: model-view-projection, column-major
  float world[16];        // VS c12-c15: model -> world
  float rotation[16];     // VS c0-c3: rotates normals into world space (3x3 used)
  float eye[4];           // camera position (lit simple: VS c8; character: PS c128)
  float ambient[4];       // PS c8: the scene's ambient light
  // 4 directional lights (PS c9-c16: direction, colour, direction, colour...).
  // The direction is the way the light travels (the shaders use -direction).
  float light_direction[4][4];
  float light_colour[4][4];
  // The surface's reaction to light (PS c64-c68).
  float surface_diffuse[4];   // c64
  float surface_specular[4];  // c65 (character: .x = amount)
  float surface_emissive[4];  // c66 (lit simple; also its alpha in .w)
  float surface_ambient[4];   // c67
  float specular_power[4];    // c68.x
  // "Proximity" point lights (PS c29.x count, c30-c33 positions, c34-c37
  // colours, c38-c41: x brightness, y radius, z intensity).
  float point_position[4][4];
  float point_colour[4][4];
  float point_params[4][4];
  // "Fx shadows": up to 8 points (PS c200.x count, c201-c208) that darken
  // what's near them, e.g. under a character.
  float shadow_spheres[8][4];
  // PS c100. Character material: the rim light colour; reflect material:
  // the reflection's tint ("EnvColour"). Character material only, then
  // x pulse (c98.x: a yellow flash), y blend weight towards the second
  // texture (c99.x), z rim blend (c101.x), w rim amount (c102.x).
  float rim_colour[4];
  float character[4];
  // Fog colour (PS c70) and x fog start (c71.x), y end (c72.x), z fade (c2.x),
  // w alpha test reference (0..1).
  float fog_colour[4];
  float fog_fade_alpha[4];
  // Texel sizes of the palettized textures: xy diffuse, zw normal map.
  float texture_sizes[4];
  // x: bit i = directional light i on (PS bools b0-b3); y: point lights;
  // z: shadow spheres; w: kLit* flags below.
  uint32_t counts[4];
  // x: alpha test compare function (Xbox numbering, 7 = always).
  uint32_t more[4];
  float padding[4 * 10];  // up to 1024 bytes: see the static_assert
};
// A multiple of 256 bytes, the largest offset alignment Vulkan allows for
// uniform buffers: a frame's blocks can be uploaded as one array and each
// bound at index * sizeof.
static_assert(sizeof(LitParams) == 1024);
// LitParams::counts[3] (same bits in shaders/lit_params.glsl).
constexpr uint32_t kLitTextured = 1;          // lit simple: has a texture (PS c20.x > 0)
constexpr uint32_t kLitPalettized = 2;        // the (diffuse) texture is 8-bit palettized
constexpr uint32_t kLitNormalPalettized = 4;  // character: the normal map is palettized
constexpr uint32_t kLitFog = 8;               // distance fog (PS bool b69)
constexpr uint32_t kLitFadeRgb = 16;          // additive / subtractive: fade the colour too
constexpr uint32_t kLitAlphaTest = 32;
constexpr uint32_t kLitBlendMap = 64;         // character: a second texture is bound
constexpr uint32_t kLitNoUv = 128;            // the vertex has no texture coordinates
constexpr uint32_t kLitNoNormal = 256;        // ... no normal
constexpr uint32_t kLitNoTangents = 512;      // ... no tangent / binormal
constexpr uint32_t kLitNoNormalMap = 1024;    // character without a normal map: flat
constexpr uint32_t kLitVertexColour = 2048;   // reflect: use the vertex colour (PS bool b10)
constexpr uint32_t kLitNoColour = 4096;       // ... the vertex has no colour (white)

// The shadow material's parameters (Material::kShadow, shaders/shadow.*),
// std140 like LitParams. The game's pixel shader blurs a copy of the stencil
// buffer with 9 taps: offsets in texture coordinates (PS c2-c10, .xy) and
// per-channel weights (PS c30-c38). See findings/12.
struct alignas(16) ShadowParams {
  float mvp[16];          // VS c4-c7
  float offsets[9][4];    // xy used
  float weights[9][4];
  float padding[256 - 16 - 72];
};
static_assert(sizeof(ShadowParams) == 1024);

// The particle material's parameters (Material::kParticle,
// shaders/particle_params.glsl), std140. Register numbers are the game's
// (VS = vertex shader, PS = pixel shader), names from its constant table.
struct alignas(16) ParticleParams {
  float mvp[16];          // VS c4-c7
  // x Fade (PS c2.x), y Falloff (PS c4.x: depth distance of the soft edge),
  // zw NearFar (PS c73.xy: turns a stored depth into a distance).
  float fade_falloff_near_far[4];
  float fog_colour[4];    // PS c70 (rgb); w alpha test reference (0..1)
  // x fog start (PS c71.x), y fog end (c72.x), zw the texture's size in
  // texels (palettized filtering; the game's c112 "BilinearScale").
  float fog_range_texture_size[4];
  uint32_t flags[4];      // x kParticle* flags, y alpha test compare function
  float padding[256 - 16 - 16];
};
static_assert(sizeof(ParticleParams) == 1024);
// ParticleParams::flags[0] (same bits in shaders/particle_params.glsl).
constexpr uint32_t kParticleTextured = 1;
constexpr uint32_t kParticlePalettized = 2;
constexpr uint32_t kParticleFadeRgb = 4;    // additive / subtractive (PS b80 / b81)
constexpr uint32_t kParticleFog = 8;        // PS b69
constexpr uint32_t kParticleAlphaTest = 16;
constexpr uint32_t kParticleNoColour = 32;  // the vertex has no colour: white
constexpr uint32_t kParticleNoUv = 64;
constexpr uint32_t kParticleSoft = 128;     // the depth copy is bound
constexpr uint32_t kParticleHighlight = 256;  // debug: F11 paints it magenta (spotter.h)
// Debug experiments (F12, spotter.h): skip one step of the particle shader.
constexpr uint32_t kParticleNoSoft = 512;       // soft edge off (soft = 1)
constexpr uint32_t kParticleNoFog = 1024;       // fog off
constexpr uint32_t kParticleWhiteTex = 2048;    // texture = white
constexpr uint32_t kParticleWhiteColour = 4096; // vertex colour = white
constexpr uint32_t kParticleBaseMip = 8192;      // sample mip level 0 only
constexpr uint32_t kParticleSmallestMip = 16384; // sample the smallest mip level only

// The water material's parameters (Material::kWater, shaders/water_params.glsl),
// std140, registers and names as in guest.h (shader::kWaterSpecularShaderVtable).
struct alignas(16) WaterParams {
  float mvp[16];              // VS c4-c7
  float world[16];            // VS c12-c15: model -> world
  float camera[4];            // PS c128 CameraPos
  float view_direction[4];    // PS c129 ViewDirection
  float reflect_colour[4];    // PS c102 ReflectColor (RGBA)
  // x ReflectBumpiness (PS c100.x), y WaterBumpiness (c101.x), z Blend
  // (c25.x: between the two animation frames), w PixelHeight (c23.x).
  float bumpiness_blend_pixel[4];
  float near_far[4];          // PS c73 NearFar (xy)
  float fog_colour[4];        // PS c70
  float fog_fade[4];          // x fog start (c71.x), y end (c72.x), z Fade (c2.x)
  float shadow_spheres[8][4]; // PS c201-c208 FxShadowPositions (xyz)
  uint32_t flags[4];          // x kWater* flags, y Fx shadow count (c200.x)
  float padding[256 - 16 - 16 - 4 * 7 - 32 - 4];
};
static_assert(sizeof(WaterParams) == 1024);
// WaterParams::flags[0] (same bits in shaders/water_params.glsl).
constexpr uint32_t kWaterReflection = 1;  // reflection on (PS b10 off, map + mask bound)
constexpr uint32_t kWaterFog = 2;         // PS b69
constexpr uint32_t kWaterNoColour = 4;    // the vertex has no colour: white
constexpr uint32_t kWaterNoUv = 8;

// The depth of field's parameters (Material::kDof, shaders/dof_params.glsl),
// std140, registers and names as in guest.h (shader::kDofShaderVtable).
// Distances are the game's world units, like NearFar's.
struct alignas(16) DofParams {
  float mvp[16];              // VS c4-c7 (the full-screen quad's)
  // xy PixelSizeHigh (PS c1.xy: 1 / screen size), zw MaxRadius (PS c5.xy).
  float pixel_size_max_radius[4];
  // x FocalPlaneDistance (PS c2.x), y NearBlurPlaneDistance (c3.x),
  // z FarBlurPlaneDistance (c4.x).
  float planes[4];
  float near_far[4];          // PS c73 NearFar (xy)
  uint32_t flags[4];          // x kDof* flags
  float padding[256 - 16 - 16];
};
static_assert(sizeof(DofParams) == 1024);
// DofParams::flags[0] (same bits in shaders/dof_params.glsl).
constexpr uint32_t kDofNearBlur = 1;  // PS bool b10 bEnableNearBlur

// The bump mega material's parameters (Material::kBumpMega,
// shaders/bumpmega_params.glsl), std140, registers and names as in guest.h
// (shader::kBumpMegaShaderVtable).
struct alignas(16) BumpMegaParams {
  float mvp[16];              // VS c4-c7 matMVP
  float world[16];            // VS c12-c15 matWorld: model -> world
  float rotation[16];         // VS c0-c3 matRot: normals -> world
  float camera[4];            // PS c128 CameraPos (xyz)
  float rim_colour[4];        // PS c100 RimColor
  float reflect_colour[4];    // PS c101 ReflectColor (RGBA)
  // x RefractBumpiness (c103.x), y ReflectBumpiness (c104.x), z TileWidth
  // (c105.x), w RefractBrightness (c106.x).
  float bump[4];
  // xy BilinearScale (c112.xy: the palettized diffuse's size), zw
  // BilinearScale2 (c113.xy: the palettized normal map's).
  float palette_sizes[4];
  float fog_colour[4];        // PS c70
  // x fog start (c71.x), y end (c72.x), z Fade (c2.x), w alpha test
  // reference (0..1, the state cache's)
  float fog_fade[4];
  float shadow_spheres[8][4]; // PS c201-c208 FxShadowPositions (xyz)
  // x kBumpMega* flags, y Fx shadow count (c200.x), z alpha test compare
  // function (xenos::CompareFunction; 7 = always, the test off)
  uint32_t flags[4];
  float padding[256 - 16 * 3 - 4 * 7 - 32 - 4];
};
static_assert(sizeof(BumpMegaParams) == 1024);
// BumpMegaParams::flags[0] (same bits in shaders/bumpmega_params.glsl): the
// game's PS bools b10-b16 (guest.h), plus what the vertex lacks.
constexpr uint32_t kBumpMegaRim = 1;               // b10 IsEnableRim
constexpr uint32_t kBumpMegaRefract = 2;           // b11 IsEnableRefract (and the copy is there)
constexpr uint32_t kBumpMegaReflect = 4;           // b12 IsEnableReflect (and the map is there)
constexpr uint32_t kBumpMegaReflectMask = 8;       // b13 IsUseReflectMask
constexpr uint32_t kBumpMegaPalette = 16;          // b14 IsPaletteTexture (diffuse)
constexpr uint32_t kBumpMegaPalette2 = 32;         // b15 IsPaletteTexture2 (normal map)
constexpr uint32_t kBumpMegaRefractModulated = 64; // b16 IsRefractColourModulated
constexpr uint32_t kBumpMegaFog = 128;             // b69 FogEnabled
constexpr uint32_t kBumpMegaNoColour = 256;        // the vertex has no colour: white
constexpr uint32_t kBumpMegaNoNormalMap = 512;     // no normal map (or no tangents): flat
constexpr uint32_t kBumpMegaNoUv = 1024;           // the vertex has no texture coordinates
constexpr uint32_t kBumpMegaHighlight = 2048;      // debug: F11 paints it magenta (spotter.h)

// One draw's slot in the frame's uniform buffer (binding 9): its layout
// depends on the material.
union MaterialBlock {
  LitParams lit;
  ShadowParams shadow;
  ParticleParams particle;
  WaterParams water;
  DofParams dof;
  BumpMegaParams bump_mega;
};
static_assert(sizeof(MaterialBlock) == 1024);

// Blending, in the Xbox GPU's numbering (xenos::BlendFactor / BlendOp): the
// values the game's material code hands to D3D.
struct Blend {
  bool enable = false;
  uint8_t op = 0;   // 0 add, 1 subtract, 2 min, 3 max, 4 reverse subtract
  uint8_t src = 1;  // 1 = one
  uint8_t dst = 0;  // 0 = zero
};

// Stencil test (guest.h, context::kStencilState), in the Xbox GPU's
// numbering, which Vulkan shares (VkCompareOp, VkStencilOp). "Front" and
// "back" as the Xbox GPU tells them apart, which depends on the cull mode:
// with D3D cull mode 6 clockwise triangles are the front, otherwise
// counter-clockwise ones (native_renderer.cpp, ToFacing). D3D puts its normal
// stencil state in the front slot and its "CCW" two-sided state in the back
// slot; without two-sided, both use front's.
struct Stencil {
  bool enable = false;
  bool two_sided = false;
  uint8_t func = 7, fail = 0, depth_fail = 0, pass = 0;              // front
  uint8_t back_func = 7, back_fail = 0, back_depth_fail = 0, back_pass = 0;
  uint8_t reference = 0, read_mask = 0xFF, write_mask = 0xFF;
};

// The D3D viewport (guest.h, context::kViewport): the rectangle of the render
// target drawn into, in pixels, and the depth range. The game's depth range
// is flipped (min_z 1, max_z 0); Vulkan accepts that as is.
struct Viewport {
  float x = 0, y = 0, width = 1280, height = 720;
  float min_z = 0, max_z = 1;
};

// A D3D surface: a render target in EDRAM. Identified by the address of the
// game's D3D surface object; described by the GPU registers it holds
// (guest.h, d3d_surface).
struct Surface {
  uint32_t guest = 0;  // the D3D surface object; 0 = none
  uint32_t width = 0, height = 0;
  bool depth = false;  // a depth/stencil surface
  uint8_t format = 0;  // xenos::ColorRenderTargetFormat / DepthRenderTargetFormat
  uint8_t msaa = 0;    // xenos::MsaaSamples: 0 = 1x, 1 = 2x, 2 = 4x
};

// The surfaces a draw or clear goes to (0 = none bound).
struct Targets {
  uint32_t colour = 0;
  uint32_t depth = 0;
  bool operator==(const Targets& o) const { return colour == o.colour && depth == o.depth; }
  bool operator!=(const Targets& o) const { return !(*this == o); }
};

struct DrawCommand {
  Material material = Material::kSimple;
  Topology topology = Topology::kTriangleList;

  // --- Geometry: one of two kinds ---
  // Immediate mode (BeginPrims/EndPrims, BeginIndexedPrims): `vertex_count`
  // vertices copied into Frame::vertex_data from byte `vertex_offset` on, and
  // for indexed draws `index_count` indices in Frame::indices from
  // `first_index` on.
  // Mesh (xnPrimBuffer::Draw): the game's own buffers, uploaded by the
  // buffer cache; drawn with `index_count` indices if there's an index
  // buffer, else `vertex_count` vertices from the start.
  // Both: the vertices' components as `layout` says.
  bool mesh = false;
  uint32_t vertex_offset = 0;
  uint32_t vertex_count = 0;
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  GuestBuffer vertex_buffer;  // mesh only
  GuestBuffer index_buffer;   // mesh only, 16-bit indices (none = not indexed)
  VertexLayout layout;

  // Lit, shadow, particle, water and depth of field: index of this draw's
  // MaterialBlock in Frame::blocks (else kNoBlock). The unlit simple
  // material gets one (a LitParams) only while proximity lights or Fx
  // shadows are on (recorder.cpp, ReadSimpleLights).
  static constexpr uint32_t kNoBlock = UINT32_MAX;
  uint32_t block = kNoBlock;

  // Model-view-projection matrix, column-major (the vertex shader's c4..c7).
  float mvp[16] = {};
  // Colour multiplier the game's vertex shader applies (c100.rgb).
  float tint[3] = {1, 1, 1};

  // Material ("simple" shader). The game's pixel shader computes
  //   colour = vertex colour (x texture, if textured)
  //   alpha *= fade; for additive/subtractive blending also rgb *= fade
  bool textured = false;  // pixel shader c20.x > 0
  float fade = 1;         // pixel shader c2.x: global fade x material alpha
  bool fade_rgb = false;  // blend mode 2 or 3 (pixel shader bools b80/b81)
  // Fx grid only: add the vertex colour to the texture (PS bool b10), else
  // multiply (guest.h, shader::kFxGridShaderVtable).
  bool fx_add = false;
  // Distance fog (guest.h, context::kFogState), applied by the game's pixel
  // shader as colour = mix(fog colour, colour, f), f = (end - d) / (end -
  // start) clamped to 0..1, d = the vertex's clip-space depth.
  bool fog = false;
  float fog_colour[3] = {};
  float fog_start = 0, fog_end = 1;
  GuestTexture texture;
  GuestTexture palette;  // for 8-bit palettized textures: 256x1 colours
  // How the texture is sampled (sampler 0 as D3D programmed the GPU).
  SamplerState texture_sampler;

  // Bink only. `texture` is the Y (brightness) plane, these the two colour
  // planes; `colour_matrix` = pixel shader c0..c3: rows of the YCrCb -> RGB
  // matrix with their offsets in .w (times c3.x), and the alpha in c3.w.
  GuestTexture chroma[2];  // Cr, Cb
  float colour_matrix[16] = {};

  // Character material only (`texture` + `palette` are its diffuse texture):
  // the normal map (sampler 1) and its palette if it's 8-bit palettized, and
  // a second texture the diffuse one can be blended towards (sampler 3; the
  // game calls it the "evil map", LitParams::character.y is the blend).
  GuestTexture normal_map, normal_palette, blend_map;
  SamplerState normal_sampler, blend_sampler;

  // Shadow material only: the texture holding a copy of the stencil buffer
  // (a depth/stencil resolve's destination; the render targets keep a
  // stencil image for it), and how it's sampled.
  GuestTexture stencil_copy;
  SamplerState stencil_sampler;

  // Reflect material only: the reflection texture ("sphere map", sampler 1).
  GuestTexture reflection;
  SamplerState reflection_sampler;

  // Particle material only (`texture` + `palette` are its sprite): the
  // scene's depth, a depth resolve's destination (the render targets keep a
  // depth copy for it; not present = drawn without the soft edge).
  // Depth of field: `texture` is the copy of the picture so far (sampler 0),
  // `depth_copy` the scene's depth (sampler 1), both required.
  // (The fx grid's `texture` is such a copy too; `fx_add` below.)
  GuestTexture depth_copy;
  SamplerState depth_sampler;

  // Water and bump mega: the nine textures on samplers 0-8, as D3D's copy of
  // the fetch constants holds them (addresses made virtual where readable,
  // guest::ReadableAddressOfPhysical), and how each is sampled. Index =
  // sampler (guest.h, shader::kWaterSpecularShaderVtable /
  // kBumpMegaShaderVtable).
  std::array<GuestTexture, 9> device_textures{};
  std::array<SamplerState, 9> device_samplers{};

  // --- Fixed-function state (what Vulkan bakes into pipelines) ---
  Blend blend;
  bool alpha_test = false;
  uint8_t alpha_func = 7;  // xenos::CompareFunction (7 = always)
  float alpha_ref = 0;     // 0..1
  bool depth_test = false;
  bool depth_write = false;
  uint8_t depth_func = 7;   // xenos::CompareFunction, already "reversed" (guest.h)
  uint8_t cull = 0;         // D3D cull mode: 0 none, 2 clockwise, 6 counter-clockwise
  uint8_t colour_mask = 0xF;  // bit 0 red .. bit 3 alpha
  Stencil stencil;
  Viewport viewport;

  // Where it draws.
  Targets targets;
};

// D3D's Clear (0x82322808), including the clears D3D's resolve can do after
// copying. Only render target 0 is kept (the game also clears a second
// target, which we don't have yet).
struct ClearCommand {
  bool colour = false, depth = false, stencil = false;
  float colour_value[4] = {};  // red, green, blue, alpha
  float depth_value = 0;       // as the depth buffer stores it (reversed: 0 = far)
  uint32_t stencil_value = 0;
  // Rectangles to clear in surface pixels (x0, y0, x1, y1); none = all of it.
  std::vector<std::array<int32_t, 4>> rects;
  Targets targets;
};

// D3D's Resolve (0x8231EE00): copy a rectangle of a surface into a texture.
struct ResolveCommand {
  uint32_t source = 0;  // the surface (Targets' colour or depth at the time)
  bool depth = false;   // resolving the depth surface (else colour target 0)
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // source rectangle, surface pixels
  int32_t dest_x = 0, dest_y = 0;          // where it lands in the texture
  GuestTexture dest;                       // the destination texture
  bool frontbuffer = false;  // EndFrame's resolve: this is the finished picture
  // Which samples of a multisampled surface (D3D's flags, bits 4-6): 0 =
  // none named (D3D then averages a colour surface's samples), 1-4 = only
  // sample 0-3, 5 = average of 0 and 1, 6 = of 2 and 3, 7 = of all four.
  // Depth is never averaged: the game asks for sample 0 (findings/16).
  uint8_t sample_select = 0;
};

struct Command {
  enum class Type : uint8_t { kClear, kDraw, kResolve };
  Type type;
  uint32_t index;  // into Frame::clears, Frame::draws or Frame::resolves
};

struct Frame {
  uint64_t number = 0;    // game frames since start
  bool recorded = false;  // false: recording was off, the lists are empty
  // Immediate-mode geometry of all draws, in host byte order (VertexLayout):
  // vertices as 32-bit words, and 16-bit indices.
  std::vector<uint32_t> vertex_data;
  std::vector<uint16_t> indices;
  std::vector<MaterialBlock> blocks;  // parameters of the draws with a block (DrawCommand::block)
  std::vector<DrawCommand> draws;
  std::vector<ClearCommand> clears;
  std::vector<ResolveCommand> resolves;
  std::vector<Command> commands;  // clears, draws and resolves, in the game's order
  std::vector<Surface> surfaces;  // every surface the commands use

  const Surface* FindSurface(uint32_t guest) const {
    for (const Surface& s : surfaces) {
      if (s.guest == guest) return &s;
    }
    return nullptr;
  }

  void Reset() {
    recorded = false;
    vertex_data.clear();
    indices.clear();
    blocks.clear();
    draws.clear();
    clears.clear();
    resolves.clear();
    commands.clear();
    surfaces.clear();
  }
};

}  // namespace native
