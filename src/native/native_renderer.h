// =============================================================================
// native/native_renderer.h -- our own Vulkan renderer (roadmap phase 4)
// =============================================================================
//
// WHERE THIS IS GOING (docs/04-native-renderer.md, docs/findings/08)
//   Today the game's picture comes from ReXGlue emulating the Xbox 360's GPU:
//   game -> Radical's renderer (PDDI) -> Microsoft's D3D -> GPU command
//   packets -> emulator -> Vulkan. The native renderer takes the calls at the
//   PDDI level instead (its Xbox backend, the xn* classes, is the only code
//   that talks to D3D) and draws them with Vulkan directly: no EDRAM
//   emulation, no 3-strip tiling, any resolution.
//
// MILESTONE 2 (2026-09-25): the plumbing
//   * Shares the SDK's Vulkan device (the presenter's), so our images can be
//     handed to the window without copies between devices.
//   * Hands its frame to the window with Presenter::RefreshGuestOutput, the
//     same door the emulated GPU uses. The presenter's image can be drawn
//     into but not copied into, so a tiny "present" pass samples our image
//     into it.
//   * F9 (keybind "bind_renderer", changeable in the F4 settings) switches
//     the window between the emulated picture and the native one. While the
//     native one is shown, Presenter::SetGuestOutputExternal(true) tells the
//     emulated GPU to stop refreshing the window (SDK patch 0004).
//
// MILESTONE 3: the game's 2D screens, drawn natively
//   * recorder.cpp watches the game's renderer calls ("shadow mode": the
//     emulated GPU still gets them too) and describes each frame as a list
//     of clears and draws (frame.h).
//   * After each xnDisplay::SwapBuffers (pddi::AddFrameEndListener) this
//     class draws that list into its own 1280x720 image: vertices from the
//     game, its textures through texture_cache.cpp, blending and alpha test
//     as the game's material set them, the "simple" material's shaders
//     rewritten in GLSL (shaders/simple.*).
//   * What isn't drawn yet (other materials, render-to-texture effects) is
//     listed once in the log by the recorder.
//
// MILESTONE 4 (this version, part 1 of 2): the 3D world (docs/findings/10)
//   * Static meshes: their vertex and index buffers go through
//     buffer_cache.cpp (uploaded once, re-checked by hash every frame) and
//     are drawn with the vertex layout their PDDI format flags describe.
//   * Render targets (render_targets.cpp): one image per D3D surface the
//     game draws into (32-bit float depth + 8-bit stencil for depth ones),
//     resolves as copies into images that stand for the resolved textures,
//     and the picture we show is what the game's EndFrame resolves into its
//     frontbuffer (our image of that texture, copied into our frame image).
//   * The game's depth test/write/compare, culling, colour write masks,
//     stencil (two-sided too), viewport per draw, and the simple material's
//     distance fog. The game's reversed depth is its viewport's flipped
//     depth range (guest.h), which Vulkan takes as is.
//   * Pipelines are cached per combination of material, topology, blending,
//     depth/stencil state, culling, colour mask and vertex layout
//     (PipelineKey).
//
// MILESTONE 4, PART 2: characters (docs/findings/11)
//   * Immediate geometry is drawn from the game's own vertex layout (raw
//     words, like meshes), and with indices (BeginIndexedPrims: the
//     characters, skinned by the game's CPU every frame).
//   * Two lit materials rewritten in GLSL: the character material
//     (shaders/character.*: normal map, 4 directional lights with
//     specular, rim light, Fx shadows) and the lit variant of the simple
//     material (shaders/lit.*: lights, point lights). Their many constants
//     don't fit in push constants: each such draw gets a native::LitParams
//     block in a per-frame uniform buffer (descriptor binding 5, now 9).
//
// MILESTONE 4, PART 3: shadows, the digging marker, reflections (findings/12, 13),
// particles and water (findings/14): the particle and water materials read
// the scene's depth / colour copies that the game resolves mid-frame, which
// the render targets stand in for. Water binds nine textures, so a draw's
// set now has 9 texture bindings and its block moved to binding 9.
//
// DUAL MODE (2026-09-27): our picture in a SECOND window
//   native_window.h opens a second window with its own presenter and hands
//   it over with SetWindowPresenter. While it's open our picture goes THERE,
//   and the main window stays on the emulated one: one game, both renderers
//   side by side, live. So the picture has one of three destinations:
//     native window open          -> the native window's presenter
//     else F9 set to native       -> the main window's presenter (presenter_)
//     else                        -> nowhere (nothing recorded or drawn)
//   F9 is ignored while the native window is open (each window already
//   shows one picture); closing the window restores what F9 had chosen.
//
// DEBUG FLAG
//   --debug_native_emulated_resolves (with --readback_resolve=full): sample
//   the emulated GPU's resolved textures instead of ours (findings/10 s. 5).
//
// FLAGS
//   --renderer=emulated        (default) start on the emulated picture
//   --renderer=native          start on the native picture
//   (--native_window: dual mode at start, native_window.h)
//
// THREADS
//   OnFrameEnd runs on the game's main thread; the F9 callback and
//   SetWindowPresenter on the UI thread; Vulkan submissions go to the
//   device's graphics queue 0, taken through VulkanDevice::AcquireQueue (the
//   queue is shared with the emulator and the presenters, and Vulkan queues
//   need external locking).
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/submission_tracker.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

