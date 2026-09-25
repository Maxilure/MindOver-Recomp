// =============================================================================
// native/native_renderer.cpp -- see native_renderer.h for the why and how
// =============================================================================

#include "native_renderer.h"

#include <algorithm>
#include <cmath>

#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/vulkan/util.h>

#include "../pddi/intercept.h"

REXCVAR_DEFINE_STRING(renderer, "emulated", "CrashMoM",
                      "Picture shown at start: 'emulated' (ReXGlue's GPU emulation) or "
                      "'native' (our Vulkan renderer, in development). F9 switches");

namespace {

// SPIR-V of src/native/shaders/*, compiled by glslc at build time
// (CMakeLists.txt, into <build>/shaders/). Each file is "{0x07230203,...}".
constexpr uint32_t kTestSceneVs[] =
#include "test_scene.vert.h"
    ;
constexpr uint32_t kTestSceneFs[] =
#include "test_scene.frag.h"
    ;
constexpr uint32_t kPresentVs[] =
#include "present.vert.h"
    ;
constexpr uint32_t kPresentFs[] =
#include "present.frag.h"
    ;

// Push constants of the test picture (test_scene.vert/.frag).
struct TestSceneParams {
  float angle, aspect, unused0, unused1;
  float tint[4];
};

}  // namespace

// -----------------------------------------------------------------------------
// Creation / destruction
// -----------------------------------------------------------------------------

std::unique_ptr<NativeRenderer> NativeRenderer::Create(rex::ui::Presenter* presenter) {
  auto* vulkan_presenter = dynamic_cast<VulkanPresenter*>(presenter);
  if (!vulkan_presenter) {
    REXLOG_WARN("NativeRenderer: needs the Vulkan presenter; staying on the emulated picture");
    return nullptr;
  }
  std::unique_ptr<NativeRenderer> renderer(new NativeRenderer(vulkan_presenter));
  if (!renderer->Initialize()) {
    REXLOG_ERROR("NativeRenderer: Vulkan setup failed; staying on the emulated picture");
    return nullptr;  // the destructor cleans up whatever was created
  }
  return renderer;
}

NativeRenderer::NativeRenderer(VulkanPresenter* presenter)
    : presenter_(presenter), device_(presenter->vulkan_device()) {}

NativeRenderer::~NativeRenderer() {
  // Order matters: first make sure the game's main thread can't call us
  // any more (this waits for a frame in progress), then give the window back
  // to the emulated GPU, then wait for the GPU before destroying anything.
  pddi::RemoveFrameEndListener(&FrameEndThunk, this);
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

  // --- Our frame image (drawn into, then sampled by the present pass) ---
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kFrameFormat;
  image_info.extent = {kWidth, kHeight, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC: room for reading frames back later (captures, A/B diffs).
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
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

  // --- Scene pass: clear our image, draw, leave it ready for sampling ---
  scene_pass_ = CreateRenderPass(kFrameFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  if (!scene_pass_) {
    return false;
  }
  VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb_info.renderPass = scene_pass_;
  fb_info.attachmentCount = 1;
  fb_info.pAttachments = &frame_view_;
  fb_info.width = kWidth;
  fb_info.height = kHeight;
  fb_info.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_info, nullptr, &scene_framebuffer_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_range.size = sizeof(TestSceneParams);
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &test_layout_) != VK_SUCCESS) {
    return false;
  }
  test_pipeline_ = CreatePipeline(test_layout_, scene_pass_, kTestSceneVs, sizeof(kTestSceneVs),
                                  kTestSceneFs, sizeof(kTestSceneFs));
  if (!test_pipeline_) {
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
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &present_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &present_set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &alloc_info, &present_set_) != VK_SUCCESS) {
    return false;
  }
  // Our image never changes, so the set is written once.
  VkDescriptorImageInfo image_desc{sampler_, frame_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = present_set_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_desc;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  VkPipelineLayoutCreateInfo present_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  present_layout_info.setLayoutCount = 1;
  present_layout_info.pSetLayouts = &present_set_layout_;
  if (dfn.vkCreatePipelineLayout(device, &present_layout_info, nullptr, &present_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  present_pipeline_ = CreatePipeline(present_layout_, present_pass_, kPresentVs,
                                     sizeof(kPresentVs), kPresentFs, sizeof(kPresentFs));
  if (!present_pipeline_) {
    return false;
  }

  // --- Command buffers (graphics queue family: the presenter requires
  //     "graphics and compute queue 0" for guest-output work) ---
  for (Frame& frame : frames_) {
    VkCommandPoolCreateInfo cmd_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cmd_pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &cmd_pool_info, nullptr, &frame.pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = frame.pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device, &cmd_info, &frame.cmd) != VK_SUCCESS) {
      return false;
    }
  }

  // --- Hook up: F9, the start setting, the game's frame end ---
  rex::ui::RegisterBind("bind_renderer", "F9",
                        "Switch the picture between the emulated GPU and the native renderer",
                        [this] { SetShowNative(!show_native()); });
  const std::string& start = REXCVAR_GET(renderer);
  if (start != "emulated" && start != "native") {
    REXLOG_WARN("NativeRenderer: unknown --renderer=\"{}\" (emulated or native)", start);
  }
  SetShowNative(start == "native");
  pddi::AddFrameEndListener(&FrameEndThunk, this);
  REXLOG_INFO("NativeRenderer: ready ({}x{}), F9 switches emulated/native", kWidth, kHeight);
  return true;
}

