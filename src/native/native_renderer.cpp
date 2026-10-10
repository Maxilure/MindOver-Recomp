// =============================================================================
// native/native_renderer.cpp -- see native_renderer.h for the why and how
// =============================================================================

#include "native_renderer.h"


#include <algorithm>
#include <cstdlib>
#include <string>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>

#include <rex/graphics/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/vulkan/util.h>

#include "../pddi/intercept.h"
#include "../pddi/trace.h"
#include "ab_capture.h"
#include "guest.h"
#include "recorder.h"
#include "spotter.h"
#include "../overlay_banner.h"

REXCVAR_DEFINE_STRING(renderer, "emulated", "CrashMoM",
                      "Picture shown at start: 'emulated' (ReXGlue's GPU emulation) or "
                      "'native' (our Vulkan renderer, in development). F9 switches");
// NATIVE ONLY (2026-09-30, experimental): a preview of
// the finish line, where our renderer replaces the emulated GPU. The emulated
// GPU normally keeps drawing every frame even while nobody sees its picture,
// so the native picture paid for BOTH renderers (the GPU was the bottleneck:
// SwapBuffers waited ~15 ms/frame in the Ratcicle Kingdom). With this flag the
// emulated GPU skips all its draws and resolves (SDK patch 0009, GPU flag
// "skip_draws") whenever the main window shows our picture; F9 back to the
// emulated picture turns it on again (its first frame or two can show stale
// leftovers). Dual mode keeps it on (its picture is in the main window).
// Known gaps: an F10 photo holds our picture only (ab_capture.h, NATIVE
// ONLY; until 2026-10-01 it froze the game 3 s waiting for the emulated
// one), and anything the game READS BACK from an emulated resolve (none
// known) would go stale.
REXCVAR_DEFINE_BOOL(native_only, true, "CrashMoM",
                    "Start on the native picture and stop the emulated GPU's drawing while "
                    "the native picture is in the main window (F9 back turns it on again)");
// EMULATED ONLY (2026-09-30, the mirror image of --native_only): the plain
// emulated game as a clean baseline to compare
// with. Starts on the emulated picture and locks it there: F9 and F8 (dual
// mode) do nothing, so our renderer never draws (it only records a frame
// while its picture is on a screen). What stays: the hooks watching the
// game's renderer calls (a few function calls per draw, nothing drawn) and
// F10 photos of the screen.
REXCVAR_DEFINE_BOOL(emulated_only, false, "CrashMoM",
                    "Only the emulated GPU's picture: F9 / F8 locked, the native renderer never "
                    "draws. The baseline to compare --native_only with");
// EMULATED EVERY NTH FRAME (2026-10-03): with both renderers drawing (dual
// mode, or our picture in the main window without --native_only), the host
// GPU was ~97% busy at 60 fps in a quiet spot of the Ice Prison and busier
// scenes dropped frames, which made dual-mode testing painful. The emulated
// GPU now draws (and presents) only every Nth frame (SDK patch 0013, GPU flag
// draw_every_nth_frame): each frame it does draw is complete and exactly the
// original look, its window just moves at 1/N of the game's frame rate. The
// game and our picture are untouched. Photos (F10) and A/B captures put it
// back to every frame while they run (ab_capture::HoldEmulatedEveryFrame),
// so a pair is always the same frame from both renderers. 1 = old behaviour.
// The emulated picture alone (no native picture anywhere) always draws
// every frame.
REXCVAR_DEFINE_INT32(emulated_draw_every, 2, "CrashMoM",
                     "While our renderer draws too (dual mode, or the native picture): the "
                     "emulated GPU draws every Nth frame (1 = every frame, 2 = half the work)")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// How we found the 25% darkening of the hub (docs/findings/10): with the
// emulated GPU's own resolves (the scene it rendered) fed into our later
// passes, our picture matched the emulated one exactly, which pinned the
// difference on our 3D pass (a stencil-masked quad drawn without stencil).
REXCVAR_DEFINE_BOOL(debug_native_emulated_resolves, false, "CrashMoM",
                    "Debug: the native renderer samples the emulated GPU's resolved textures "
                    "instead of its own. Needs --readback_resolve=full (the emulator then copies "
                    "each resolve back into guest memory)");
// Texture write watch (write_watch.h, docs/findings/20): on = a texture is
// re-hashed only when its memory was written. Off = every used texture is
// hashed every frame (the old way): to measure the difference, or to rule
// the watch out if a texture ever looks stale.
REXCVAR_DEFINE_BOOL(debug_native_write_watch, true, "CrashMoM",
                    "Native renderer: re-check a texture only when the game wrote to its memory "
                    "(off: hash every texture every frame)");

