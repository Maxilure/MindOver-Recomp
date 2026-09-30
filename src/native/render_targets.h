// =============================================================================
// native/render_targets.h -- the game's render targets and resolves, in Vulkan
// =============================================================================
//
// WHAT THE GAME DOES (docs/findings/10)
//   On the Xbox 360 every draw lands in EDRAM, through a D3D "surface" (a
//   colour or depth render target). To reuse a picture the game RESOLVES a
//   rectangle of a surface into a texture, which later draws sample. A hub
//   frame: the water reflection is drawn and resolved; the surface is cleared
//   and the scene drawn (resolving depth and colour mid-way for particles and
//   water); the scene is resolved into a texture; a second, full-size surface
//   gets that texture back plus the post-processing (depth of field) and the
//   HUD; EndFrame resolves it into the frontbuffer the TV shows.
//
// WHAT THIS CLASS DOES
//   * One Vulkan image per surface (keyed by the address of the game's D3D
//     surface object), created on first sight, re-created if its size or
//     format changes. Tiled surfaces are full size (the recorder sizes them
//     by the tiling rectangles), and rendering is single-sample for now.
//   * One Vulkan image per resolve destination (keyed by the texture's
//     PHYSICAL address in guest memory, guest::PhysicalAddress: the texture
//     resolved into and one sampling it may name the same memory
//     differently). Draws sampling such a texture get this image
//     (FindTexture) instead of guest memory, which on our side never receives
//     the resolved pixels (the emulated GPU keeps them in its own buffers).
//   * Resolves of the DEPTH/STENCIL surface (findings/12) make two images per
//     destination: the depth (R32 float) and the stencil (R8, value / 255).
//     The game resolves both into one 32-bit-per-pixel texture; its shadow
//     material reads the stencil byte, soft particles and water the depth.
//     Vulkan can't copy a depth/stencil image into a colour one directly,
//     so each aspect goes through a staging buffer.
//   * Render passes: BeginPass(targets) opens a pass on a surface pair,
//     loading what's there (EDRAM keeps its contents too). Resolve() ends it
//     and copies the rectangle. Every image's current layout is tracked, and
//     each transition is a pipeline barrier that also orders the reads and
//     writes around it.
//   * MSAA (docs/findings/16): a surface the game made multisampled (the
//     hub's main colour and depth surfaces: 2x) gets a multisampled image,
//     with a render pass per sample count (render_pass(samples)). Its colour
//     resolves AVERAGE the samples (vkCmdResolveImage), as D3D does by
//     default. Its depth resolves take ONE sample (the game asks for its
//     sample 0): a multisampled image can't be copied to a buffer, so a small
//     shader pass (shaders/depth_copy.*) reads that sample into the depth
//     and stencil copies. The Xbox's sample 0 is Vulkan's sample 1 for 2x
//     (the emulator's mapping: Vulkan's standard 2x pattern has the two
//     samples bottom-right, top-left; the Xbox counts top first).
//     A GPU that can't do a sample count (drawing, or sampling depth and
//     stencil from it) gets 1x surfaces instead (logged once).
//
// LIMITS (for now): colour surfaces are all RGBA8 (the hub's are A8R8G8B8);
// only render target 0 is drawn: the game only ever sets target 1 to
// nothing (its motion blur is switched off on the Xbox, docs/findings/21),
// so its clears of target 1 (clear flag 0x2) clear nothing and are ignored;
// 1x depth copies need the D32_SFLOAT_S8_UINT depth format (the stencil
// copy works with either).
// =============================================================================

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/mem_alloc.h>

#include "frame.h"

namespace native {

class RenderTargets {
 public:
  // `colour_format` / `depth_format`: what surfaces are made of (the render
  // pass and every draw pipeline use these).
  RenderTargets(const rex::ui::vulkan::VulkanDevice* device, VkFormat colour_format,
                VkFormat depth_format);
  ~RenderTargets();  // the GPU must be done with everything (caller awaits)

  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  bool Initialize();
  // The render pass draws happen in (and pipelines are built for): one
  // colour + one depth/stencil attachment of `samples` samples, contents
  // loaded and stored. One per sample count (1, 2, 4).
  VkRenderPass render_pass(VkSampleCountFlagBits samples) const {
    return render_passes_[SampleIndex(samples)];
  }
  // The sample count of the image standing for the game's surface `guest`
  // (after Prepare; 1 if there's none). A draw's pipeline must match its
  // colour surface's.
  VkSampleCountFlagBits SurfaceSamples(uint32_t guest) const;