void NativeRenderer::DestroyVulkanObjects() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  using rex::ui::vulkan::util::DestroyAndNullHandle;
  for (Frame& frame : frames_) {
    // Destroying the pool frees its command buffer too.
    DestroyAndNullHandle(dfn.vkDestroyCommandPool, device, frame.pool);
    frame.cmd = VK_NULL_HANDLE;
  }
  for (PresentFramebuffer& fb : present_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
  }
  present_framebuffers_.clear();
  DestroyAndNullHandle(dfn.vkDestroyPipeline, device, present_pipeline_);
  DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, present_layout_);
  DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, descriptor_pool_);  // frees the set
  present_set_ = VK_NULL_HANDLE;
  DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, present_set_layout_);
  DestroyAndNullHandle(dfn.vkDestroySampler, device, sampler_);
  DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, present_pass_);
  DestroyAndNullHandle(dfn.vkDestroyPipeline, device, test_pipeline_);
  DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, test_layout_);
  DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, scene_framebuffer_);
  DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, scene_pass_);
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
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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

// A pipeline with no vertex buffers (shaders make their own vertices),
// triangle lists, no culling, no blending, and viewport + scissor set per
// frame (dynamic state), so it works for any size.
VkPipeline NativeRenderer::CreatePipeline(VkPipelineLayout layout, VkRenderPass render_pass,
                                          const uint32_t* vs, size_t vs_size,
                                          const uint32_t* fs, size_t fs_size) {
  const VulkanDevice::Functions& dfn = device_->functions();
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
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
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
    tracker_->AwaitSubmissionCompletion(oldest->last_submission);
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

// -----------------------------------------------------------------------------
// Per frame
// -----------------------------------------------------------------------------

void NativeRenderer::SetShowNative(bool show) {
  show_native_.store(show, std::memory_order_relaxed);
  // Tell the emulated GPU whether it may still refresh the window
  // (SDK patch 0004). When switching back it simply takes over at its next swap.
  presenter_->SetGuestOutputExternal(show);
  REXLOG_INFO("NativeRenderer: showing the {} picture (F9 switches)",
              show ? "native" : "emulated");
}

void NativeRenderer::OnFrameEnd() {
  ++game_frame_;
  if (!show_native()) {
    return;
  }
  // The presenter picks one of its guest-output images and calls us back
  // right away, on this thread; we must submit our work before returning.
  presenter_->RefreshGuestOutput(
      kWidth, kHeight, 16, 9, [this](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        return RecordAndSubmit(
            static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context));
      });
}

bool NativeRenderer::RecordAndSubmit(VulkanPresenter::VulkanGuestOutputRefreshContext& context) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();

  // Reuse the oldest command buffer, once the GPU is done with it.
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  Frame& frame = frames_[frame_index_];
  tracker_->AwaitSubmissionCompletion(frame.submission);
  dfn.vkResetCommandPool(device, frame.pool, 0);

  VkFramebuffer present_framebuffer = GetPresentFramebuffer(context.image_version(),
                                                            context.image_view());
  if (!present_framebuffer) {
    return false;
  }

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(frame.cmd, &begin);

  const VkViewport viewport{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};

  // 1. Our frame: for milestone 2, the test picture.
  VkClearValue clear{};
  clear.color = {{0.02f, 0.03f, 0.08f, 1.0f}};  // dark navy
  VkRenderPassBeginInfo scene_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  scene_begin.renderPass = scene_pass_;
  scene_begin.framebuffer = scene_framebuffer_;
  scene_begin.renderArea = scissor;
  scene_begin.clearValueCount = 1;
  scene_begin.pClearValues = &clear;
  dfn.vkCmdBeginRenderPass(frame.cmd, &scene_begin, VK_SUBPASS_CONTENTS_INLINE);
  dfn.vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
  dfn.vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
  dfn.vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, test_pipeline_);
  TestSceneParams params{};
  params.angle = float(std::fmod(double(game_frame_) * 0.02, 2.0 * 3.14159265358979));
  params.aspect = float(kWidth) / float(kHeight);
  params.tint[0] = params.tint[1] = params.tint[2] = params.tint[3] = 1.0f;
  dfn.vkCmdPushConstants(frame.cmd, test_layout_,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(params), &params);
  dfn.vkCmdDraw(frame.cmd, 6, 1, 0, 0);
  dfn.vkCmdEndRenderPass(frame.cmd);

  // 2. Present: sample our frame into the presenter's image.
  VkRenderPassBeginInfo present_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  present_begin.renderPass = present_pass_;
  present_begin.framebuffer = present_framebuffer;
  present_begin.renderArea = scissor;
  dfn.vkCmdBeginRenderPass(frame.cmd, &present_begin, VK_SUBPASS_CONTENTS_INLINE);
  dfn.vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
  dfn.vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
  dfn.vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
  dfn.vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1,
                              &present_set_, 0, nullptr);
  dfn.vkCmdDraw(frame.cmd, 3, 1, 0, 0);
  dfn.vkCmdEndRenderPass(frame.cmd);

  if (dfn.vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) {
    return false;
  }

  // Submit on the graphics queue the presenter requires. The tracker's
  // current index is the one this submission's fence will complete.
  const uint64_t submission = tracker_->GetCurrentSubmission();
  {
    auto fence = tracker_->AcquireFenceToAdvanceSubmission();
    const VulkanDevice::Queue::Acquisition queue =
        device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.cmd;
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence.fence()) != VK_SUCCESS) {
      fence.SubmissionFailedOrDropped();
      return false;
    }
  }
  frame.submission = submission;
  for (PresentFramebuffer& fb : present_framebuffers_) {
    if (fb.framebuffer == present_framebuffer) {
      fb.last_submission = submission;
    }
  }
  context.SetIs8bpc(true);  // our frame is 8 bits per channel
  return true;
}