namespace {

// SPIR-V of src/native/shaders/*, compiled by glslc at build time
// (CMakeLists.txt, into <build>/shaders/). Each file is "{0x07230203,...}".
constexpr uint32_t kSimpleVs[] =
#include "simple.vert.h"
    ;
constexpr uint32_t kSimpleFs[] =
#include "simple.frag.h"
    ;
constexpr uint32_t kBinkVs[] =
#include "bink.vert.h"
    ;
constexpr uint32_t kBinkFs[] =
#include "bink.frag.h"
    ;
constexpr uint32_t kLitVs[] =
#include "lit.vert.h"
    ;
constexpr uint32_t kLitFs[] =
#include "lit.frag.h"
    ;
constexpr uint32_t kCharacterVs[] =
#include "character.vert.h"
    ;
constexpr uint32_t kCharacterFs[] =
#include "character.frag.h"
    ;
constexpr uint32_t kShadowVs[] =
#include "shadow.vert.h"
    ;
constexpr uint32_t kShadowFs[] =
#include "shadow.frag.h"
    ;
constexpr uint32_t kReflectVs[] =
#include "reflect.vert.h"
    ;
constexpr uint32_t kReflectFs[] =
#include "reflect.frag.h"
    ;
constexpr uint32_t kParticleVs[] =
#include "particle.vert.h"
    ;
constexpr uint32_t kParticleFs[] =
#include "particle.frag.h"
    ;
constexpr uint32_t kWaterVs[] =
#include "water.vert.h"
    ;
constexpr uint32_t kWaterFs[] =
#include "water.frag.h"
    ;
constexpr uint32_t kDofVs[] =
#include "dof.vert.h"
    ;
constexpr uint32_t kDofFs[] =
#include "dof.frag.h"
    ;
constexpr uint32_t kFxGridFs[] =
#include "fxgrid.frag.h"
    ;
constexpr uint32_t kBumpMegaVs[] =
#include "bumpmega.vert.h"
    ;
constexpr uint32_t kBumpMegaFs[] =
#include "bumpmega.frag.h"
    ;
constexpr uint32_t kPresentVs[] =
#include "present.vert.h"
    ;
constexpr uint32_t kPresentFs[] =
#include "present.frag.h"
    ;

// Push constants of the simple material: same layout as SimpleParams in
// shaders/simple_params.glsl.
struct SimpleParams {
  float mvp[16];
  float tint_fade[4];
  float texture_size_fog_range[4];     // xy texture size, zw fog start / end
  float alpha_ref_fog_colour[4];       // x alpha reference, yzw fog colour
  uint32_t flags[4];
};
static_assert(sizeof(SimpleParams) == 128, "the push constant size every GPU supports");
constexpr uint32_t kFlagTextured = 1, kFlagPalettized = 2, kFlagFadeRgb = 4, kFlagAlphaTest = 8;
// The vertex has no colour (white is used) / no texture coordinates (0, 0).
constexpr uint32_t kFlagNoColour = 16, kFlagNoUv = 32;
constexpr uint32_t kFlagFog = 64;  // distance fog on (simple_params.glsl)
constexpr uint32_t kFlagFxAdd = 128;  // fx grid: add the colour, else multiply (fxgrid.frag)
// Proximity lights / Fx shadows on: they're in the draw's LitParams block
// (the recorder gives a simple draw one only then; simple_params.glsl).
constexpr uint32_t kFlagLights = 256;

// Push constants of the Bink material (shaders/bink.*): the game's movie
// pixel shader constants c0..c3, and the game's screen size (its movie quad
// is in screen pixels).
struct BinkParams {
  float c[16];
  float screen_size[4];
};
// Texture bindings of a draw (the same set layout for every material):
//   simple:     0 texture, 1 its palette
//   Bink:       0 Y, 1 Cr, 2 Cb (the movie's planes)
//   lit simple: 0 texture, 3 its palette
//   character:  0 diffuse, 1 normal map, 2 blend ("evil") map,
//               3 diffuse palette, 4 normal-map palette
//   shadow:     0 the stencil copy
//   reflect:    0 texture, 1 the reflection (sphere map), 3 texture's palette
//   particle:   0 texture, 1 the scene's depth copy, 8 texture's palette
//               (the game's own sampler numbers)
//   water:      0-8 = the game's samplers 0-8 (guest.h: diffuse, distortion,
//               scene copy, depth copy, normal map, 2 animation frames,
//               reflection picture, reflection mask)
//   depth of field: 0 the picture so far, 1 the scene's depth copy (the
//               game's sampler numbers)
//   fx grid:    0 the picture so far (the game's sampler 0)
//   bump mega:  0-8 = the game's samplers (guest.h: diffuse, normal map,
//               reflection, its mask, the picture so far, -, -, the two
//               palettes)
// Unused bindings get the 1x1 white texture. Binding 9 is the draw's
// MaterialBlock (frame.h: LitParams, shaders/lit_params.glsl, for the lit,
// character and reflect materials; ShadowParams, shaders/shadow.*;
// ParticleParams, shaders/particle_params.glsl; WaterParams,
// shaders/water_params.glsl; DofParams, shaders/dof_params.glsl). It was
// binding 5 until water needed 9 textures.
constexpr uint32_t kTexturesPerDraw = 9;
constexpr uint32_t kBlockBinding = 9;
// Uniform buffer blocks must start at a multiple of the GPU's
// minUniformBufferOffsetAlignment, which Vulkan caps at 256.
constexpr size_t kUniformAlignment = 256;
static_assert(sizeof(native::MaterialBlock) % kUniformAlignment == 0);

// A material's name for the log (the game's class).
const char* MaterialName(native::Material m) {
  switch (m) {
    case native::Material::kSimple: return "xnSimpleShader";
    case native::Material::kBink: return "xnBinkShader";
    case native::Material::kLitSimple: return "xnSimpleShader (lit)";
    case native::Material::kCharacter: return "xnCharacterRimShader";
    case native::Material::kShadow: return "xnShadowShader";
    case native::Material::kReflect: return "xnReflectShader";
    case native::Material::kParticle: return "xnParticleShader";
    case native::Material::kWater: return "xnWaterSpecularShader";
    case native::Material::kDof: return "xnDOFShader";
    case native::Material::kFxGrid: return "xnFxGridShader";
    case native::Material::kBumpMega: return "xnBumpMegaShader";
  }
  return "?";
}

// The materials whose parameters are in a MaterialBlock (no push constants).
bool UsesBlock(native::Material m) {
  return m == native::Material::kLitSimple || m == native::Material::kCharacter ||
         m == native::Material::kShadow || m == native::Material::kReflect ||
         m == native::Material::kParticle || m == native::Material::kWater ||
         m == native::Material::kDof || m == native::Material::kBumpMega;
}

// --- Xbox GPU enums -> Vulkan (numbering from the SDK's xenos.h) -------------

VkBlendFactor ToBlendFactor(uint8_t factor) {
  switch (factor) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp ToBlendOp(uint8_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

VkPrimitiveTopology ToTopology(native::Topology topology) {
  switch (topology) {
    case native::Topology::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case native::Topology::kLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case native::Topology::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case native::Topology::kPointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

// xenos::ClampMode -> Vulkan. The "mirror once" and "halfway" variants need
// extensions or don't exist; the nearest plain mode is close enough for now.
VkSamplerAddressMode ToAddressMode(uint8_t clamp) {
  switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 6:
    case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

// The emulated GPU's copy of the display gamma table. The SDK only exposes
// it to CommandProcessor subclasses (a protected getter). A pointer to that
// member, formed inside a subclass, is an ordinary member-function pointer
// that may then be called on any CommandProcessor: C++'s sanctioned way to
// read a protected member from outside, without patching the SDK. Nothing
// ever creates a GammaRampReader.
struct GammaRampReader : rex::graphics::CommandProcessor {
  static const uint32_t* Table(const rex::graphics::CommandProcessor* gpu) {
    const auto getter = &GammaRampReader::gamma_ramp_256_entry_table;
    return reinterpret_cast<const uint32_t*>((gpu->*getter)());
  }
  // The brightness curve the emulated GPU applies (SDK patch 0015; static,
  // protected, so reached from here too).
  static uint32_t WithPower(uint32_t entry, double power) {
    return GammaRampEntryWithPower(entry, power);
  }
};

// D3D cull mode -> Vulkan culling AND which side is the front.
//
// The Xbox's D3D cull values are the GPU's PA_SU_SC_MODE_CNTL bits: bit 1 =
// cull back faces, bit 2 = "clockwise triangles are the front" (else
// counter-clockwise ones are). So 0 = no culling with counter-clockwise
// front faces, 2 = cull back faces with counter-clockwise front faces (so
// clockwise triangles vanish), 6 = cull back faces with clockwise front faces
// (counter-clockwise ones vanish). Which side is the front also decides
// which half of the two-sided stencil state a triangle uses (frame.h,
// Stencil): found with the soft character shadow, whose full-screen quad
// is clockwise, so with culling off it's a BACK face on the Xbox and uses
// the back-face stencil test (findings/12). Before that fix we always took
// clockwise as the front: culling came out the same, the stencil sides not.
//
// Our vertex shaders flip Y (D3D clip space points up, Vulkan's down), which
// makes Vulkan's framebuffer match the Xbox's screen orientation, so Vulkan's
// "clockwise" is clockwise as seen on screen, like the Xbox's.
struct Facing {
  VkCullModeFlags cull;
  VkFrontFace front;
};
Facing ToFacing(uint8_t d3d_cull) {
  const VkCullModeFlags cull = (d3d_cull & 2) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  const VkFrontFace front =
      (d3d_cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  return {cull, front};
}

// NativeRenderer::Cost: adds the time until it goes out of scope to `sum_ms`.
using CostClock = std::chrono::steady_clock;
double MsSince(CostClock::time_point start) {
  return std::chrono::duration<double, std::milli>(CostClock::now() - start).count();
}
struct CostTimer {
  explicit CostTimer(double& sum_ms) : sum(sum_ms), start(CostClock::now()) {}
  ~CostTimer() { sum += MsSince(start); }
  double& sum;
  CostClock::time_point start;
};

}  // namespace

// xenos::CompareFunction and VkCompareOp use the same numbering (0 never,
// 1 less, 2 equal, 3 less-equal, 4 greater, 5 not-equal, 6 greater-equal,
// 7 always), so depth functions pass through unchanged.
static_assert(VK_COMPARE_OP_GREATER == 4 && VK_COMPARE_OP_ALWAYS == 7);

NativeRenderer::PipelineKey NativeRenderer::MakePipelineKey(const native::DrawCommand& d,
                                                            bool has_depth,
                                                            VkSampleCountFlagBits samples) {
  PipelineKey key{};
  key.samples = uint8_t(samples);
  key.material = uint8_t(d.material);
  key.topology = uint8_t(d.topology);
  if (d.blend.enable) {
    key.blend_enable = 1;
    key.blend_op = d.blend.op;
    key.blend_src = d.blend.src;
    key.blend_dst = d.blend.dst;
  }
  key.depth_test = d.depth_test && has_depth;
  // Vulkan (like D3D) only writes depth where the depth test is on.
  key.depth_write = key.depth_test && d.depth_write;
  key.depth_func = key.depth_test ? d.depth_func : 7;
  key.cull = d.cull;
  key.colour_mask = d.colour_mask & 0xF;
  // D3D can't test stencil without a depth/stencil surface either.
  if (d.stencil.enable && has_depth) {
    const native::Stencil& st = d.stencil;
    key.stencil_enable = 1;
    key.stencil_front[0] = st.func & 7;
    key.stencil_front[1] = st.fail & 7;
    key.stencil_front[2] = st.depth_fail & 7;
    key.stencil_front[3] = st.pass & 7;
    key.stencil_back[0] = st.back_func & 7;
    key.stencil_back[1] = st.back_fail & 7;
    key.stencil_back[2] = st.back_depth_fail & 7;
    key.stencil_back[3] = st.back_pass & 7;
  }
  key.stride = d.layout.stride;
  key.normal_offset = d.layout.normal;
  key.colour_offset = d.layout.colour;
  key.uv_offset = d.layout.uv;
  key.binormal_offset = d.layout.binormal;
  key.tangent_offset = d.layout.tangent;
  return key;
}

size_t NativeRenderer::PipelineKeyHash::operator()(const PipelineKey& k) const {
  uint64_t w[sizeof(PipelineKey) / 8];
  std::memcpy(w, &k, sizeof(w));
  uint64_t h = 0x632BE59BD9B4E019ull;
  for (uint64_t v : w) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  return size_t(h);
}

// -----------------------------------------------------------------------------
// Creation / destruction
// -----------------------------------------------------------------------------

std::unique_ptr<NativeRenderer> NativeRenderer::Create(rex::ui::Presenter* presenter,
                                                       const rex::graphics::CommandProcessor* gpu) {
  auto* vulkan_presenter = dynamic_cast<VulkanPresenter*>(presenter);
  if (!vulkan_presenter) {
    REXLOG_WARN("NativeRenderer: needs the Vulkan presenter; staying on the emulated picture");
    return nullptr;
  }
  std::unique_ptr<NativeRenderer> renderer(new NativeRenderer(vulkan_presenter, gpu));
  if (!renderer->Initialize()) {
    REXLOG_ERROR("NativeRenderer: Vulkan setup failed; staying on the emulated picture");
    return nullptr;  // the destructor cleans up whatever was created
  }
  return renderer;
}

NativeRenderer::NativeRenderer(VulkanPresenter* presenter,
                               const rex::graphics::CommandProcessor* gpu)
    : presenter_(presenter), gpu_(gpu), device_(presenter->vulkan_device()) {}

NativeRenderer::~NativeRenderer() {
  // Order matters: first make sure the game's main thread can't call us
  // any more (this waits for a frame in progress), then give the window back
  // to the emulated GPU, then wait for the GPU before destroying anything.
  native::spotter::Uninstall();
  native::ab_capture::SetNativeShownCheck(nullptr, nullptr);
  pddi::RemoveFrameEndListener(&FrameEndThunk, this);
  native::recorder::Uninstall();
  rex::ui::UnregisterBind("bind_renderer");
  presenter_->SetGuestOutputExternal(false);
  if (tracker_) {
    tracker_->AwaitAllSubmissionsCompletion();
  }
  DestroyVulkanObjects();
  if (tracker_) {
    tracker_->Shutdown();
  }
}

bool NativeRenderer::Initialize() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  tracker_ = std::make_unique<rex::ui::vulkan::VulkanSubmissionTracker>(device_);

  // --- Our frame image: the finished picture (the surface the game resolves
  //     into its frontbuffer is copied here), sampled by the present pass ---
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kFrameFormat;
  image_info.extent = {kWidth, kHeight, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC: room for reading frames back later (captures, A/B diffs).
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, frame_image_,
          frame_memory_)) {
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = frame_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kFrameFormat;
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &frame_view_) != VK_SUCCESS) {
    return false;
  }

  // --- Depth/stencil format for the game's depth surfaces: the first one
  //     the GPU supports ---
  const VulkanDevice* dev = device_;
  const auto& ifn = dev->vulkan_instance()->functions();
  for (VkFormat format : kDepthFormats) {
    VkFormatProperties props;
    ifn.vkGetPhysicalDeviceFormatProperties(dev->physical_device(), format, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      depth_format_ = format;
      break;
    }
  }
  if (depth_format_ == VK_FORMAT_UNDEFINED) {
    REXLOG_ERROR("NativeRenderer: no depth/stencil format supported");
    return false;
  }
  {
    // Mesh and immediate vertex colours are read as B8G8R8A8 (frame.h).
    VkFormatProperties props;
    ifn.vkGetPhysicalDeviceFormatProperties(dev->physical_device(), VK_FORMAT_B8G8R8A8_UNORM,
                                            &props);
    if (!(props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
      REXLOG_ERROR("NativeRenderer: the GPU can't read B8G8R8A8 vertex colours");
      return false;
    }
  }
  {
    // Depth copies are R32_SFLOAT images (render_targets.h). The depth of
    // field asks for them bilinear-filtered, which Vulkan doesn't promise for
    // 32-bit floats: where the GPU can't, they're point-sampled instead.
    VkFormatProperties props;
    ifn.vkGetPhysicalDeviceFormatProperties(dev->physical_device(), VK_FORMAT_R32_SFLOAT,
                                            &props);
    depth_copy_linear_ =
        (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  }
  {
    // The largest mip level bias a sampler may have (the SDK's device
    // properties don't carry it; Vulkan promises at least 2).
    VkPhysicalDeviceProperties props;
    ifn.vkGetPhysicalDeviceProperties(dev->physical_device(), &props);
    max_lod_bias_ = props.limits.maxSamplerLodBias;
  }

  // --- The game's render targets (surfaces) and resolved textures; its
  //     render pass is the one every draw pipeline is built for ---
  render_targets_ = std::make_unique<native::RenderTargets>(device_, kFrameFormat, depth_format_);
  if (!render_targets_->Initialize()) {
    return false;
  }

  // --- The materials: 9 textures, a MaterialBlock, push constants ---
  VkDescriptorSetLayoutBinding draw_bindings[kTexturesPerDraw + 1]{};
  for (uint32_t i = 0; i < kTexturesPerDraw; ++i) {
    draw_bindings[i].binding = i;
    draw_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    draw_bindings[i].descriptorCount = 1;
    draw_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  draw_bindings[kBlockBinding].binding = kBlockBinding;
  draw_bindings[kBlockBinding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  draw_bindings[kBlockBinding].descriptorCount = 1;
  draw_bindings[kBlockBinding].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo draw_set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  draw_set_info.bindingCount = kTexturesPerDraw + 1;
  draw_set_info.pBindings = draw_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &draw_set_info, nullptr, &draw_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_range.size = sizeof(SimpleParams);
  VkPipelineLayoutCreateInfo draw_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  draw_layout_info.setLayoutCount = 1;
  draw_layout_info.pSetLayouts = &draw_set_layout_;
  draw_layout_info.pushConstantRangeCount = 1;
  draw_layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(device, &draw_layout_info, nullptr, &draw_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  if (!CreateWhiteTexture()) {
    return false;
  }
  texture_cache_ = std::make_unique<native::TextureCache>(device_);
  buffer_cache_ = std::make_unique<native::BufferCache>(device_);
  // Immediate vertices and indices; also the staging source of the gamma
  // ramp (TRANSFER_SRC).
  // Pages of 8 MiB: skinned characters are ~130 KB of vertices per draw.
  vertex_pool_ = std::make_unique<rex::ui::vulkan::VulkanUploadBufferPool>(
      device_,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      8u << 20);
  uniform_pool_ = std::make_unique<rex::ui::vulkan::VulkanUploadBufferPool>(
      device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  if (!CreateGammaImage()) {
    return false;
  }

  // --- Present pass: our image -> the presenter's guest-output image ---
  // The presenter wants that image left in SHADER_READ_ONLY_OPTIMAL for its
  // fragment shaders (VulkanPresenter::kGuestOutputInternal*).
  present_pass_ = CreateRenderPass(VulkanPresenter::kGuestOutputFormat,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   VulkanPresenter::kGuestOutputInternalStageMask,
                                   VulkanPresenter::kGuestOutputInternalAccessMask);
  if (!present_pass_) {
    return false;
  }
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 0.0f;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &present_sampler_) != VK_SUCCESS) {
    return false;
  }
  sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &point_sampler_) != VK_SUCCESS) {
    return false;
  }
  // Binding 0: our frame; binding 1: the gamma ramp.
  VkDescriptorSetLayoutBinding present_bindings[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    present_bindings[i].binding = i;
    present_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    present_bindings[i].descriptorCount = 1;
    present_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = present_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &present_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &present_descriptor_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = present_descriptor_pool_;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &present_set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &present_set_) != VK_SUCCESS) {
    return false;
  }
  // Both images never change (only their contents), so the set is written once.
  VkDescriptorImageInfo image_desc[2] = {
      {present_sampler_, frame_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {point_sampler_, gamma_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = present_set_;
  write.dstBinding = 0;
  write.descriptorCount = 2;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = image_desc;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  VkPipelineLayoutCreateInfo present_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  present_layout_info.setLayoutCount = 1;
  present_layout_info.pSetLayouts = &present_set_layout_;
  if (dfn.vkCreatePipelineLayout(device, &present_layout_info, nullptr, &present_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  present_pipeline_ = CreatePresentPipeline();
  if (!present_pipeline_) {
    return false;
  }

  // --- Per frame in flight: command buffer (graphics queue family: the
  //     presenter requires "graphics and compute queue 0" for guest-output
  //     work) and a descriptor pool for the draws' texture bindings ---
  for (FrameSlot& slot : slots_) {
    VkCommandPoolCreateInfo cmd_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cmd_pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &cmd_pool_info, nullptr, &slot.pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = slot.pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device, &cmd_info, &slot.cmd) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize draw_pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexturesPerDraw * kMaxDrawsPerFrame},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDrawsPerFrame},
    };
    VkDescriptorPoolCreateInfo draw_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    draw_pool_info.maxSets = kMaxDrawsPerFrame;
    draw_pool_info.poolSizeCount = 2;
    draw_pool_info.pPoolSizes = draw_pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &draw_pool_info, nullptr, &slot.descriptor_pool) !=
        VK_SUCCESS) {
      return false;
    }
  }

  // --- Hook up: F9, the start setting, the game's calls, its frame end ---
  rex::ui::RegisterBind("bind_renderer", "F9",
                        "Switch the picture between the emulated GPU and the native renderer",
                        [this] {
                          if (emulated_only()) {
                            overlay_banner::Show("EMULATED ONLY",
                                                 "F9 is off (--emulated_only)");
                            return;
                          }
                          // Dual mode: each window already shows one picture.
                          if (window_presenter_.load(std::memory_order_relaxed)) {
                            overlay_banner::Show("DUAL MODE",
                                                 "F9 does nothing while the native window is open "
                                                 "(F8 closes it)");
                            REXLOG_INFO(
                                "NativeRenderer: F9 does nothing while the native window is open "
                                "(the main window shows the emulated picture, the native window "
                                "ours; F8 closes it)");
                            return;
                          }
                          SetShowNative(!show_native());
                        });
  // F10: a photo of the next frame, from both renderers when ours is drawn
  // (main window on native, or the native window open; ab_capture.h: PNGs
  // into --photo_dir). With --debug_pddi_trace_dir (tools/play.sh --trace)
  // it also records every renderer call of that frame (pddi/trace.cpp).
  rex::ui::RegisterBind("bind_photo", "F10",
                        "Save a photo of the game (both renderers' pictures of one frame "
                        "while the native one is shown) into --photo_dir",
                        [this] {
                          native::ab_capture::RequestPhoto(native_drawn());
                          pddi::trace::RequestTrace();
                        });
  const std::string& start = REXCVAR_GET(renderer);
  if (start != "emulated" && start != "native") {
    REXLOG_WARN("NativeRenderer: unknown --renderer=\"{}\" (emulated or native)", start);
  }
  if (REXCVAR_GET(native_only) && emulated_only()) {
    REXLOG_WARN("NativeRenderer: --native_only and --emulated_only together: emulated only wins");
  }
  SetShowNative(!emulated_only() && (start == "native" || REXCVAR_GET(native_only)));
  native::recorder::Install();
  pddi::AddFrameEndListener(&FrameEndThunk, this);
  // Loud log lines when a material we're hunting shows up (spotter.h).
  native::spotter::Install(
      [](void* self) { return static_cast<NativeRenderer*>(self)->native_drawn(); }, this);
  // The debug console's `photo` = F10 (ab_capture::RequestPhotoLikeF10).
  native::ab_capture::SetNativeShownCheck(
      [](void* self) { return static_cast<NativeRenderer*>(self)->native_drawn(); }, this);
  REXLOG_INFO("NativeRenderer: ready ({}x{}), F9 switches emulated/native, F10 takes a photo",
              kWidth, kHeight);
  return true;
}

bool NativeRenderer::CreateWhiteTexture() {
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {1, 1, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, white_image_,
          white_memory_)) {
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = white_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  return device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                                &white_view_) == VK_SUCCESS;
}

bool NativeRenderer::CreateGammaImage() {
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // sampling it is always supported
  image_info.extent = {256, 1, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, image_info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_image_,
          gamma_memory_)) {
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = gamma_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  // The GPU register keeps red in the high bits, where this format has blue.
  view_info.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
                          VK_COMPONENT_SWIZZLE_ONE};
  view_info.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  return device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                                &gamma_view_) == VK_SUCCESS;
}