  // Start of a frame's recording: frees what the GPU finished with
  // (`completed` = last finished submission).
  void BeginFrame(uint64_t completed);

  // Before the frame's draws are prepared: an image for every surface and
  // every resolve destination of `frame` (new ones are cleared to black /
  // "far"). Records into `cmd`, outside any render pass.
  void Prepare(const Frame& frame, VkCommandBuffer cmd, uint64_t submission);

  // The image standing for `texture` if it's one the game resolves into.
  struct TextureView {
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
  };
  // (For a k_24_8 / k_24_8_FLOAT texture: the depth copy of a depth resolve.)
  TextureView FindTexture(const GuestTexture& texture) const;
  // The stencil copy of a depth resolve into `texture` (R8: stencil / 255).
  TextureView FindStencilCopy(const GuestTexture& texture) const;

  // --- Recording the frame's commands, in order ---
  // Opens a render pass on `targets` (nothing if it's already open). False
  // if the colour surface has no image (the draw or clear is then skipped).
  // `has_depth` tells whether the game has a depth surface bound; without
  // one, a scratch depth image fills the slot and draws must not test depth.
  bool BeginPass(const Targets& targets, VkCommandBuffer cmd, bool& has_depth);
  void EndPass(VkCommandBuffer cmd);
  VkExtent2D pass_extent() const { return pass_extent_; }
  // Copies the resolve's rectangle into its destination's image (averaging
  // the samples of a multisampled colour surface).
  void Resolve(const ResolveCommand& resolve, VkCommandBuffer cmd);
  // End of the frame: copies the image standing for `resolved` (a texture
  // the frame resolved into: the frontbuffer) into `dest` (the picture we
  // show), which is then left in SHADER_READ_ONLY_OPTIMAL. False if there's
  // no such image. `dest_layout` is dest's current layout.
  // (Not the resolve's source surface: the game may clear that right after
  // the resolve, as the movies' EndFrame does; findings/11.)
  bool CopyToImage(const GuestTexture& resolved, VkImage dest, VkImageLayout dest_layout,
                   uint32_t width, uint32_t height, VkCommandBuffer cmd);

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool depth = false;             // a depth/stencil image
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t last_submission = 0;
    // Multisampled depth/stencil images only: views of each aspect alone,
    // and the descriptor set that binds them to the depth copy shader
    // (made once with the image: its views never change).
    VkImageView depth_view = VK_NULL_HANDLE, stencil_view = VK_NULL_HANDLE;
    VkDescriptorSet copy_set = VK_NULL_HANDLE;
  };
  // render_passes_ / scratch_depth_ index of a sample count: 1 -> 0, 2 -> 1,
  // 4 -> 2 (the game's own MSAA numbering, xenos::MsaaSamples).
  static uint32_t SampleIndex(VkSampleCountFlagBits samples) {
    return samples == VK_SAMPLE_COUNT_4_BIT ? 2 : samples == VK_SAMPLE_COUNT_2_BIT ? 1 : 0;
  }
  struct Framebuffer {
    VkImage colour, depth;  // what it was made for (images can be re-created)
    VkFramebuffer framebuffer;
    uint64_t last_submission;
  };

