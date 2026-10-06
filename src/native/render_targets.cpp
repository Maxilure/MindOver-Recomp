// =============================================================================
// native/render_targets.cpp -- see render_targets.h for the why and how
// =============================================================================

#include "render_targets.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "guest.h"
#include "scan.h"

namespace native {

namespace {
// The multisampled depth copy's shaders (SPIR-V, compiled at build time from
// shaders/depth_copy.*).
constexpr uint32_t kDepthCopyVs[] =
#include "depth_copy.vert.h"
    ;
constexpr uint32_t kDepthCopyFs[] =
#include "depth_copy.frag.h"
    ;

// Push constants of the depth copy (shaders/depth_copy.frag): xy = source
// pixel minus destination pixel, z = the Vulkan sample to read.
struct DepthCopyParams {
  int32_t offset_x, offset_y, sample, unused;
};

// Which Vulkan sample holds the Xbox's sample `xbox` (the emulator's mapping,
// SpirvShaderTranslator::FSI_LoadSampleMask in the SDK): the Xbox counts 2x
// samples top, bottom and 4x ones top-left, bottom-left, top-right,
// bottom-right; Vulkan's standard patterns are bottom-right, top-left (2x)
// and top-left, top-right, bottom-left, bottom-right (4x).
int32_t VulkanSampleOf(uint32_t xbox, VkSampleCountFlagBits samples) {
  static constexpr int32_t k2x[2] = {1, 0};
  static constexpr int32_t k4x[4] = {0, 2, 1, 3};
  return samples == VK_SAMPLE_COUNT_4_BIT ? k4x[xbox & 3] : k2x[xbox & 1];
}
}  // namespace

using rex::ui::vulkan::VulkanDevice;

namespace {

void ReportOnce(const std::string& what) {
  static std::mutex mutex;
  static std::unordered_set<std::string> reported;
  std::lock_guard<std::mutex> lock(mutex);
  if (reported.insert(what).second) {
    REXLOG_INFO("NativeRenderer: not drawn natively yet: {}", what);
    native::scan::NewGap(what);  // with --trace: record this spot (scan.h)
  }
}

// The guest address of a texture's pixels (its fetch constant's word 1, bits
// 12-31), which identifies a resolve destination: the texture object the
// game resolves into and the one it later samples describe the same memory.
// Made physical: D3D texture objects hold virtual addresses, D3D's copy of
// the GPU fetch constants physical ones (guest.h, PhysicalAddress).
uint32_t TextureKey(const GuestTexture& t) {
  return guest::PhysicalAddress(t.fetch[1] & 0xFFFFF000u);
}

// A texture fetch constant's format (word 1, bits 0-5: xenos::TextureFormat).
uint32_t TextureFormatOf(const GuestTexture& t) { return t.fetch[1] & 0x3F; }
constexpr uint32_t kFormat24_8 = 22, kFormat24_8Float = 23;  // depth + stencil

// Its size (fetch word 2 for 2D textures: width - 1 in bits 0-12, height - 1
// in bits 13-25).
void TextureSize(const GuestTexture& t, uint32_t& width, uint32_t& height) {
  width = (t.fetch[2] & 0x1FFF) + 1;
  height = ((t.fetch[2] >> 13) & 0x1FFF) + 1;
}

// What a layout is used for: the stages and accesses on either side of a
// transition (Transition()).
struct LayoutUse {
  VkPipelineStageFlags stages;
  VkAccessFlags writes;  // what a later user must wait for
  VkAccessFlags access;  // what a user in this layout does
};
LayoutUse UseOf(VkImageLayout layout) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT};
    default:  // UNDEFINED: nothing to wait for
      return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0};
  }
}

VkImageAspectFlags AspectOf(bool depth) {
  return depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
               : VK_IMAGE_ASPECT_COLOR_BIT;
}

}  // namespace

RenderTargets::RenderTargets(const VulkanDevice* device, VkFormat colour_format,
                             VkFormat depth_format)
    : device_(device), colour_format_(colour_format), depth_format_(depth_format) {}