void NativeRenderer::UpdateGammaRamp(VkCommandBuffer cmd, uint64_t submission) {
  // The game's current ramp, as the emulated GPU received it (it writes the
  // display's gamma registers through D3D; the SDK's command processor keeps
  // the table and applies it at every swap of the emulated picture). Read
  // from the main thread while the GPU thread may write it: a torn read only
  // ever mixes two ramps for one frame. Without an emulated GPU: identity.
  std::array<uint32_t, 256> ramp;
  if (gpu_) {
    std::memcpy(ramp.data(), GammaRampReader::Table(gpu_), sizeof(ramp));
  } else {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = i * 1023 / 255;
      ramp[i] = v | (v << 10) | (v << 20);
    }
  }
  // Brightness (the Options screen; SDK patch 0015): the same power on the
  // ramp the emulated GPU applies at its swap. A GPU-plugin flag: by name.
  static double power = 1.0;
  static uint32_t frames_since_read = 0;
  if (++frames_since_read >= 15) {  // a string parse: 4 times a second is plenty
    frames_since_read = 0;
    const std::string value = rex::cvar::GetFlagByName("gamma_ramp_power");
    power = value.empty() ? 1.0 : std::strtod(value.c_str(), nullptr);
    if (!(power > 0.0)) power = 1.0;
  }
  if (power != 1.0) {
    for (uint32_t& entry : ramp) {
      entry = GammaRampReader::WithPower(entry, power);
    }
  }
  if (gamma_uploaded_ && ramp == gamma_ramp_) {
    return;
  }
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = vertex_pool_->Request(submission, sizeof(ramp), 4, buffer, offset);
  if (!mapping) {
    return;
  }
  std::memcpy(mapping, ramp.data(), sizeof(ramp));
  const VulkanDevice::Functions& dfn = device_->functions();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;  // earlier frames only read it: ordering is enough
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = gamma_uploaded_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = gamma_image_;
  barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {256, 1, 1};
  dfn.vkCmdCopyBufferToImage(cmd, buffer, gamma_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  gamma_ramp_ = ramp;
  gamma_uploaded_ = true;
}

