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
// MILESTONE 2 (this file, 2026-09-25): the plumbing, with a test picture
//   * Shares the SDK's Vulkan device (the presenter's), so our images can be
//     handed to the window without copies between devices.
//   * Once per game frame (after xnDisplay::SwapBuffers, via
//     pddi::AddFrameEndListener) it draws its own frame into its own image.
//     For now that's a test picture: a quad rotating with the game's frame
//     counter (proof that it runs in step with the game).
//   * Hands the frame to the window with Presenter::RefreshGuestOutput, the
//     same door the emulated GPU uses. The presenter's image can be drawn
//     into but not copied into, so a tiny "present" pass samples our image
//     into it.
//   * F9 (keybind "bind_renderer", changeable in the F4 settings) switches
//     the window between the emulated picture and the native one. While the
//     native one is shown, Presenter::SetGuestOutputExternal(true) tells the
//     emulated GPU to stop refreshing the window (SDK patch 0004). The game
//     itself keeps running on the emulated GPU either way ("shadow mode").
//
// FLAGS
//   --renderer=emulated   (default) start on the emulated picture
//   --renderer=native     start on the native picture
//
// THREADS
//   OnFrameEnd runs on the game's main thread; the F9 callback on the UI
//   thread; Vulkan submissions go to the device's graphics queue 0, taken
//   through VulkanDevice::AcquireQueue (the queue is shared with the
//   emulator and the presenter, and Vulkan queues need external locking).
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/submission_tracker.h>

REXCVAR_DECLARE(std::string, renderer);

namespace rex::ui {
class Presenter;
}

class NativeRenderer {
 public:
  // Returns null (and logs why) if the presenter isn't a Vulkan one or setup
  // fails; the game then simply keeps the emulated picture.
  static std::unique_ptr<NativeRenderer> Create(rex::ui::Presenter* presenter);
  ~NativeRenderer();

  NativeRenderer(const NativeRenderer&) = delete;
  NativeRenderer& operator=(const NativeRenderer&) = delete;

  // Which picture the window shows. Any thread.
  void SetShowNative(bool show);
  bool show_native() const { return show_native_.load(std::memory_order_relaxed); }

 private:
  using VulkanPresenter = rex::ui::vulkan::VulkanPresenter;
  using VulkanDevice = rex::ui::vulkan::VulkanDevice;

  // Our frame's size and format. Fixed for now; resolution options come with
  // the real renderer (the presenter accepts any size).
  static constexpr uint32_t kWidth = 1280;
  static constexpr uint32_t kHeight = 720;
  static constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
  // Command buffers in flight before the CPU waits for the GPU.
  static constexpr size_t kFramesInFlight = 3;

  explicit NativeRenderer(VulkanPresenter* presenter);
  bool Initialize();
  void DestroyVulkanObjects();

  static void FrameEndThunk(void* self) { static_cast<NativeRenderer*>(self)->OnFrameEnd(); }
  void OnFrameEnd();

  // The presenter's callback: record our frame + the present pass, submit.
  bool RecordAndSubmit(VulkanPresenter::VulkanGuestOutputRefreshContext& context);
  // A framebuffer for one of the presenter's guest-output images (cached:
  // the presenter rotates between a few images, each with its own version).
  VkFramebuffer GetPresentFramebuffer(uint64_t image_version, VkImageView image_view);

  VkRenderPass CreateRenderPass(VkFormat format, VkAttachmentLoadOp load_op,
                                VkPipelineStageFlags next_stages, VkAccessFlags next_access);
  VkPipeline CreatePipeline(VkPipelineLayout layout, VkRenderPass render_pass,
                            const uint32_t* vs, size_t vs_size, const uint32_t* fs,
                            size_t fs_size);

  VulkanPresenter* presenter_;
  const VulkanDevice* device_;
  std::unique_ptr<rex::ui::vulkan::VulkanSubmissionTracker> tracker_;

  std::atomic<bool> show_native_{false};
  uint64_t game_frame_ = 0;  // game frames seen (main thread only)

  // Our frame: image + the pass that draws the test picture into it.
  VkImage frame_image_ = VK_NULL_HANDLE;
  VkDeviceMemory frame_memory_ = VK_NULL_HANDLE;
  VkImageView frame_view_ = VK_NULL_HANDLE;
  VkRenderPass scene_pass_ = VK_NULL_HANDLE;
  VkFramebuffer scene_framebuffer_ = VK_NULL_HANDLE;
  VkPipelineLayout test_layout_ = VK_NULL_HANDLE;
  VkPipeline test_pipeline_ = VK_NULL_HANDLE;

  // The present pass: our frame -> the presenter's guest-output image.
  VkRenderPass present_pass_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
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

  // Command buffers, used round-robin.
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t submission = 0;  // tracker index of its last submission (0 = never)
  };
  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;
};