RenderTargets::~RenderTargets() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [key, image] : surfaces_) DestroyImage(image);
  for (auto& [key, image] : textures_) DestroyImage(image);
  for (auto& [key, image] : depth_copies_) DestroyImage(image);
  for (auto& [key, image] : stencil_copies_) DestroyImage(image);
  if (staging_.buffer) vmaDestroyBuffer(allocator_, staging_.buffer, staging_.allocation);
  for (Buffer& b : buffer_graveyard_) vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
  for (Image& image : scratch_depth_) DestroyImage(image);
  for (Image& image : image_graveyard_) DestroyImage(image);
  for (Framebuffer& fb : framebuffers_) dfn.vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
  for (Framebuffer& fb : framebuffer_graveyard_) {
    dfn.vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
  }
  for (VkRenderPass pass : render_passes_) {
    if (pass) dfn.vkDestroyRenderPass(device, pass, nullptr);
  }
  if (copy_pipeline_) dfn.vkDestroyPipeline(device, copy_pipeline_, nullptr);
  if (copy_layout_) dfn.vkDestroyPipelineLayout(device, copy_layout_, nullptr);
  if (copy_pool_) dfn.vkDestroyDescriptorPool(device, copy_pool_, nullptr);
  if (copy_set_layout_) dfn.vkDestroyDescriptorSetLayout(device, copy_set_layout_, nullptr);
  if (copy_sampler_) dfn.vkDestroySampler(device, copy_sampler_, nullptr);
  if (copy_pass_) dfn.vkDestroyRenderPass(device, copy_pass_, nullptr);
  if (allocator_) vmaDestroyAllocator(allocator_);
}

bool RenderTargets::Initialize() {
  allocator_ = rex::ui::vulkan::CreateVmaAllocator(device_, true);
  if (!allocator_) {
    return false;
  }
  // Core Vulkan 1.0 commands the SDK's function table doesn't load.
  const auto& ifn = device_->vulkan_instance()->functions();
  cmd_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(
      ifn.vkGetDeviceProcAddr(device_->device(), "vkCmdCopyImage"));
  cmd_clear_depth_stencil_image_ = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
      ifn.vkGetDeviceProcAddr(device_->device(), "vkCmdClearDepthStencilImage"));
  cmd_resolve_image_ = reinterpret_cast<PFN_vkCmdResolveImage>(
      ifn.vkGetDeviceProcAddr(device_->device(), "vkCmdResolveImage"));
  free_descriptor_sets_ = reinterpret_cast<PFN_vkFreeDescriptorSets>(
      ifn.vkGetDeviceProcAddr(device_->device(), "vkFreeDescriptorSets"));
  if (!cmd_copy_image_ || !cmd_clear_depth_stencil_image_ || !cmd_resolve_image_ ||
      !free_descriptor_sets_) {
    return false;
  }

  // The sample counts this GPU can give a surface: drawn into (colour, depth
  // and stencil), and its depth and stencil read by the depth copy shader.
  // Vulkan promises 1x and 4x for all of these; 2x is near universal.
  {
    const VulkanDevice::Properties& props = device_->properties();
    VkFormatProperties depth_features;
    ifn.vkGetPhysicalDeviceFormatProperties(device_->physical_device(), depth_format_,
                                            &depth_features);
    const bool depth_sampled =
        (depth_features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    msaa_supported_ = VK_SAMPLE_COUNT_1_BIT;
    if (depth_sampled) {
      msaa_supported_ |= props.framebufferColorSampleCounts & props.framebufferDepthSampleCounts &
                         props.framebufferStencilSampleCounts &
                         props.sampledImageDepthSampleCounts &
                         props.sampledImageStencilSampleCounts &
                         (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT);
    }
  }

  // Both attachments keep their contents across passes (loaded and stored)
  // and stay in their attachment layouts; the transitions around passes are
  // explicit barriers (Transition()), so no subpass dependencies are needed.
  // One pass per sample count: a pipeline must match its pass's.
  VkAttachmentDescription attachments[2]{};
  attachments[0].format = colour_format_;
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[1] = attachments[0];
  attachments[1].format = depth_format_;
  attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colour_ref;
  subpass.pDepthStencilAttachment = &depth_ref;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 2;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  const VkSampleCountFlagBits counts[3] = {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                                           VK_SAMPLE_COUNT_4_BIT};
  for (VkSampleCountFlagBits samples : counts) {
    if (!(msaa_supported_ & samples)) {
      continue;
    }
    attachments[0].samples = attachments[1].samples = samples;
    if (device_->functions().vkCreateRenderPass(device_->device(), &info, nullptr,
                                                &render_passes_[SampleIndex(samples)]) !=
        VK_SUCCESS) {
      return false;
    }
  }
  // Multisampled surfaces need the depth copy shader for their depth
  // resolves; without it, none.
  if (msaa_supported_ != VK_SAMPLE_COUNT_1_BIT && !CreateDepthCopyPipeline()) {
    REXLOG_WARN("NativeRenderer: no depth copy shader: surfaces drawn without MSAA");
    msaa_supported_ = VK_SAMPLE_COUNT_1_BIT;
  }
  return true;
}