void NativeRenderer::DestroyVulkanObjects() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  using rex::ui::vulkan::util::DestroyAndNullHandle;
  for (FrameSlot& slot : slots_) {
    // Destroying the pools frees their command buffer / descriptor sets too.
    DestroyAndNullHandle(dfn.vkDestroyCommandPool, device, slot.pool);
    DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, slot.descriptor_pool);
    slot.cmd = VK_NULL_HANDLE;
  }
  for (PresentFramebuffer& fb : present_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
  }
  present_framebuffers_.clear();
  DestroyAndNullHandle(dfn.vkDestroyPipeline, device, present_pipeline_);
  DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, present_layout_);
  DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, present_descriptor_pool_);
  present_set_ = VK_NULL_HANDLE;
  DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, present_set_layout_);
  DestroyAndNullHandle(dfn.vkDestroySampler, device, present_sampler_);
  DestroyAndNullHandle(dfn.vkDestroySampler, device, point_sampler_);
  DestroyAndNullHandle(dfn.vkDestroyImageView, device, gamma_view_);
  DestroyAndNullHandle(dfn.vkDestroyImage, device, gamma_image_);
  DestroyAndNullHandle(dfn.vkFreeMemory, device, gamma_memory_);
  DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, present_pass_);

  texture_cache_.reset();
  buffer_cache_.reset();
  render_targets_.reset();
  vertex_pool_.reset();
  uniform_pool_.reset();
  for (auto& [key, pipeline] : pipelines_) {
    dfn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  pipelines_.clear();
  for (auto& [key, sampler] : samplers_) {
    dfn.vkDestroySampler(device, sampler, nullptr);
  }
  samplers_.clear();
  DestroyAndNullHandle(dfn.vkDestroyImageView, device, white_view_);
  DestroyAndNullHandle(dfn.vkDestroyImage, device, white_image_);
  DestroyAndNullHandle(dfn.vkFreeMemory, device, white_memory_);
  DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, draw_layout_);
  DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, draw_set_layout_);

  DestroyAndNullHandle(dfn.vkDestroyImageView, device, frame_view_);
  DestroyAndNullHandle(dfn.vkDestroyImage, device, frame_image_);
  DestroyAndNullHandle(dfn.vkFreeMemory, device, frame_memory_);
}

// -----------------------------------------------------------------------------
// Building blocks
// -----------------------------------------------------------------------------