  // `format`: for colour images, overrides the colour surface format (the
  // depth and stencil copies). `samples` > 1: a multisampled surface.
  bool CreateImage(Image& image, uint32_t width, uint32_t height, bool depth,
                   VkFormat format = VK_FORMAT_UNDEFINED,
                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  // The sample count a surface of the game's MSAA mode (xenos::MsaaSamples:
  // 0 = 1x, 1 = 2x, 2 = 4x) gets: the same, if this GPU can do it.
  VkSampleCountFlagBits SamplesFor(uint8_t msaa) const;
  // The render pass and pipeline of the multisampled depth copy
  // (shaders/depth_copy.*); false if they can't be made (then no MSAA).
  bool CreateDepthCopyPipeline();
  void DestroyImage(Image& image);
  // Moves `image` to `layout`, ordering everything before (any stage) with
  // everything after in the stages that use `layout`.
  void Transition(Image& image, VkImageLayout layout, VkCommandBuffer cmd);
  // A cleared new image for a surface / a resolve destination / the scratch depth.
  Image* EnsureSurface(const Surface& surface, VkCommandBuffer cmd);
  Image* EnsureTexture(const GuestTexture& texture, VkCommandBuffer cmd);
  // The depth and stencil copies for a depth resolve into `texture`.
  bool EnsureDepthCopies(const GuestTexture& texture, VkCommandBuffer cmd);
  // An image kept in `map` under `key`, `width` x `height` of `format`,
  // (re)created and cleared as needed, left readable by shaders.
  Image* EnsureColourImage(std::unordered_map<uint32_t, Image>& map, uint32_t key,
                           uint32_t width, uint32_t height, VkFormat format,
                           VkCommandBuffer cmd);
  void ResolveDepth(const ResolveCommand& resolve, VkCommandBuffer cmd);
  // A resolve's rectangle, clamped to both images: source corner, where it
  // lands, size.
  struct ResolveRect {
    int32_t x0, y0, dx, dy, w, h;
  };
  // ResolveDepth for a multisampled depth surface: one sample, by shader.
  void ResolveDepthMultisampled(const ResolveCommand& resolve, const ResolveRect& rect,
                                Image& src, Image& depth_copy, Image& stencil_copy,
                                VkCommandBuffer cmd);
  // The staging buffer depth resolves go through, at least `size` bytes.
  bool EnsureStaging(VkDeviceSize size);
  Image* ScratchDepth(uint32_t width, uint32_t height, VkSampleCountFlagBits samples,
                      VkCommandBuffer cmd);
  bool IsScratch(const Image* image) const {
    return image == &scratch_depth_[0] || image == &scratch_depth_[1] ||
           image == &scratch_depth_[2];
  }
  void ClearNew(Image& image, VkCommandBuffer cmd);
  // A framebuffer of `pass` on the two images (a surface pair, or the
  // depth copy's two outputs), cached until either image is retired.
  VkFramebuffer GetFramebuffer(Image& first, Image& second, VkRenderPass pass);

  const rex::ui::vulkan::VulkanDevice* device_;
  VkFormat colour_format_, depth_format_;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkRenderPass render_passes_[3] = {};  // by SampleIndex
  PFN_vkCmdCopyImage cmd_copy_image_ = nullptr;
  PFN_vkCmdClearDepthStencilImage cmd_clear_depth_stencil_image_ = nullptr;
  PFN_vkCmdResolveImage cmd_resolve_image_ = nullptr;
  PFN_vkFreeDescriptorSets free_descriptor_sets_ = nullptr;
  // Sample counts surfaces may use on this GPU (bit set = VkSampleCountFlagBits).
  VkSampleCountFlags msaa_supported_ = VK_SAMPLE_COUNT_1_BIT;
  // The multisampled depth copy (shaders/depth_copy.*): a pass writing the
  // depth (R32F) and stencil (R8) copies, its pipeline, and what binds the
  // source: a point sampler, one descriptor set per multisampled depth image.
  VkRenderPass copy_pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout copy_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout copy_layout_ = VK_NULL_HANDLE;
  VkPipeline copy_pipeline_ = VK_NULL_HANDLE;
  VkSampler copy_sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool copy_pool_ = VK_NULL_HANDLE;

  std::unordered_map<uint32_t, Image> surfaces_;  // key: guest D3D surface object
  std::unordered_map<uint32_t, Image> textures_;  // key: physical address of the texture
  std::unordered_map<uint32_t, Image> depth_copies_;    // R32_SFLOAT, same keys
  std::unordered_map<uint32_t, Image> stencil_copies_;  // R8_UNORM, same keys
  // Staging buffer for depth resolves (depth/stencil image -> buffer ->
  // colour image), and replaced ones waiting for the GPU.
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint64_t last_submission = 0;
  };
  Buffer staging_;
  std::vector<Buffer> buffer_graveyard_;
  Image scratch_depth_[3];  // by SampleIndex
  std::vector<Framebuffer> framebuffers_;
  // Replaced images and framebuffers, destroyed once the GPU is done.
  std::vector<Image> image_graveyard_;
  std::vector<Framebuffer> framebuffer_graveyard_;
  uint64_t completed_ = 0;
  uint64_t recording_submission_ = 0;  // the submission being recorded

  // The open render pass (none: colour_ == null).
  Image* pass_colour_ = nullptr;
  Image* pass_depth_ = nullptr;
  Targets pass_targets_;
  VkExtent2D pass_extent_{};
};

}  // namespace native