bool RenderTargets::CreateDepthCopyPipeline() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  // The pass: the depth copy (R32F) and the stencil copy (R8) as two colour
  // attachments, loaded (only the resolve's rectangle is written) and
  // stored, kept in COLOR_ATTACHMENT_OPTIMAL like the surfaces' passes.
  VkAttachmentDescription attachments[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    attachments[i].format = i == 0 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8_UNORM;
    attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  const VkAttachmentReference refs[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                         {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 2;
  subpass.pColorAttachments = refs;
  VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 2;
  pass_info.pAttachments = attachments;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  if (dfn.vkCreateRenderPass(device, &pass_info, nullptr, &copy_pass_) != VK_SUCCESS) {
    return false;
  }
  // Its inputs: the depth and the stencil of the multisampled surface, read
  // one sample at a time (texelFetch: no filtering; the sampler is required
  // by the binding type only).
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &copy_sampler_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding bindings[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &copy_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  // One set per multisampled depth image (a hub frame has one); retired
  // images keep theirs until the GPU is done, hence some room.
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 16};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 16;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &copy_pool_) != VK_SUCCESS) {
    return false;
  }
  const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DepthCopyParams)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &copy_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &copy_layout_) != VK_SUCCESS) {
    return false;
  }
  // The pipeline: one full-screen triangle (no vertex buffer), both outputs
  // written as they are, viewport and scissor set per copy.
  VkShaderModule vs = rex::ui::vulkan::util::CreateShaderModule(device_, kDepthCopyVs,
                                                                sizeof(kDepthCopyVs));
  VkShaderModule fs = rex::ui::vulkan::util::CreateShaderModule(device_, kDepthCopyFs,
                                                                sizeof(kDepthCopyFs));
  if (vs && fs) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[i].module = i == 0 ? vs : fs;
      stages[i].pName = "main";
    }
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
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachments[2]{};
    blend_attachments[0].colorWriteMask = blend_attachments[1].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 2;
    blend.pAttachments = blend_attachments;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
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
    info.layout = copy_layout_;
    info.renderPass = copy_pass_;
    if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                      &copy_pipeline_) != VK_SUCCESS) {
      copy_pipeline_ = VK_NULL_HANDLE;
    }
  }
  if (vs) dfn.vkDestroyShaderModule(device, vs, nullptr);
  if (fs) dfn.vkDestroyShaderModule(device, fs, nullptr);
  return copy_pipeline_ != VK_NULL_HANDLE;
}

VkSampleCountFlagBits RenderTargets::SamplesFor(uint8_t msaa) const {
  const VkSampleCountFlagBits wanted = msaa >= 2   ? VK_SAMPLE_COUNT_4_BIT
                                       : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT
                                                   : VK_SAMPLE_COUNT_1_BIT;
  if (!(msaa_supported_ & wanted)) {
    ReportOnce(std::to_string(uint32_t(wanted)) + "x MSAA on this GPU (drawn without)");
    return VK_SAMPLE_COUNT_1_BIT;
  }
  return wanted;
}

VkSampleCountFlagBits RenderTargets::SurfaceSamples(uint32_t guest) const {
  auto it = surfaces_.find(guest);
  return it != surfaces_.end() && it->second.image ? it->second.samples : VK_SAMPLE_COUNT_1_BIT;
}