// A single-subpass render pass with one colour attachment that ends in
// SHADER_READ_ONLY_OPTIMAL, for whoever samples it next (`next_stages` /
// `next_access`: our present pass, or the presenter).
//
// It always starts from layout UNDEFINED: every pass overwrites the whole
// image, so the old contents may be discarded. The incoming dependency makes
// the write wait for the previous frame's readers (a fragment shader sampled
// this image, in an earlier submission on the same queue).
VkRenderPass NativeRenderer::CreateRenderPass(VkFormat format, VkAttachmentLoadOp load_op,
                                              VkPipelineStageFlags next_stages,
                                              VkAccessFlags next_access) {
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = load_op;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  VkSubpassDependency dependencies[2]{};
  // In: earlier readers (fragment shaders) before our colour writes.
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | next_stages;
  dependencies[0].srcAccessMask = 0;  // write-after-read: ordering is enough
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  // Out: our colour writes visible to the next reader.
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = next_stages;
  dependencies[1].dstAccessMask = next_access;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &attachment;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 2;
  info.pDependencies = dependencies;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (device_->functions().vkCreateRenderPass(device_->device(), &info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return render_pass;
}

namespace {

// Everything both kinds of pipelines share: shader stages, viewport + scissor
// as dynamic state (any size), one colour attachment. `depth_stencil`: null
// for a render pass without depth (the present pass).
VkPipeline BuildPipeline(const rex::ui::vulkan::VulkanDevice* device_, VkPipelineLayout layout,
                         VkRenderPass render_pass, const uint32_t* vs, size_t vs_size,
                         const uint32_t* fs, size_t fs_size,
                         const VkPipelineVertexInputStateCreateInfo& vertex_input,
                         VkPrimitiveTopology topology,
                         const VkPipelineColorBlendAttachmentState& blend_attachment,
                         VkCullModeFlags cull_mode = VK_CULL_MODE_NONE,
                         VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                         const VkPipelineDepthStencilStateCreateInfo* depth_stencil = nullptr,
                         VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  VkShaderModule vs_module = rex::ui::vulkan::util::CreateShaderModule(device_, vs, vs_size);
  VkShaderModule fs_module = rex::ui::vulkan::util::CreateShaderModule(device_, fs, fs_size);
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vs_module && fs_module) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_module;
    stages[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = cull_mode;
    raster.frontFace = front_face;  // see ToFacing
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    // MSAA: the samples of the render pass's attachments. Plain
    // multisampling, like the Xbox's: coverage and depth per sample, the
    // pixel shader once per pixel (no sample shading).
    multisample.rasterizationSamples = samples;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    // Viewport + scissor (any size); with a depth/stencil attachment also the
    // stencil reference and masks (set per draw, no pipeline per value).
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = depth_stencil ? 5 : 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = render_pass;
    if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
  }
  // Modules are only needed while creating the pipeline.
  if (vs_module) {
    dfn.vkDestroyShaderModule(device, vs_module, nullptr);
  }
  if (fs_module) {
    dfn.vkDestroyShaderModule(device, fs_module, nullptr);
  }
  return pipeline;
}

constexpr VkColorComponentFlags kWriteAll = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

}  // namespace

VkPipeline NativeRenderer::CreatePresentPipeline() {
  VkPipelineVertexInputStateCreateInfo no_vertices{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState no_blend{};
  no_blend.colorWriteMask = kWriteAll;
  return BuildPipeline(device_, present_layout_, present_pass_, kPresentVs, sizeof(kPresentVs),
                       kPresentFs, sizeof(kPresentFs), no_vertices,
                       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, no_blend);
}

VkPipeline NativeRenderer::GetPipeline(const native::DrawCommand& draw, bool has_depth,
                                       VkSampleCountFlagBits samples) {
  const PipelineKey key = MakePipelineKey(draw, has_depth, samples);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }
  // Vertex layout (frame.h): the game's own, for meshes and immediate
  // geometry alike. A component the vertex lacks still gets an attribute
  // (Vulkan wants every shader input fed) reading offset 0; the shader is
  // told to ignore it (kFlagNoColour / kFlagNoUv, kLitNo*). Each material
  // gets only the attributes its vertex shader reads:
  //   location 0 position, 1 colour, 2 uv, 3 normal, 4 binormal, 5 tangent.
  auto offset_or_zero = [](uint8_t offset) {
    return offset == native::VertexLayout::kAbsent ? 0u : uint32_t(offset);
  };
  VkVertexInputBindingDescription vertex_binding{0, key.stride, VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription all_attributes[6] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
      // D3DCOLOR after the 32-bit byte swap: bytes B, G, R, A.
      {1, 0, VK_FORMAT_B8G8R8A8_UNORM, offset_or_zero(key.colour_offset)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offset_or_zero(key.uv_offset)},
      {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offset_or_zero(key.normal_offset)},
      {4, 0, VK_FORMAT_R32G32B32_SFLOAT, offset_or_zero(key.binormal_offset)},
      {5, 0, VK_FORMAT_R32G32B32_SFLOAT, offset_or_zero(key.tangent_offset)},
  };
  VkVertexInputAttributeDescription attributes[6];
  uint32_t attribute_count = 0;
  auto use = [&](std::initializer_list<uint32_t> locations) {
    for (uint32_t location : locations) {
      attributes[attribute_count++] = all_attributes[location];
    }
  };
  const uint32_t* vs = kSimpleVs;
  const uint32_t* fs = kSimpleFs;
  size_t vs_size = sizeof(kSimpleVs), fs_size = sizeof(kSimpleFs);
  switch (draw.material) {
    case native::Material::kSimple:
      use({0, 1, 2});
      break;
    case native::Material::kBink:
      use({0, 1, 2});
      vs = kBinkVs, vs_size = sizeof(kBinkVs), fs = kBinkFs, fs_size = sizeof(kBinkFs);
      break;
    case native::Material::kLitSimple:
      use({0, 2, 3});
      vs = kLitVs, vs_size = sizeof(kLitVs), fs = kLitFs, fs_size = sizeof(kLitFs);
      break;
    case native::Material::kCharacter:
      use({0, 2, 3, 4, 5});
      vs = kCharacterVs, vs_size = sizeof(kCharacterVs);
      fs = kCharacterFs, fs_size = sizeof(kCharacterFs);
      break;
    case native::Material::kShadow:
      use({0, 1});
      vs = kShadowVs, vs_size = sizeof(kShadowVs), fs = kShadowFs, fs_size = sizeof(kShadowFs);
      break;
    case native::Material::kReflect:
      use({0, 1, 2, 3});
      vs = kReflectVs, vs_size = sizeof(kReflectVs);
      fs = kReflectFs, fs_size = sizeof(kReflectFs);
      break;
    case native::Material::kParticle:
      use({0, 1, 2});
      vs = kParticleVs, vs_size = sizeof(kParticleVs);
      fs = kParticleFs, fs_size = sizeof(kParticleFs);
      break;
    case native::Material::kWater:
      use({0, 1, 2});
      vs = kWaterVs, vs_size = sizeof(kWaterVs), fs = kWaterFs, fs_size = sizeof(kWaterFs);
      break;
    case native::Material::kDof:
      use({0, 2});
      vs = kDofVs, vs_size = sizeof(kDofVs), fs = kDofFs, fs_size = sizeof(kDofFs);
      break;
    case native::Material::kFxGrid:
      // The simple vertex shader (the game's fxgridvp does the same).
      use({0, 1, 2});
      fs = kFxGridFs, fs_size = sizeof(kFxGridFs);
      break;
    case native::Material::kBumpMega:
      use({0, 1, 2, 3, 4, 5});
      vs = kBumpMegaVs, vs_size = sizeof(kBumpMegaVs);
      fs = kBumpMegaFs, fs_size = sizeof(kBumpMegaFs);
      break;
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &vertex_binding;
  vertex_input.vertexAttributeDescriptionCount = attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;
  // Blending as the game's material set it (D3D9-style: the same factors for
  // colour and alpha), and the colour write mask (Vulkan's R/G/B/A bits are
  // the same as ours).
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = key.colour_mask;
  if (key.blend_enable) {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = ToBlendFactor(key.blend_src);
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = ToBlendFactor(key.blend_dst);
    blend.colorBlendOp = blend.alphaBlendOp = ToBlendOp(key.blend_op);
  }
  // Depth: the game's test / write / compare function (already reversed for
  // its flipped viewport, guest.h). Stencil: not yet.
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = key.depth_test;
  depth.depthWriteEnable = key.depth_write;
  depth.depthCompareOp = VkCompareOp(key.depth_func);
  // Stencil: compare and ops baked in (Xbox and Vulkan number them the same);
  // reference and masks are dynamic (BuildPipeline), set per draw.
  depth.stencilTestEnable = key.stencil_enable;
  auto stencil_face = [](const uint8_t s[4]) {
    VkStencilOpState face{};
    face.compareOp = VkCompareOp(s[0]);
    face.failOp = VkStencilOp(s[1]);
    face.depthFailOp = VkStencilOp(s[2]);
    face.passOp = VkStencilOp(s[3]);
    face.compareMask = face.writeMask = 0xFF;
    return face;
  };
  depth.front = stencil_face(key.stencil_front);
  depth.back = stencil_face(key.stencil_back);
  VkPipeline pipeline =
      BuildPipeline(device_, draw_layout_, render_targets_->render_pass(samples), vs, vs_size,
                    fs, fs_size, vertex_input, ToTopology(draw.topology), blend,
                    ToFacing(key.cull).cull, ToFacing(key.cull).front, &depth, samples);
  if (!pipeline) {
    REXLOG_ERROR("NativeRenderer: can't create a pipeline (material {}, stride {})", key.material,
                 key.stride);
  }
  pipelines_[key] = pipeline;  // a failure is cached too (not retried)
  return pipeline;
}

VkSampler NativeRenderer::GetSampler(const native::SamplerState& state) {
  // Everything the sampler depends on, packed into one key.
  const uint64_t key = uint64_t(state.address_u & 7) | uint64_t(state.address_v & 7) << 3 |
                       uint64_t(state.linear) << 6 | uint64_t(state.min_linear) << 7 |
                       uint64_t(state.mip & 3) << 8 | uint64_t(state.anisotropy & 31) << 10 |
                       uint64_t(state.min_level & 15) << 15 |
                       uint64_t(uint16_t(state.lod_bias)) << 19;
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  // As the emulator builds its samplers (the SDK's VulkanTextureCache::
  // UseSampler), from the game's fetch constant (frame.h, SamplerState).
  const VulkanDevice::Properties& props = device_->properties();
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = state.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = state.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = state.mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = ToAddressMode(state.address_u);
  info.addressModeV = ToAddressMode(state.address_v);
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  // Mip levels (docs/findings/17): from the most detailed one allowed on,
  // as far as the image has them; "base map" = that one level only.
  info.minLod = float(state.min_level);
  info.maxLod = state.mip == 0 ? info.minLod + 0.25f : VK_LOD_CLAMP_NONE;
  info.mipLodBias = std::clamp(float(state.lod_bias) / 32.0f, -max_lod_bias_, max_lod_bias_);
  // Anisotropic filtering: the 3D world asks for 16 samples along slanted
  // surfaces (keeps the ground sharp at shallow angles), where the GPU can.
  if (state.anisotropy >= 1 && props.samplerAnisotropy) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = std::min(float(state.anisotropy), props.maxSamplerAnisotropy);
  }
  VkSampler sampler = VK_NULL_HANDLE;
  device_->functions().vkCreateSampler(device_->device(), &info, nullptr, &sampler);
  samplers_[key] = sampler;
  return sampler;
}

VkFramebuffer NativeRenderer::GetPresentFramebuffer(uint64_t image_version,
                                                    VkImageView image_view) {
  for (const PresentFramebuffer& fb : present_framebuffers_) {
    if (fb.image_version == image_version && fb.view == image_view) {
      return fb.framebuffer;
    }
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  // The presenter keeps at most this many guest-output images alive; if we
  // have more, the oldest one's image is gone: drop its framebuffer (after
  // the GPU is done with our last use of it).
  if (present_framebuffers_.size() >= VulkanPresenter::kMaxActiveGuestOutputImageVersions) {
    auto oldest = std::min_element(
        present_framebuffers_.begin(), present_framebuffers_.end(),
        [](const auto& a, const auto& b) { return a.image_version < b.image_version; });
    {
      CostTimer wait(cost_.gpu_wait_ms);
      tracker_->AwaitSubmissionCompletion(oldest->last_submission);
    }
    dfn.vkDestroyFramebuffer(device_->device(), oldest->framebuffer, nullptr);
    present_framebuffers_.erase(oldest);
  }
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = present_pass_;
  info.attachmentCount = 1;
  info.pAttachments = &image_view;
  info.width = kWidth;
  info.height = kHeight;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (dfn.vkCreateFramebuffer(device_->device(), &info, nullptr, &framebuffer) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  present_framebuffers_.push_back({image_version, image_view, framebuffer, 0});
  return framebuffer;
}

void NativeRenderer::ForgetPresentFramebuffers() {
  if (present_framebuffers_.empty()) {
    return;
  }
  // Rare (a window opens or closes, or F9): simply wait for all our work.
  tracker_->AwaitAllSubmissionsCompletion();
  for (PresentFramebuffer& fb : present_framebuffers_) {
    device_->functions().vkDestroyFramebuffer(device_->device(), fb.framebuffer, nullptr);
  }
  present_framebuffers_.clear();
}

// -----------------------------------------------------------------------------
// Per frame
// -----------------------------------------------------------------------------

void NativeRenderer::SetShowNative(bool show) {
  show_native_.store(show, std::memory_order_relaxed);
  // Tell the emulated GPU whether it may still refresh the main window
  // (SDK patch 0004). When switching back it simply takes over at its next
  // swap. In dual mode the main window stays emulated whatever F9 says.
  presenter_->SetGuestOutputExternal(show && !window_presenter_.load(std::memory_order_relaxed));
  REXLOG_INFO("NativeRenderer: showing the {} picture (F9 switches)",
              show ? "native" : "emulated");
  UpdateEmulatedDrawing();
  AnnouncePicture();
}

bool NativeRenderer::emulated_only() const { return REXCVAR_GET(emulated_only); }

void NativeRenderer::AnnouncePicture() {
  // The on-screen message (overlay_banner.h): which picture the MAIN window
  // shows now, and what the other renderer is doing meanwhile.
  if (window_presenter_.load(std::memory_order_relaxed)) {
    overlay_banner::Show("DUAL MODE", "this window: emulated (Xbox 360 GPU)   second window: "
                                      "native (Vulkan)   F8 closes it");
  } else if (emulated_only()) {
    overlay_banner::Show("EMULATED ONLY", "Xbox 360 GPU emulation; native renderer off");
  } else if (show_native()) {
    overlay_banner::Show("NATIVE (Vulkan)",
                         REXCVAR_GET(native_only)
                             ? "native only: the emulated GPU is off   F9 switches"
                             : "the emulated GPU still draws in the background   F9 switches");
  } else {
    overlay_banner::Show("EMULATED (Xbox 360 GPU)", "native renderer idle   F9 switches");
  }
}

void NativeRenderer::UpdateEmulatedDrawing() {
  if (!REXCVAR_GET(native_only)) {
    // Native only can be switched OFF while the game runs (the Options
    // screen, options/options_settings.cpp): the emulated GPU draws again.
    if (rex::cvar::GetFlagByName("skip_draws") == "true") {
      rex::cvar::SetFlagByName("skip_draws", "false");
      REXLOG_INFO("NativeRenderer: emulated GPU drawing ON (native only switched off)");
    }
    return;
  }
  // Off exactly when the emulated picture is on no screen: main window on
  // native and no native window (dual mode shows the emulated one).
  const bool skip = show_native() && !window_presenter_.load(std::memory_order_relaxed);
  // A GPU-plugin flag (loaded at runtime, not linkable): set by name, like
  // ab_capture's readback_resolve. Hot-reloadable: read at every draw.
  if (!rex::cvar::SetFlagByName("skip_draws", skip ? "true" : "false")) {
    REXLOG_WARN("NativeRenderer: --native_only needs SDK patch 0009 (GPU flag skip_draws)");
    return;
  }
  REXLOG_INFO("NativeRenderer: emulated GPU drawing {} (--native_only)",
              skip ? "OFF: only our renderer draws" : "ON");
}

void NativeRenderer::UpdateEmulatedFrameRate(bool native_drawn) {
  // Every frame unless our renderer draws too (see emulated_draw_every), and
  // always while a photo / A/B capture is under way.
  int32_t every = native_drawn ? REXCVAR_GET(emulated_draw_every) : 1;
  if (native::ab_capture::CaptureUnderway()) {
    every = 1;
  }
  // A GPU-plugin flag (SDK patch 0013), set by name like skip_draws; only
  // when it changes (setting a flag isn't free, this runs every frame).
  if (rex::cvar::Query<int32_t>("draw_every_nth_frame") == every) {
    return;
  }
  if (!rex::cvar::SetFlagByName("draw_every_nth_frame", std::to_string(every))) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("NativeRenderer: --emulated_draw_every needs SDK patch 0013 (GPU flag "
                  "draw_every_nth_frame)");
    }
    return;
  }
  REXLOG_INFO("NativeRenderer: the emulated GPU draws 1 frame in {} (--emulated_draw_every)",
              every);
}

void NativeRenderer::SetWindowPresenter(rex::ui::Presenter* presenter) {
  // The native window's presenter comes from the same Vulkan provider as the
  // main one (same device), so it's a VulkanPresenter too.
  auto* vulkan_presenter = dynamic_cast<VulkanPresenter*>(presenter);
  if (presenter && !vulkan_presenter) {
    REXLOG_WARN("NativeRenderer: the native window's presenter isn't a Vulkan one");
    return;
  }
  window_presenter_.store(vulkan_presenter, std::memory_order_relaxed);
  // Open: the main window goes (back) to the emulated picture. Closed: the
  // main window shows what F9 had chosen.
  presenter_->SetGuestOutputExternal(!vulkan_presenter && show_native());
  if (vulkan_presenter) {
    REXLOG_INFO("NativeRenderer: dual mode: our picture in the native window, the emulated "
                "one in the main window");
  } else {
    REXLOG_INFO("NativeRenderer: native window closed: the main window shows the {} picture",
                show_native() ? "native" : "emulated");
  }
  UpdateEmulatedDrawing();
  AnnouncePicture();
}

void NativeRenderer::OnFrameEnd() {
  // Where our picture goes this frame (native_renderer.h, DUAL MODE): the
  // native window, else the main window if F9 chose ours, else nowhere.
  VulkanPresenter* target = window_presenter_.load(std::memory_order_relaxed);
  if (!target && show_native()) {
    target = presenter_;
  }
  // How often the emulated GPU draws from the next frame on.
  UpdateEmulatedFrameRate(target != nullptr);
  // Take what the game just drew; record the next frame only while our
  // picture is on screen somewhere.
  native::recorder::TakeFrame(frame_, target != nullptr);
  if (!target) {
    // F10 photos of the emulated picture (ab_capture.h).
    native::ab_capture::WhileEmulatedShown(presenter_);
    return;
  }
  const CostClock::time_point start = CostClock::now();
  if (target != last_target_) {
    ForgetPresentFramebuffers();  // they were the other presenter's
    last_target_ = target;
  }
  // The presenter picks one of its guest-output images and calls us back
  // right away, on this thread; we must submit our work before returning.
  target->RefreshGuestOutput(
      kWidth, kHeight, 16, 9, [this](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        return RecordAndSubmit(
            static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context));
      });
  LogCost(MsSince(start));
  // F10 photos and the debug --debug_native_ab_ms: this frame from both
  // renderers (ours captured from whichever presenter got it).
  native::ab_capture::AfterPresent(target, *texture_cache_, gamma_ramp_, frame_, &MaterialName);
}