#include "buffer_cache.h"
#include "frame.h"
#include "render_targets.h"
#include "texture_cache.h"

REXCVAR_DECLARE(std::string, renderer);
REXCVAR_DECLARE(bool, debug_native_emulated_resolves);

namespace rex::ui {
class Presenter;
}
namespace rex::graphics {
class CommandProcessor;
}

class NativeRenderer {
 public:
  // Returns null (and logs why) if the presenter isn't a Vulkan one or setup
  // fails; the game then simply keeps the emulated picture.
  // `gpu` (may be null): the emulated GPU, read for the game's gamma ramp.
  static std::unique_ptr<NativeRenderer> Create(rex::ui::Presenter* presenter,
                                                const rex::graphics::CommandProcessor* gpu);
  ~NativeRenderer();

  NativeRenderer(const NativeRenderer&) = delete;
  NativeRenderer& operator=(const NativeRenderer&) = delete;

  // Which picture the MAIN window shows (F9). Any thread. While the native
  // window is open the main window shows the emulated picture anyway; this
  // choice applies again once it closes.
  void SetShowNative(bool show);
  bool show_native() const { return show_native_.load(std::memory_order_relaxed); }

  // Dual mode (native_window.h): the second window's presenter, to draw our
  // picture into from now on (the main window goes back to the emulated
  // picture), or null when that window closes. UI thread. The presenter must
  // stay alive until this renderer is destroyed (the game's main thread may
  // be refreshing it while this is called).
  void SetWindowPresenter(rex::ui::Presenter* presenter);

  // --native_only: turns the emulated GPU's drawing off while its picture is
  // on no screen, on again when it is (SDK patch 0009). No-op without the flag.
  void UpdateEmulatedDrawing();

  // --emulated_draw_every: the emulated GPU draws every Nth frame while our
  // picture is drawn too (native_drawn), every frame otherwise or while a
  // photo is taken (SDK patch 0013). Called every frame.
  void UpdateEmulatedFrameRate(bool native_drawn);

  // --emulated_only: F9 and F8 are locked on the emulated picture.
  bool emulated_only() const;

  // Our picture is drawn somewhere (main window on native, or the native
  // window open): an F10 photo can then have both pictures of one frame.
  bool native_drawn() const {
    return show_native() || window_presenter_.load(std::memory_order_relaxed);
  }

 private:
  // Shows which picture the main window has now (overlay_banner.h, ~1.5 s).
  void AnnouncePicture();

  using VulkanPresenter = rex::ui::vulkan::VulkanPresenter;
  using VulkanDevice = rex::ui::vulkan::VulkanDevice;

  // Our frame's size and format. Fixed for now; resolution options come
  // later (the presenter accepts any size).
  static constexpr uint32_t kWidth = 1280;
  static constexpr uint32_t kHeight = 720;
  static constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
  // The depth buffer's format: 32-bit float depth (the game's reversed depth
  // wants float precision near 0) with an 8-bit stencil. Every desktop GPU
  // has one of these two; the first supported one is used.
  static constexpr VkFormat kDepthFormats[] = {VK_FORMAT_D32_SFLOAT_S8_UINT,
                                               VK_FORMAT_D24_UNORM_S8_UINT};
  // Command buffers in flight before the CPU waits for the GPU.
  static constexpr size_t kFramesInFlight = 3;
  // Draws per frame we have descriptor sets for (menus use ~100).
  static constexpr uint32_t kMaxDrawsPerFrame = 4096;

  NativeRenderer(VulkanPresenter* presenter, const rex::graphics::CommandProcessor* gpu);
  bool Initialize();
  void DestroyVulkanObjects();

  static void FrameEndThunk(void* self) { static_cast<NativeRenderer*>(self)->OnFrameEnd(); }
  void OnFrameEnd();