bool RenderTargets::CreateImage(Image& image, uint32_t width, uint32_t height, bool depth,
                                VkFormat format, VkSampleCountFlagBits samples) {
  const bool multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = depth                             ? depth_format_
                : format != VK_FORMAT_UNDEFINED ? format
                                                  : colour_format_;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // Attachment, copied from (resolves), cleared/copied into, sampled. A
  // multisampled colour surface is resolved (a transfer), never sampled; a
  // multisampled depth surface is sampled by the depth copy shader, never
  // copied from.
  if (!multisampled) {
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  } else if (depth) {
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
  } else {
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &info, &alloc_info, &image.image, &image.allocation, nullptr) !=
      VK_SUCCESS) {
    image = Image{};
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = info.format;
  view_info.subresourceRange = {AspectOf(depth), 0, 1, 0, 1};
  if (device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                             &image.view) != VK_SUCCESS) {
    DestroyImage(image);
    return false;
  }
  image.width = width;
  image.height = height;
  image.depth = depth;
  image.samples = samples;
  image.format = info.format;
  image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (multisampled && depth) {
    // The depth copy shader reads each aspect through its own view, bound by
    // a descriptor set made once here.
    const auto& dfn = device_->functions();
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkResult result = dfn.vkCreateImageView(device_->device(), &view_info, nullptr,
                                            &image.depth_view);
    view_info.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    if (result == VK_SUCCESS) {
      result = dfn.vkCreateImageView(device_->device(), &view_info, nullptr, &image.stencil_view);
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = copy_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &copy_set_layout_;
    if (result == VK_SUCCESS) {
      result = dfn.vkAllocateDescriptorSets(device_->device(), &alloc, &image.copy_set);
    }
    if (result != VK_SUCCESS) {
      image.copy_set = VK_NULL_HANDLE;
      DestroyImage(image);
      return false;
    }
    // Sampled in SHADER_READ_ONLY_OPTIMAL (ResolveDepthMultisampled).
    const VkDescriptorImageInfo images[2] = {
        {copy_sampler_, image.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {copy_sampler_, image.stencil_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = image.copy_set;
    write.dstBinding = 0;
    write.descriptorCount = 2;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = images;
    dfn.vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
  }
  return true;
}

void RenderTargets::DestroyImage(Image& image) {
  const auto& dfn = device_->functions();
  if (image.copy_set) {
    free_descriptor_sets_(device_->device(), copy_pool_, 1, &image.copy_set);
  }
  if (image.depth_view) dfn.vkDestroyImageView(device_->device(), image.depth_view, nullptr);
  if (image.stencil_view) dfn.vkDestroyImageView(device_->device(), image.stencil_view, nullptr);
  if (image.view) {
    dfn.vkDestroyImageView(device_->device(), image.view, nullptr);
  }
  if (image.image) {
    vmaDestroyImage(allocator_, image.image, image.allocation);
  }
  image = Image{};
}

void RenderTargets::BeginFrame(uint64_t completed) {
  completed_ = completed;
  const VulkanDevice::Functions& dfn = device_->functions();
  auto images_done = std::remove_if(image_graveyard_.begin(), image_graveyard_.end(),
                                    [&](Image& image) {
                                      if (image.last_submission > completed) return false;
                                      DestroyImage(image);
                                      return true;
                                    });
  image_graveyard_.erase(images_done, image_graveyard_.end());
  auto fbs_done = std::remove_if(framebuffer_graveyard_.begin(), framebuffer_graveyard_.end(),
                                 [&](Framebuffer& fb) {
                                   if (fb.last_submission > completed) return false;
                                   dfn.vkDestroyFramebuffer(device_->device(), fb.framebuffer,
                                                            nullptr);
                                   return true;
                                 });
  framebuffer_graveyard_.erase(fbs_done, framebuffer_graveyard_.end());
  auto buffers_done = std::remove_if(buffer_graveyard_.begin(), buffer_graveyard_.end(),
                                     [&](Buffer& b) {
                                       if (b.last_submission > completed) return false;
                                       vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
                                       return true;
                                     });
  buffer_graveyard_.erase(buffers_done, buffer_graveyard_.end());
}

void RenderTargets::Transition(Image& image, VkImageLayout layout, VkCommandBuffer cmd) {
  const LayoutUse before = UseOf(image.layout), after = UseOf(layout);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = before.writes;
  barrier.dstAccessMask = after.access;
  barrier.oldLayout = image.layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {AspectOf(image.depth), 0, 1, 0, 1};
  device_->functions().vkCmdPipelineBarrier(cmd, before.stages, after.stages, 0, 0, nullptr, 0,
                                            nullptr, 1, &barrier);
  image.layout = layout;
  image.last_submission = recording_submission_;
}

void RenderTargets::ClearNew(Image& image, VkCommandBuffer cmd) {
  const VulkanDevice::Functions& dfn = device_->functions();
  Transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);
  const VkImageSubresourceRange range{AspectOf(image.depth), 0, 1, 0, 1};
  if (image.depth) {
    // ("far_value", not "far": Windows' headers define near / far as macros)
    const VkClearDepthStencilValue far_value{0.0f, 0};  // reversed depth: 0 = far
    cmd_clear_depth_stencil_image_(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_value,
                                    1, &range);
  } else {
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    dfn.vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
  }
}

// Retires an image (and the framebuffers made with it) once the GPU is done.
#define RETIRE(image_ref)                                                             \
  do {                                                                                \
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {               \
      if (it->colour == (image_ref).image || it->depth == (image_ref).image) {        \
        framebuffer_graveyard_.push_back(*it);                                        \
        it = framebuffers_.erase(it);                                                 \
      } else {                                                                        \
        ++it;                                                                         \
      }                                                                               \
    }                                                                                 \
    image_graveyard_.push_back(image_ref);                                            \
    (image_ref) = Image{};                                                            \
  } while (0)

RenderTargets::Image* RenderTargets::EnsureSurface(const Surface& surface, VkCommandBuffer cmd) {
  if (!surface.guest || !surface.width || !surface.height || surface.width > 8192 ||
      surface.height > 8192) {
    return nullptr;
  }
  if (!surface.depth && surface.format > 1) {
    // Only 8-bit RGBA (format 0, and 1 = the same with gamma) so far.
    ReportOnce("colour surface format " + std::to_string(surface.format) + " (drawn as RGBA8)");
  }
  const VkSampleCountFlagBits samples = SamplesFor(surface.msaa);
  Image& image = surfaces_[surface.guest];
  if (image.image && image.width == surface.width && image.height == surface.height &&
      image.depth == surface.depth && image.samples == samples) {
    return &image;
  }
  if (image.image) {
    RETIRE(image);
  }
  if (!CreateImage(image, surface.width, surface.height, surface.depth, VK_FORMAT_UNDEFINED,
                   samples)) {
    REXLOG_ERROR("NativeRenderer: can't create a {}x{} {}x MSAA surface image", surface.width,
                 surface.height, uint32_t(samples));
    surfaces_.erase(surface.guest);
    return nullptr;
  }
  REXLOG_DEBUG("NativeRenderer: surface {:08X}: {}x{} {} {}x MSAA", surface.guest,
               surface.width, surface.height, surface.depth ? "depth" : "colour",
               uint32_t(samples));
  ClearNew(image, cmd);
  return &image;
}

RenderTargets::Image* RenderTargets::EnsureColourImage(std::unordered_map<uint32_t, Image>& map,
                                                       uint32_t key, uint32_t width,
                                                       uint32_t height, VkFormat format,
                                                       VkCommandBuffer cmd) {
  if (!key) {
    return nullptr;
  }
  Image& image = map[key];
  if (image.image && image.width == width && image.height == height && image.format == format) {
    return &image;
  }
  if (image.image) {
    RETIRE(image);
  }
  if (!CreateImage(image, width, height, false, format)) {
    map.erase(key);
    return nullptr;
  }
  REXLOG_DEBUG("NativeRenderer: resolve texture {:08X}: {}x{} (format {})", key, width, height,
               uint32_t(format));
  ClearNew(image, cmd);
  Transition(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
  return &image;
}

RenderTargets::Image* RenderTargets::EnsureTexture(const GuestTexture& texture,
                                                   VkCommandBuffer cmd) {
  uint32_t width, height;
  TextureSize(texture, width, height);
  return EnsureColourImage(textures_, TextureKey(texture), width, height, colour_format_, cmd);
}

bool RenderTargets::EnsureDepthCopies(const GuestTexture& texture, VkCommandBuffer cmd) {
  uint32_t width, height;
  TextureSize(texture, width, height);
  const uint32_t key = TextureKey(texture);
  return EnsureColourImage(depth_copies_, key, width, height, VK_FORMAT_R32_SFLOAT, cmd) &&
         EnsureColourImage(stencil_copies_, key, width, height, VK_FORMAT_R8_UNORM, cmd);
}

bool RenderTargets::EnsureStaging(VkDeviceSize size) {
  if (staging_.buffer && staging_.size >= size) {
    return true;
  }
  if (staging_.buffer) {
    buffer_graveyard_.push_back(staging_);
    staging_ = Buffer{};
  }
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = std::max<VkDeviceSize>(size, 1280 * 720 * 4);  // one 720p depth copy at least
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateBuffer(allocator_, &info, &alloc_info, &staging_.buffer, &staging_.allocation,
                      nullptr) != VK_SUCCESS) {
    staging_ = Buffer{};
    return false;
  }
  staging_.size = info.size;
  return true;
}

RenderTargets::Image* RenderTargets::ScratchDepth(uint32_t width, uint32_t height,
                                                  VkSampleCountFlagBits samples,
                                                  VkCommandBuffer cmd) {
  Image& scratch = scratch_depth_[SampleIndex(samples)];
  if (scratch.image && scratch.width >= width && scratch.height >= height) {
    return &scratch;
  }
  width = std::max(width, scratch.width);
  height = std::max(height, scratch.height);
  if (scratch.image) {
    RETIRE(scratch);
  }
  if (!CreateImage(scratch, width, height, true, VK_FORMAT_UNDEFINED, samples)) {
    return nullptr;
  }
  ClearNew(scratch, cmd);
  return &scratch;
}

#undef RETIRE

void RenderTargets::Prepare(const Frame& frame, VkCommandBuffer cmd, uint64_t submission) {
  recording_submission_ = submission;
  for (const Surface& surface : frame.surfaces) {
    EnsureSurface(surface, cmd);
  }
  for (const ResolveCommand& resolve : frame.resolves) {
    if (resolve.depth) {
      EnsureDepthCopies(resolve.dest, cmd);
    } else {
      EnsureTexture(resolve.dest, cmd);
    }
  }
}

RenderTargets::TextureView RenderTargets::FindTexture(const GuestTexture& texture) const {
  const uint32_t format = TextureFormatOf(texture);
  const auto& map =
      format == kFormat24_8 || format == kFormat24_8Float ? depth_copies_ : textures_;
  auto it = map.find(TextureKey(texture));
  if (it == map.end() || !it->second.image) {
    return {};
  }
  return {it->second.view, it->second.width, it->second.height};
}

RenderTargets::TextureView RenderTargets::FindStencilCopy(const GuestTexture& texture) const {
  auto it = stencil_copies_.find(TextureKey(texture));
  if (it == stencil_copies_.end() || !it->second.image) {
    return {};
  }
  return {it->second.view, it->second.width, it->second.height};
}

VkFramebuffer RenderTargets::GetFramebuffer(Image& first, Image& second, VkRenderPass pass) {
  for (Framebuffer& fb : framebuffers_) {
    if (fb.colour == first.image && fb.depth == second.image) {
      fb.last_submission = recording_submission_;
      return fb.framebuffer;
    }
  }
  const VkImageView views[2] = {first.view, second.view};
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = pass;
  info.attachmentCount = 2;
  info.pAttachments = views;
  info.width = first.width;
  info.height = first.height;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (device_->functions().vkCreateFramebuffer(device_->device(), &info, nullptr,
                                               &framebuffer) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  framebuffers_.push_back({first.image, second.image, framebuffer, recording_submission_});
  return framebuffer;
}

bool RenderTargets::BeginPass(const Targets& targets, VkCommandBuffer cmd, bool& has_depth) {
  if (pass_colour_ && targets == pass_targets_) {
    has_depth = !IsScratch(pass_depth_);
    return true;
  }
  EndPass(cmd);
  auto colour_it = surfaces_.find(targets.colour);
  if (!targets.colour || colour_it == surfaces_.end() || !colour_it->second.image) {
    return false;
  }
  Image& colour = colour_it->second;
  // The game's depth surface, if it has one that covers the colour surface
  // with the same number of samples (Vulkan wants both attachments alike);
  // otherwise the scratch image (draws then don't test depth).
  Image* depth = nullptr;
  auto depth_it = surfaces_.find(targets.depth);
  if (targets.depth && depth_it != surfaces_.end() && depth_it->second.image &&
      depth_it->second.width >= colour.width && depth_it->second.height >= colour.height &&
      depth_it->second.samples == colour.samples) {
    depth = &depth_it->second;
  } else {
    if (targets.depth) {
      ReportOnce("a depth surface smaller than its colour surface, or with other MSAA");
    }
    depth = ScratchDepth(colour.width, colour.height, colour.samples, cmd);
  }
  if (!depth) {
    return false;
  }
  has_depth = !IsScratch(depth);
  Transition(colour, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cmd);
  Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, cmd);
  const VkRenderPass pass = render_pass(colour.samples);
  const VkFramebuffer framebuffer = GetFramebuffer(colour, *depth, pass);
  if (!framebuffer) {
    return false;
  }
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = pass;
  begin.framebuffer = framebuffer;
  begin.renderArea = {{0, 0}, {colour.width, colour.height}};
  device_->functions().vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  pass_colour_ = &colour;
  pass_depth_ = depth;
  pass_targets_ = targets;
  pass_extent_ = {colour.width, colour.height};
  return true;
}

void RenderTargets::EndPass(VkCommandBuffer cmd) {
  if (!pass_colour_) {
    return;
  }
  device_->functions().vkCmdEndRenderPass(cmd);
  pass_colour_ = pass_depth_ = nullptr;
}

void RenderTargets::Resolve(const ResolveCommand& r, VkCommandBuffer cmd) {
  EndPass(cmd);
  if (r.depth) {
    ResolveDepth(r, cmd);
    return;
  }
  auto src_it = surfaces_.find(r.source);
  auto dst_it = textures_.find(TextureKey(r.dest));
  if (src_it == surfaces_.end() || dst_it == textures_.end() || !src_it->second.image ||
      !dst_it->second.image) {
    return;
  }
  Image& src = src_it->second;
  Image& dst = dst_it->second;
  // Clamp the rectangle to both images.
  const int32_t x0 = std::clamp(r.x0, 0, int32_t(src.width));
  const int32_t y0 = std::clamp(r.y0, 0, int32_t(src.height));
  const int32_t x1 = std::clamp(r.x1, x0, int32_t(src.width));
  const int32_t y1 = std::clamp(r.y1, y0, int32_t(src.height));
  const int32_t dx = std::clamp(r.dest_x, 0, int32_t(dst.width));
  const int32_t dy = std::clamp(r.dest_y, 0, int32_t(dst.height));
  const int32_t w = std::min(x1 - x0, int32_t(dst.width) - dx);
  const int32_t h = std::min(y1 - y0, int32_t(dst.height) - dy);
  if (w <= 0 || h <= 0) {
    return;
  }
  Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cmd);
  Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);
  if (src.samples != VK_SAMPLE_COUNT_1_BIT) {
    // A multisampled surface: each pixel becomes the average of its samples,
    // what D3D asks the GPU for when the game names no sample (its resolve
    // picks "all fragments" for a 2x surface; findings/16). A resolve of one
    // chosen sample (never seen) is averaged too, and reported.
    if (r.sample_select >= 1 && r.sample_select <= 4) {
      ReportOnce("a colour resolve of one sample of an MSAA surface (averaged instead)");
    }
    VkImageResolve region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {x0, y0, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {dx, dy, 0};
    region.extent = {uint32_t(w), uint32_t(h), 1};
    cmd_resolve_image_(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {x0, y0, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {dx, dy, 0};
    region.extent = {uint32_t(w), uint32_t(h), 1};
    cmd_copy_image_(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  Transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
}

bool RenderTargets::CopyToImage(const GuestTexture& resolved, VkImage dest,
                                VkImageLayout dest_layout, uint32_t width, uint32_t height,
                                VkCommandBuffer cmd) {
  EndPass(cmd);
  auto it = textures_.find(TextureKey(resolved));
  if (!resolved.present() || it == textures_.end() || !it->second.image) {
    return false;
  }
  Image& src = it->second;
  Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cmd);
  // `dest` isn't one of ours: its barriers by hand.
  Image dst;
  dst.image = dest;
  dst.layout = dest_layout;
  Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {std::min(width, src.width), std::min(height, src.height), 1};
  cmd_copy_image_(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  Transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
  return true;
}

// A depth/stencil resolve: the rectangle of the depth surface, depth and
// stencil separately, into the destination's two copies (see the header).
// Each aspect: image -> staging buffer -> colour image, with barriers so the
// staging buffer's uses never overlap (earlier frames' included: a barrier
// covers everything submitted before it).
void RenderTargets::ResolveDepth(const ResolveCommand& r, VkCommandBuffer cmd) {
  const uint32_t key = TextureKey(r.dest);
  auto src_it = surfaces_.find(r.source);
  auto depth_it = depth_copies_.find(key);
  auto stencil_it = stencil_copies_.find(key);
  if (src_it == surfaces_.end() || depth_it == depth_copies_.end() ||
      stencil_it == stencil_copies_.end() || !src_it->second.image || !src_it->second.depth ||
      !depth_it->second.image || !stencil_it->second.image) {
    return;
  }
  Image& src = src_it->second;
  // Clamp the rectangle to both images (the copies share one size).
  const Image& dst_size = stencil_it->second;
  const int32_t x0 = std::clamp(r.x0, 0, int32_t(src.width));
  const int32_t y0 = std::clamp(r.y0, 0, int32_t(src.height));
  const int32_t x1 = std::clamp(r.x1, x0, int32_t(src.width));
  const int32_t y1 = std::clamp(r.y1, y0, int32_t(src.height));
  const int32_t dx = std::clamp(r.dest_x, 0, int32_t(dst_size.width));
  const int32_t dy = std::clamp(r.dest_y, 0, int32_t(dst_size.height));
  const int32_t w = std::min(x1 - x0, int32_t(dst_size.width) - dx);
  const int32_t h = std::min(y1 - y0, int32_t(dst_size.height) - dy);
  if (w <= 0 || h <= 0) {
    return;
  }
  if (src.samples != VK_SAMPLE_COUNT_1_BIT) {
    ResolveRect rect{x0, y0, dx, dy, w, h};
    ResolveDepthMultisampled(r, rect, src, depth_it->second, stencil_it->second, cmd);
    return;
  }
  if (!EnsureStaging(VkDeviceSize(w) * h * 4)) {
    return;
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cmd);
  auto buffer_barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = staging_.buffer;
    barrier.size = VK_WHOLE_SIZE;
    dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
  };
  auto copy_aspect = [&](VkImageAspectFlags aspect, Image& dst) {
    // The staging buffer's previous contents have been copied out.
    buffer_barrier(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = uint32_t(w);
    region.bufferImageHeight = uint32_t(h);
    region.imageSubresource = {aspect, 0, 0, 1};
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    dfn.vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging_.buffer, 1, &region);
    buffer_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {dx, dy, 0};
    dfn.vkCmdCopyBufferToImage(cmd, staging_.buffer, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
  };
  // The depth aspect copies out as 32-bit floats only from a float depth
  // format (D24's would be packed integers).
  if (depth_format_ == VK_FORMAT_D32_SFLOAT_S8_UINT) {
    copy_aspect(VK_IMAGE_ASPECT_DEPTH_BIT, depth_it->second);
  } else {
    ReportOnce("depth copies with a 24-bit depth buffer");
  }
  copy_aspect(VK_IMAGE_ASPECT_STENCIL_BIT, stencil_it->second);
  staging_.last_submission = recording_submission_;
}

// A depth/stencil resolve of a MULTISAMPLED surface: Vulkan can't copy such
// an image to a buffer, and the game wants one sample (a depth can't be
// averaged), so a full-screen triangle (shaders/depth_copy.*) reads that
// sample of each pixel of the rectangle and writes the depth and stencil
// copies as two colour attachments.
void RenderTargets::ResolveDepthMultisampled(const ResolveCommand& r, const ResolveRect& rect,
                                             Image& src, Image& depth_copy,
                                             Image& stencil_copy, VkCommandBuffer cmd) {
  if (!src.copy_set) {
    return;
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
  Transition(depth_copy, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cmd);
  Transition(stencil_copy, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cmd);
  const VkFramebuffer framebuffer = GetFramebuffer(depth_copy, stencil_copy, copy_pass_);
  if (!framebuffer) {
    return;
  }
  // Only the rectangle is drawn (and, loaded, the rest keeps its contents).
  const VkRect2D area{{rect.dx, rect.dy}, {uint32_t(rect.w), uint32_t(rect.h)}};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = copy_pass_;
  begin.framebuffer = framebuffer;
  begin.renderArea = area;
  dfn.vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_pipeline_);
  const VkViewport viewport{0.0f, 0.0f, float(depth_copy.width), float(depth_copy.height),
                            0.0f, 1.0f};
  dfn.vkCmdSetViewport(cmd, 0, 1, &viewport);
  dfn.vkCmdSetScissor(cmd, 0, 1, &area);
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_layout_, 0, 1,
                              &src.copy_set, 0, nullptr);
  // The game's choice (D3D flags bits 4-6: 1-4 = its sample 0-3; nothing or
  // an average = sample 0, as the GPU treats a depth resolve), as a Vulkan
  // sample.
  const uint32_t xbox_sample = r.sample_select >= 1 && r.sample_select <= 4
                                   ? uint32_t(r.sample_select - 1)
                                   : 0u;
  const DepthCopyParams params{rect.x0 - rect.dx, rect.y0 - rect.dy,
                               VulkanSampleOf(xbox_sample, src.samples), 0};
  dfn.vkCmdPushConstants(cmd, copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params),
                         &params);
  dfn.vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn.vkCmdEndRenderPass(cmd);
  Transition(depth_copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
  Transition(stencil_copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
}

}  // namespace native