void NativeRenderer::LogCost(double frame_ms) {
  cost_.total_ms += frame_ms;
  cost_.worst_ms = std::max(cost_.worst_ms, frame_ms);
  if (++cost_.frames < kCostFrames) {
    return;
  }
  const double n = cost_.frames;
  const double rest =
      cost_.total_ms - cost_.gpu_wait_ms - cost_.textures_ms - cost_.buffers_ms;
  REXLOG_INFO("NativeRenderer: last {} frames, on the game's main thread: {:.2f} ms per frame "
              "(waiting for the GPU {:.2f}, textures {:.2f}, mesh buffers {:.2f}, the rest "
              "{:.2f}), worst frame {:.1f} ms",
              cost_.frames, cost_.total_ms / n, cost_.gpu_wait_ms / n, cost_.textures_ms / n,
              cost_.buffers_ms / n, rest / n, cost_.worst_ms);
  // How the texture checks went (texture_cache.h, "when it re-uploads"):
  // "unchanged" ones cost no hash thanks to the write watch.
  const native::TextureCache::CheckStats checks = texture_cache_->TakeCheckStats();
  REXLOG_INFO("NativeRenderer: texture checks per frame: {:.1f} unchanged (write watch), {:.1f} "
              "hashed, {:.2f} uploaded; {} missed by the watch",
              checks.unchanged / n, checks.hashed / n, checks.uploaded / n, checks.missed);
  cost_ = Cost{};
}