  // The presenter's callback: record the game's frame + the present pass, submit.
  bool RecordAndSubmit(VulkanPresenter::VulkanGuestOutputRefreshContext& context);
  // Draws frame_ into our image (inside the scene render pass).
  void RecordScene(VkCommandBuffer cmd, uint64_t submission, VkDescriptorPool descriptor_pool);
  // A framebuffer for one of the presenter's guest-output images (cached:
  // the presenter rotates between a few images, each with its own version).
  VkFramebuffer GetPresentFramebuffer(uint64_t image_version, VkImageView image_view);
  // Drops every cached present framebuffer (after the GPU is done with them):
  // when our picture moves to the other window's presenter. The cache only
  // holds as many as ONE presenter rotates through, and ages them by image
  // version, which two presenters number separately.
  void ForgetPresentFramebuffers();

  // A colour-only pass (the present pass).
  VkRenderPass CreateRenderPass(VkFormat format, VkAttachmentLoadOp load_op,
                                VkPipelineStageFlags next_stages, VkAccessFlags next_access);
  // The present pass's pipeline: a fullscreen triangle, no vertex buffers.
  VkPipeline CreatePresentPipeline();

  // Everything about a draw that Vulkan bakes into a pipeline. Compared and
  // hashed as raw bytes, so it has no padding and unused fields stay zero.
  // (Stencil reference and masks are dynamic state, set per draw.)
  struct PipelineKey {
    uint8_t material, topology;
    uint8_t blend_enable, blend_op, blend_src, blend_dst;
    uint8_t depth_test, depth_write, depth_func, cull, colour_mask;
    uint8_t stride, normal_offset, colour_offset, uv_offset;
    uint8_t stencil_enable;
    uint8_t stencil_front[4];  // compare, fail, depth fail, pass
    uint8_t stencil_back[4];
    uint8_t binormal_offset, tangent_offset;
    uint8_t samples;    // the render target's MSAA sample count (VkSampleCountFlagBits)
    uint8_t unused[5];  // keeps the size a multiple of 8 for the hash
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
  };
  static_assert(sizeof(PipelineKey) == 32);
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const;
  };
  // `has_depth`: whether the game has a depth surface bound for this draw
  // (without one, D3D doesn't test depth, whatever the depth state says).
  // `samples`: its colour surface's sample count (a pipeline must match its
  // render pass's, render_targets.h).
  static PipelineKey MakePipelineKey(const native::DrawCommand& draw, bool has_depth,
                                     VkSampleCountFlagBits samples);
  // The pipeline for a draw (cached by PipelineKey).
  VkPipeline GetPipeline(const native::DrawCommand& draw, bool has_depth,
                         VkSampleCountFlagBits samples);
  // A sampler as the game's fetch constant describes it (cached by all its
  // fields: filters, mips, anisotropy, clamping).
  VkSampler GetSampler(const native::SamplerState& state);
  // 1x1 white texture, bound when a draw has no texture (the shader ignores
  // it, but Vulkan wants every binding filled). Cleared to white in the
  // first frame's command buffer.
  bool CreateWhiteTexture();
  // The display gamma ramp (see gamma_image_): uploads it into `cmd` if the
  // game changed it since the last frame.
  bool CreateGammaImage();
  void UpdateGammaRamp(VkCommandBuffer cmd, uint64_t submission);

  VulkanPresenter* presenter_;
  const rex::graphics::CommandProcessor* gpu_;
  const VulkanDevice* device_;
  std::unique_ptr<rex::ui::vulkan::VulkanSubmissionTracker> tracker_;

  std::atomic<bool> show_native_{false};
  // The native window's presenter while that window is open (dual mode).
  std::atomic<VulkanPresenter*> window_presenter_{nullptr};
  // The presenter the last frame went to (main thread): a change means the
  // present framebuffers belong to the other presenter.
  VulkanPresenter* last_target_ = nullptr;
  native::Frame frame_;  // the frame the game just finished (main thread)

  // Our frame image: the finished picture (copied from the surface the game
  // resolves into its frontbuffer), sampled by the present pass.
  VkImage frame_image_ = VK_NULL_HANDLE;
  VkDeviceMemory frame_memory_ = VK_NULL_HANDLE;
  VkImageView frame_view_ = VK_NULL_HANDLE;
  VkImageLayout frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  // The game's surfaces and resolved textures (render_targets.h).
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
  // The GPU can bilinear-filter R32_SFLOAT images (the depth copies).
  bool depth_copy_linear_ = false;
  float max_lod_bias_ = 2.0f;  // VkPhysicalDeviceLimits::maxSamplerLodBias
  std::unique_ptr<native::RenderTargets> render_targets_;

  // The materials share one layout: 9 textures (bindings 0-8, see
  // kTexturesPerDraw), a uniform buffer block (binding 9: the draw's
  // MaterialBlock: lit, shadow, particle and water materials) and 128 bytes
  // of push constants (SimpleParams / BinkParams, the others).
  VkDescriptorSetLayout draw_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout draw_layout_ = VK_NULL_HANDLE;
  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  std::unique_ptr<native::TextureCache> texture_cache_;
  std::unique_ptr<native::BufferCache> buffer_cache_;
  // A frame's immediate vertices and indices (and the gamma ramp's staging).
  std::unique_ptr<rex::ui::vulkan::VulkanUploadBufferPool> vertex_pool_;
  // A frame's MaterialBlocks (uniform buffer: lit and shadow draws).
  std::unique_ptr<rex::ui::vulkan::VulkanUploadBufferPool> uniform_pool_;
  VkImage white_image_ = VK_NULL_HANDLE;
  VkDeviceMemory white_memory_ = VK_NULL_HANDLE;
  VkImageView white_view_ = VK_NULL_HANDLE;
  bool white_ready_ = false;

  // Per draw of the current frame, prepared before the render pass (texture
  // uploads can't happen inside one). Reused between frames.
  struct PreparedDraw {
    VkPipeline pipeline = VK_NULL_HANDLE;  // null = skip this draw
    VkDescriptorSet set = VK_NULL_HANDLE;
    float texture_width = 1, texture_height = 1;
    // Meshes: their buffers (from the buffer cache). Immediate draws use the
    // frame's shared vertex buffer.
    bool has_depth = true;  // the game has a depth surface bound
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
  };
  std::vector<PreparedDraw> prepared_;

  // The display gamma ramp: 256 entries per channel, applied by the present
  // pass like the emulated GPU applies it when the game swaps (the Xbox's
  // display hardware maps each 8-bit value through this table; the game sets
  // it, e.g. from its brightness calibration). Each entry is the GPU's
  // DC_LUT_30_COLOR register (10 bits blue | green << 10 | red << 20), which
  // is the bit layout of Vulkan's A2B10G10R10 with red and blue swapped:
  // uploaded as is, the image view swaps them back.
  VkImage gamma_image_ = VK_NULL_HANDLE;
  VkDeviceMemory gamma_memory_ = VK_NULL_HANDLE;
  VkImageView gamma_view_ = VK_NULL_HANDLE;
  std::array<uint32_t, 256> gamma_ramp_{};
  bool gamma_uploaded_ = false;

  // The present pass: our frame -> the presenter's guest-output image.
  VkRenderPass present_pass_ = VK_NULL_HANDLE;
  VkSampler present_sampler_ = VK_NULL_HANDLE;
  VkSampler point_sampler_ = VK_NULL_HANDLE;  // for the gamma ramp
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool present_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet present_set_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  struct PresentFramebuffer {
    uint64_t image_version;
    VkImageView view;
    VkFramebuffer framebuffer;
    uint64_t last_submission;  // tracker index of its last use
  };
  std::vector<PresentFramebuffer> present_framebuffers_;

  // Per frame in flight: command buffer + the descriptor sets of its draws.
  struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    uint64_t submission = 0;  // tracker index of its last submission (0 = never)
  };
  std::array<FrameSlot, kFramesInFlight> slots_;
  size_t slot_index_ = 0;

  // Stats for the periodic log line.
  uint64_t frames_drawn_ = 0;

  // What our frames cost the game's MAIN THREAD. OnFrameEnd runs there, so
  // the game starts its next frame that much later. WHY (2026-09-27): the
  // fight against the Ratnicians on Wumpa Island ran at ~50 fps in dual mode
  // (elsewhere on the island: 59.4),
  // and a frame rate alone can't say whether we are the reason. Summed over
  // kCostFrames frames, then logged by LogCost (one line; with
  // --debug_log_fps, frame_rate.cpp's "main thread per frame" line puts it
  // next to the game's own time): all of OnFrameEnd (without F10 photos), and
  // inside it the waits for the GPU (a frame slot, a present framebuffer),
  // the texture cache and the buffer cache (hashing guest memory, uploads).
  struct Cost {
    double total_ms = 0, gpu_wait_ms = 0, textures_ms = 0, buffers_ms = 0, worst_ms = 0;
    uint32_t frames = 0;
  };
  static constexpr uint32_t kCostFrames = 300;  // ~5 s at 60 fps
  Cost cost_;
  // Adds one frame's OnFrameEnd time; logs and restarts every kCostFrames.
  void LogCost(double frame_ms);
  // Textures a draw needed but we couldn't have (unsupported format, memory
  // not readable...), each (material, binding, format) logged once: the
  // draw is skipped, or for optional ones drawn with white instead.
  std::unordered_set<uint32_t> reported_textures_;
};