void NativeRenderer::RecordScene(VkCommandBuffer cmd, uint64_t submission,
                                 VkDescriptorPool descriptor_pool) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();

  // 1. Immediate-mode geometry: the whole frame's vertices and indices, each
  //    into one upload buffer (draws bind them at their own offsets). And
  //    the block materials' MaterialBlocks, plus one zeroed block at the
  //    end: every draw's descriptor set must fill binding 9, the other draws
  //    point it there.
  VkBuffer vertex_buffer = VK_NULL_HANDLE, index_buffer = VK_NULL_HANDLE;
  VkDeviceSize vertex_base = 0, index_base = 0;
  auto upload = [&](const void* data, size_t bytes, VkBuffer& buffer, VkDeviceSize& offset) {
    if (!bytes) {
      return;
    }
    if (uint8_t* mapping = vertex_pool_->Request(submission, bytes, 16, buffer, offset)) {
      std::memcpy(mapping, data, bytes);
    } else {
      REXLOG_WARN("NativeRenderer: frame {} has too much immediate geometry ({} bytes)",
                  frame_.number, bytes);
      buffer = VK_NULL_HANDLE;
    }
  };
  upload(frame_.vertex_data.data(), frame_.vertex_data.size() * 4, vertex_buffer, vertex_base);
  upload(frame_.indices.data(), frame_.indices.size() * 2, index_buffer, index_base);
  VkBuffer block_buffer = VK_NULL_HANDLE;
  VkDeviceSize block_base = 0;
  native::MaterialBlock* blocks = nullptr;
  const size_t block_count = frame_.blocks.size();
  if (uint8_t* mapping =
          uniform_pool_->Request(submission, (block_count + 1) * sizeof(native::MaterialBlock),
                                 kUniformAlignment, block_buffer, block_base)) {
    blocks = reinterpret_cast<native::MaterialBlock*>(mapping);
    std::memcpy(blocks, frame_.blocks.data(), block_count * sizeof(native::MaterialBlock));
    std::memset(&blocks[block_count], 0, sizeof(native::MaterialBlock));
  } else {
    // No draw can be set up without it (every set fills binding 9): the
    // frame keeps only its clears and resolves.
    REXLOG_WARN("NativeRenderer: frame {} has too many material-block draws ({})", frame_.number,
                block_count);
  }

  // 2. Images for the game's surfaces and resolve destinations (new ones
  //    are cleared), before anything samples them.
  render_targets_->Prepare(frame_, cmd, submission);
  // A texture: our image if the game resolves into it, else from guest memory.
  // Debug aid (--debug_native_emulated_resolves): sample the emulated GPU's
  // resolves (read back into guest memory) instead of our own.
  const bool use_emulated_resolves = REXCVAR_GET(debug_native_emulated_resolves);
  // Those resolves are written by the emulated GPU, past the write watch.
  texture_cache_->UseWriteWatch(!use_emulated_resolves && REXCVAR_GET(debug_native_write_watch));
  auto get_texture = [&](const native::GuestTexture& texture) {
    const native::RenderTargets::TextureView resolved =
        use_emulated_resolves ? native::RenderTargets::TextureView{}
                              : render_targets_->FindTexture(texture);
    if (resolved.view) {
      return native::TextureCache::View{resolved.view, resolved.width, resolved.height};
    }
    CostTimer timer(cost_.textures_ms);
    return texture_cache_->Get(texture, cmd, submission);
  };

  // 3. Per draw, outside any render pass: mesh buffers and textures (may
  //    record uploads), descriptor set, pipeline. A draw we can't do fully is
  //    skipped: a missing texture drawn as a plain quad could cover the
  //    whole screen.
  prepared_.assign(frame_.draws.size(), PreparedDraw{});
  uint32_t drawn = 0, meshes = 0, block_drawn = 0;
  for (size_t i = 0; i < frame_.draws.size(); ++i) {
    const native::DrawCommand& d = frame_.draws[i];
    PreparedDraw& p = prepared_[i];
    if (!d.targets.colour || !blocks) {
      continue;  // no render target 0: nothing D3D would draw either
    }
    p.has_depth = d.targets.depth != 0;
    if (d.mesh) {
      CostTimer timer(cost_.buffers_ms);
      p.vertex_buffer = buffer_cache_->Get(d.vertex_buffer, cmd, submission);
      if (d.index_buffer.present()) {
        p.index_buffer = buffer_cache_->Get(d.index_buffer, cmd, submission);
        if (!p.index_buffer) {
          continue;
        }
      }
      if (!p.vertex_buffer) {
        continue;
      }
    } else if (!d.vertex_count || !vertex_buffer || (d.index_count && !index_buffer)) {
      continue;
    }
    // The textures (bindings: see kTexturesPerDraw) and how each is sampled.
    // A palettized texture's indices are point-sampled (the shader filters
    // the looked-up colours itself); palettes are point-sampled and clamped.
    VkImageView views[kTexturesPerDraw];
    VkSampler samplers[kTexturesPerDraw];
    for (uint32_t t = 0; t < kTexturesPerDraw; ++t) {
      views[t] = white_view_;
      samplers[t] = GetSampler(native::SamplerState{});
    }
    // Binds `texture` at `binding`; false if it can't be had (skip the draw).
    auto bind = [&](uint32_t binding, const native::GuestTexture& texture,
                    const native::SamplerState& state, float* size = nullptr) {
      const native::TextureCache::View view = get_texture(texture);
      if (!view.view) {
        // Fetch constant word 1: bits 0-5 format, 12-31 address (4 KB units).
        const uint32_t format = texture.fetch[1] & 0x3F;
        if (reported_textures_.insert(uint32_t(d.material) << 16 | binding << 8 | format)
                .second) {
          REXLOG_INFO("NativeRenderer: a {} draw's texture {} isn't available (format {}, "
                      "address {:08X})",
                      MaterialName(d.material), binding, format, texture.fetch[1] & 0xFFFFF000u);
        }
        return false;
      }
      views[binding] = view.view;
      // A depth copy (the game's k_24_8 / k_24_8_FLOAT formats, 22 / 23) is
      // filtered only if the GPU can (depth_copy_linear_).
      const uint32_t format = texture.fetch[1] & 0x3F;
      const bool depth_copy = format == 22 || format == 23;
      samplers[binding] =
          GetSampler(depth_copy && !depth_copy_linear_ ? state.Unfiltered() : state);
      if (size) {
        size[0] = float(view.width);
        size[1] = float(view.height);
      }
      return true;
    };
    // Palettes are looked up one entry at a time; the movie's colour planes
    // are bilinear. Both clamped, base level only.
    const native::SamplerState point_clamp = native::SamplerState{2, 2, false}.Unfiltered();
    const native::SamplerState linear_clamp{2, 2, true};
    const bool palettized = d.textured && d.palette.present();
    // A palettized texture's indices are point-sampled at full size (the
    // shader filters the looked-up colours itself, palette.glsl).
    const native::SamplerState texture_state =
        palettized ? d.texture_sampler.Unfiltered() : d.texture_sampler;
    native::MaterialBlock* block =
        d.block != native::DrawCommand::kNoBlock ? &blocks[d.block] : nullptr;
    native::LitParams* lit = block ? &block->lit : nullptr;
    float texture_size[2] = {1, 1};
    bool ok = true;
    switch (d.material) {
      case native::Material::kBink:
        // The movie's three planes (texture = Y, chroma = Cr, Cb): Y as the
        // game's sampler 0 says, the colour planes bilinear and clamped.
        ok = bind(0, d.texture, texture_state) && bind(1, d.chroma[0], linear_clamp) &&
             bind(2, d.chroma[1], linear_clamp);
        break;
      case native::Material::kSimple:
        if (d.textured) {
          ok = bind(0, d.texture, texture_state, texture_size) &&
               (!palettized || bind(1, d.palette, point_clamp));
        }
        break;
      case native::Material::kLitSimple:
        if (d.textured) {
          ok = bind(0, d.texture, texture_state, lit ? lit->texture_sizes : nullptr) &&
               (!palettized || bind(3, d.palette, point_clamp));
        }
        break;
      case native::Material::kCharacter: {
        const bool normal_palettized = d.normal_palette.present();
        const native::SamplerState normal_state =
            normal_palettized ? d.normal_sampler.Unfiltered() : d.normal_sampler;
        ok = bind(0, d.texture, texture_state, lit ? lit->texture_sizes : nullptr) &&
             (!palettized || bind(3, d.palette, point_clamp)) &&
             (!d.normal_map.present() ||
              bind(1, d.normal_map, normal_state, lit ? lit->texture_sizes + 2 : nullptr)) &&
             (!normal_palettized || bind(4, d.normal_palette, point_clamp)) &&
             (!d.blend_map.present() || bind(2, d.blend_map, d.blend_sampler));
        break;
      }
      case native::Material::kReflect:
        ok = (!d.textured ||
              (bind(0, d.texture, texture_state, lit ? lit->texture_sizes : nullptr) &&
               (!palettized || bind(3, d.palette, point_clamp)))) &&
             bind(1, d.reflection, d.reflection_sampler);
        break;
      case native::Material::kParticle: {
        native::ParticleParams* particle = block ? &block->particle : nullptr;
        ok = !d.textured ||
             (bind(0, d.texture, texture_state,
                   particle ? particle->fog_range_texture_size + 2 : nullptr) &&
              (!palettized || bind(8, d.palette, point_clamp)));
        // The scene's depth (a depth resolve's copy). Without it the sprite
        // is still drawn, just with hard edges.
        if (ok && particle && d.depth_copy.present() &&
            !bind(1, d.depth_copy, d.depth_sampler)) {
          particle->flags[0] &= ~native::kParticleSoft;
        }
        break;
      }
      case native::Material::kWater: {
        // Each sampler as the game bound it. The scene copy and its depth
        // are required; any other texture we can't have stays white (the
        // reflection is switched off without its picture or mask).
        for (uint32_t s = 0; s < native::guest::shader::kWaterSamplers && ok; ++s) {
          const native::GuestTexture& texture = d.device_textures[s];
          if (!texture.present() || bind(s, texture, d.device_samplers[s])) {
            continue;
          }
          if (s == native::guest::shader::kWaterBackgroundSampler ||
              s == native::guest::shader::kWaterDepthSampler) {
            ok = false;
          } else if (block && (s == native::guest::shader::kWaterReflectMapSampler ||
                               s == native::guest::shader::kWaterReflectMaskSampler)) {
            block->water.flags[0] &= ~native::kWaterReflection;
          }
        }
        break;
      }
      case native::Material::kDof:
        // The picture so far and the scene's depth: both required (without
        // them the pass is skipped and the picture stays sharp).
        ok = bind(0, d.texture, texture_state) && bind(1, d.depth_copy, d.depth_sampler);
        break;
      case native::Material::kFxGrid:
        // The picture so far: required (without it the glow is skipped).
        ok = bind(0, d.texture, texture_state);
        break;
      case native::Material::kBumpMega: {
        // Each sampler as the game bound it (guest.h). A palettized texture's
        // indices are point-sampled (the shader filters the looked-up colours
        // itself, the game's way), its palette looked up entry by entry. The
        // diffuse texture is required; any other we can't have switches its
        // effect off (as ReadBumpMegaParams does when one isn't bound).
        namespace sh = native::guest::shader;
        uint32_t* flags = block ? &block->bump_mega.flags[0] : nullptr;
        auto has = [&](uint32_t flag) { return flags && (*flags & flag); };
        for (uint32_t s = 0; s < d.device_textures.size() && ok; ++s) {
          const native::GuestTexture& texture = d.device_textures[s];
          if (!texture.present()) {
            continue;
          }
          native::SamplerState state = d.device_samplers[s];
          if ((s == sh::kBumpMegaDiffuseSampler && has(native::kBumpMegaPalette)) ||
              (s == sh::kBumpMegaNormalSampler && has(native::kBumpMegaPalette2))) {
            state = state.Unfiltered();
          } else if (s == sh::kBumpMegaPaletteSampler || s == sh::kBumpMegaPalette2Sampler) {
            state = point_clamp;
          }
          if (bind(s, texture, state)) {
            continue;
          }
          if (s == sh::kBumpMegaDiffuseSampler) {
            ok = false;
          } else if (flags) {
            *flags &= ~(s == sh::kBumpMegaNormalSampler      ? 0u
                        : s == sh::kBumpMegaBackgroundSampler ? native::kBumpMegaRefract
                        : s == sh::kBumpMegaPaletteSampler    ? native::kBumpMegaPalette
                        : s == sh::kBumpMegaPalette2Sampler   ? native::kBumpMegaPalette2
                                                              : native::kBumpMegaReflect);
            if (s == sh::kBumpMegaNormalSampler) {
              *flags |= native::kBumpMegaNoNormalMap;
            }
          }
        }
        break;
      }
      case native::Material::kShadow: {
        // The stencil copy a depth resolve made earlier in this frame (or a
        // previous one: the image keeps its contents).
        const native::RenderTargets::TextureView copy =
            render_targets_->FindStencilCopy(d.stencil_copy);
        ok = copy.view != VK_NULL_HANDLE;
        if (ok) {
          views[0] = copy.view;
          samplers[0] = GetSampler(d.stencil_sampler);
        }
        break;
      }
    }
    if (!ok || (UsesBlock(d.material) && !block)) {
      continue;
    }
    p.texture_width = texture_size[0];
    p.texture_height = texture_size[1];
    VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info.descriptorPool = descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &draw_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &p.set) != VK_SUCCESS) {
      continue;  // more than kMaxDrawsPerFrame
    }
    VkDescriptorImageInfo images[kTexturesPerDraw];
    for (uint32_t t = 0; t < kTexturesPerDraw; ++t) {
      images[t] = {samplers[t], views[t], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    // The draw's MaterialBlock, or the zeroed one after the last.
    const VkDescriptorBufferInfo block_info{
        block_buffer,
        block_base + (block ? d.block : block_count) * sizeof(native::MaterialBlock),
        sizeof(native::MaterialBlock)};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = p.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = kTexturesPerDraw;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = images;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = p.set;
    writes[1].dstBinding = kBlockBinding;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &block_info;
    dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    p.pipeline =
        GetPipeline(d, p.has_depth, render_targets_->SurfaceSamples(d.targets.colour));
    drawn += p.pipeline ? 1 : 0;
    meshes += p.pipeline && d.mesh ? 1 : 0;
    block_drawn += p.pipeline && block ? 1 : 0;
  }
  // The mesh uploads recorded above, visible to the vertex stage.
  buffer_cache_->FinishUploads(cmd);

  // 4. The game's commands in order: draws and clears go into a render pass
  //    on their surfaces (RenderTargets opens a new one when the surfaces
  //    change), resolves become image copies between passes.
  //    State tracking below only avoids re-binding what's already bound; a
  //    new render pass resets it (the scissor is per pass).
  bool in_pass = false;
  native::Targets pass_targets;
  VkViewport bound_viewport{};
  bool viewport_set = false;
  VkPipeline bound = VK_NULL_HANDLE;
  VkBuffer bound_vertices = VK_NULL_HANDLE, bound_indices = VK_NULL_HANDLE;
  VkDeviceSize bound_vertex_offset = 0;
  // Opens (or keeps) the pass for `targets`; false = skip the command.
  auto enter_pass = [&](const native::Targets& targets) {
    if (in_pass && targets == pass_targets) {
      return true;
    }
    bool has_depth = false;
    if (!render_targets_->BeginPass(targets, cmd, has_depth)) {
      in_pass = false;
      return false;
    }
    in_pass = true;
    pass_targets = targets;
    const VkExtent2D extent = render_targets_->pass_extent();
    const VkRect2D scissor{{0, 0}, extent};
    dfn.vkCmdSetScissor(cmd, 0, 1, &scissor);
    viewport_set = false;
    return true;
  };
  // The game's viewport (usually the whole surface). Vulkan wants a positive
  // size and depths within 0..1 (min > max is fine: the game's flipped
  // depth range).
  auto to_vulkan = [&](const native::Viewport& v) {
    VkViewport out{v.x, v.y, v.width, v.height, std::clamp(v.min_z, 0.0f, 1.0f),
                   std::clamp(v.max_z, 0.0f, 1.0f)};
    if (out.width <= 0.0f || out.height <= 0.0f) {
      const VkExtent2D extent = render_targets_->pass_extent();
      out.x = out.y = 0.0f;
      out.width = float(extent.width);
      out.height = float(extent.height);
    }
    return out;
  };
  for (const native::Command& command : frame_.commands) {
    if (command.type == native::Command::Type::kResolve) {
      render_targets_->Resolve(frame_.resolves[command.index], cmd);
      in_pass = false;
      continue;
    }
    if (command.type == native::Command::Type::kClear) {
      const native::ClearCommand& c = frame_.clears[command.index];
      if (!enter_pass(c.targets)) {
        continue;
      }
      VkClearAttachment attachments[2]{};
      uint32_t count = 0;
      if (c.colour) {
        attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[count].colorAttachment = 0;
        std::memcpy(attachments[count].clearValue.color.float32, c.colour_value,
                    sizeof(c.colour_value));
        ++count;
      }
      if (c.depth || c.stencil) {
        attachments[count].aspectMask = (c.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                        (c.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        attachments[count].clearValue.depthStencil = {std::clamp(c.depth_value, 0.0f, 1.0f),
                                                      c.stencil_value};
        ++count;
      }
      // The game's rectangles (the tiling strips), or the whole surface.
      const VkExtent2D extent = render_targets_->pass_extent();
      VkClearRect rects[16];
      uint32_t rect_count = 0;
      for (const auto& r : c.rects) {
        const int32_t x0 = std::clamp(r[0], 0, int32_t(extent.width));
        const int32_t y0 = std::clamp(r[1], 0, int32_t(extent.height));
        const int32_t x1 = std::clamp(r[2], x0, int32_t(extent.width));
        const int32_t y1 = std::clamp(r[3], y0, int32_t(extent.height));
        if (x1 > x0 && y1 > y0 && rect_count < 16) {
          rects[rect_count++] = {{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1};
        }
      }
      if (c.rects.empty()) {
        rects[rect_count++] = {{{0, 0}, extent}, 0, 1};
      }
      if (count && rect_count) {
        dfn.vkCmdClearAttachments(cmd, count, attachments, rect_count, rects);
      }
      continue;
    }
    const native::DrawCommand& d = frame_.draws[command.index];
    const PreparedDraw& p = prepared_[command.index];
    if (!p.pipeline || !enter_pass(d.targets)) {
      continue;
    }
    if (p.pipeline != bound) {
      dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
      bound = p.pipeline;
    }
    const VkViewport viewport = to_vulkan(d.viewport);
    if (!viewport_set || std::memcmp(&viewport, &bound_viewport, sizeof(viewport)) != 0) {
      dfn.vkCmdSetViewport(cmd, 0, 1, &viewport);
      bound_viewport = viewport;
      viewport_set = true;
    }
    // Stencil reference and masks (dynamic in every scene pipeline; cheap
    // enough to set per draw).
    const VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
    dfn.vkCmdSetStencilReference(cmd, both, d.stencil.reference);
    dfn.vkCmdSetStencilCompareMask(cmd, both, d.stencil.read_mask);
    dfn.vkCmdSetStencilWriteMask(cmd, both, d.stencil.write_mask);
    // Meshes: their own buffers. Immediate draws: the frame's shared buffers,
    // vertices bound at the draw's own start (its layout may differ from the
    // previous draw's), indices at the start (the draw picks its first index).
    const VkBuffer vertices = d.mesh ? p.vertex_buffer : vertex_buffer;
    const VkDeviceSize vertex_offset = d.mesh ? 0 : vertex_base + d.vertex_offset;
    if (vertices != bound_vertices || vertex_offset != bound_vertex_offset) {
      dfn.vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &vertex_offset);
      bound_vertices = vertices;
      bound_vertex_offset = vertex_offset;
    }
    const VkBuffer indices =
        d.mesh ? p.index_buffer : d.index_count ? index_buffer : VkBuffer(VK_NULL_HANDLE);
    if (indices && indices != bound_indices) {
      dfn.vkCmdBindIndexBuffer(cmd, indices, d.mesh ? 0 : index_base, VK_INDEX_TYPE_UINT16);
      bound_indices = indices;
    }
    dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_layout_, 0, 1, &p.set,
                                0, nullptr);
    if (UsesBlock(d.material)) {
      // Everything is in the draw's MaterialBlock.
    } else if (d.material == native::Material::kBink) {
      BinkParams bink{};
      std::memcpy(bink.c, d.colour_matrix, sizeof(bink.c));
      bink.screen_size[0] = float(kWidth);
      bink.screen_size[1] = float(kHeight);
      dfn.vkCmdPushConstants(cmd, draw_layout_,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(bink), &bink);
    } else {
      SimpleParams params{};
      std::memcpy(params.mvp, d.mvp, sizeof(params.mvp));
      params.tint_fade[0] = d.tint[0];
      params.tint_fade[1] = d.tint[1];
      params.tint_fade[2] = d.tint[2];
      params.tint_fade[3] = d.fade;
      params.texture_size_fog_range[0] = p.texture_width;
      params.texture_size_fog_range[1] = p.texture_height;
      params.texture_size_fog_range[2] = d.fog_start;
      params.texture_size_fog_range[3] = d.fog_end;
      params.alpha_ref_fog_colour[0] = d.alpha_ref;
      params.alpha_ref_fog_colour[1] = d.fog_colour[0];
      params.alpha_ref_fog_colour[2] = d.fog_colour[1];
      params.alpha_ref_fog_colour[3] = d.fog_colour[2];
      params.flags[0] = (d.textured ? kFlagTextured : 0) |
                        (d.textured && d.palette.present() ? kFlagPalettized : 0) |
                        (d.fade_rgb ? kFlagFadeRgb : 0) | (d.alpha_test ? kFlagAlphaTest : 0) |
                        (d.layout.colour == native::VertexLayout::kAbsent ? kFlagNoColour : 0) |
                        (d.layout.uv == native::VertexLayout::kAbsent ? kFlagNoUv : 0) |
                        (d.fog ? kFlagFog : 0) | (d.fx_add ? kFlagFxAdd : 0) |
                        (d.block != native::DrawCommand::kNoBlock ? kFlagLights : 0);
      params.flags[1] = d.alpha_func;
      dfn.vkCmdPushConstants(cmd, draw_layout_,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(params), &params);
    }
    if (indices) {
      dfn.vkCmdDrawIndexed(cmd, d.index_count, 1, d.mesh ? 0 : d.first_index, 0, 0);
    } else {
      dfn.vkCmdDraw(cmd, d.vertex_count, 1, 0, 0);
    }
  }
  render_targets_->EndPass(cmd);

  // 5. The finished picture: what EndFrame resolved into the frontbuffer
  //    (our image standing for that texture, filled by the resolves above,
  //    strip by strip while tiling), copied into our frame image for the
  //    present pass. Not the resolve's source surface: the movies' EndFrame
  //    clears it right after the copy (findings/11). A frame without a
  //    frontbuffer resolve keeps the previous picture.
  native::GuestTexture frontbuffer;
  for (const native::ResolveCommand& r : frame_.resolves) {
    if (r.frontbuffer) {
      frontbuffer = r.dest;
    }
  }
  if (render_targets_->CopyToImage(frontbuffer, frame_image_, frame_layout_, kWidth, kHeight,
                                   cmd)) {
    frame_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else if (frame_layout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
    // Nothing shown yet: start from black, in the layout the present pass samples.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = frame_image_;
    barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    dfn.vkCmdClearColorImage(cmd, frame_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    frame_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  // A line every ~10 s (at 30 fps) so logs show what the native path covers.
  if (++frames_drawn_ % 300 == 1) {
    REXLOG_INFO(
        "NativeRenderer: game frame {}: {} of {} draws ({} meshes, {} with a material block), {} clears, {} vertex bytes; "
        "{} mesh buffers cached, {} KiB uploaded so far",
        frame_.number, drawn, frame_.draws.size(), meshes, block_drawn, frame_.clears.size(),
        frame_.vertex_data.size() * 4, buffer_cache_->buffer_count(),
        buffer_cache_->bytes_uploaded() / 1024);
  }
}

bool NativeRenderer::RecordAndSubmit(VulkanPresenter::VulkanGuestOutputRefreshContext& context) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();

  // Reuse the oldest slot, once the GPU is done with it.
  slot_index_ = (slot_index_ + 1) % kFramesInFlight;
  FrameSlot& slot = slots_[slot_index_];
  {
    CostTimer wait(cost_.gpu_wait_ms);
    tracker_->AwaitSubmissionCompletion(slot.submission);
  }
  dfn.vkResetCommandPool(device, slot.pool, 0);
  dfn.vkResetDescriptorPool(device, slot.descriptor_pool, 0);
  const uint64_t completed = tracker_->UpdateAndGetCompletedSubmission();
  {
    CostTimer timer(cost_.textures_ms);
    texture_cache_->BeginFrame(completed);
  }
  {
    CostTimer timer(cost_.buffers_ms);
    buffer_cache_->BeginFrame(completed);
  }
  render_targets_->BeginFrame(completed);
  vertex_pool_->Reclaim(completed);
  uniform_pool_->Reclaim(completed);
  // The tracker's current index is the one this submission's fence completes.
  const uint64_t submission = tracker_->GetCurrentSubmission();

  VkFramebuffer present_framebuffer = GetPresentFramebuffer(context.image_version(),
                                                            context.image_view());
  if (!present_framebuffer) {
    return false;
  }

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.cmd, &begin);

  // First frame only: fill the 1x1 white texture.
  if (!white_ready_) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = white_image_;
    barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
    dfn.vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange range = rex::ui::vulkan::util::InitializeSubresourceRange();
    dfn.vkCmdClearColorImage(slot.cmd, white_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white,
                             1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    white_ready_ = true;
  }

  // The game's display gamma, for the present pass.
  UpdateGammaRamp(slot.cmd, submission);

  // 1. Our frame: what the game drew.
  RecordScene(slot.cmd, submission, slot.descriptor_pool);

  // 2. Present: sample our frame into the presenter's image.
  const VkViewport viewport{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
  VkRenderPassBeginInfo present_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  present_begin.renderPass = present_pass_;
  present_begin.framebuffer = present_framebuffer;
  present_begin.renderArea = scissor;
  dfn.vkCmdBeginRenderPass(slot.cmd, &present_begin, VK_SUBPASS_CONTENTS_INLINE);
  dfn.vkCmdSetViewport(slot.cmd, 0, 1, &viewport);
  dfn.vkCmdSetScissor(slot.cmd, 0, 1, &scissor);
  dfn.vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
  dfn.vkCmdBindDescriptorSets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1,
                              &present_set_, 0, nullptr);
  dfn.vkCmdDraw(slot.cmd, 3, 1, 0, 0);
  dfn.vkCmdEndRenderPass(slot.cmd);

  if (dfn.vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
    return false;
  }
  // Upload data (vertices, texture pixels) must be visible before the GPU runs.
  vertex_pool_->FlushWrites();
  uniform_pool_->FlushWrites();
  texture_cache_->FlushUploads();
  buffer_cache_->FlushUploads();

  // Submit on the graphics queue the presenter requires.
  {
    auto fence = tracker_->AcquireFenceToAdvanceSubmission();
    const VulkanDevice::Queue::Acquisition queue =
        device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.cmd;
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence.fence()) != VK_SUCCESS) {
      fence.SubmissionFailedOrDropped();
      return false;
    }
  }
  slot.submission = submission;
  for (PresentFramebuffer& fb : present_framebuffers_) {
    if (fb.framebuffer == present_framebuffer) {
      fb.last_submission = submission;
    }
  }
  context.SetIs8bpc(true);  // our frame is 8 bits per channel
  return true;
}
