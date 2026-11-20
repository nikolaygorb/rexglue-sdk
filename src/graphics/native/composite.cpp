/**
 * @file        graphics/native/composite.cpp
 * @brief       Native renderer: textures the game rendered, sampled at the resolution scale
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/composite.h>

#include <algorithm>
#include <string>

#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/native/texture_cache.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

// The SDK's clockwise fullscreen triangle (`xb buildshaders`).
namespace shaders {
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
}  // namespace shaders

namespace util = ui::vulkan::util;

namespace {

enum LogReason : uint32_t {
  kLogShader = 1u << 0,
  kLogPipeline = 1u << 1,
  kLogDescriptors = 1u << 2,
  kLogImage = 1u << 3,
};

// Mode 0, the base: the 1x texel under the host pixel's centre (nearest, so
// its value is kept as the game stored it). Mode 1, a piece: the resolve's
// host pixel at the same scale, offset.
constexpr char kCompositeShader[] = R"glsl(#version 460
#extension GL_EXT_samplerless_texture_functions : require
layout(push_constant) uniform Constants {
  ivec2 src_offset;
  vec2 base_scale;
  uint mode;
} c;
layout(set = 0, binding = 0) uniform texture2DArray src;
layout(set = 0, binding = 1) uniform sampler src_sampler;
layout(location = 0) out vec4 out_color;
void main() {
  if (c.mode == 0u) {
    out_color = textureLod(sampler2DArray(src, src_sampler),
                           vec3(gl_FragCoord.xy * c.base_scale, 0.0), 0.0);
  } else {
    out_color = texelFetch(src, ivec3(ivec2(gl_FragCoord.xy) + c.src_offset, 0), 0);
  }
}
)glsl";

struct CompositeConstants {
  int32_t src_offset[2];
  float base_scale[2];
  uint32_t mode;
};

constexpr uint32_t kSetsPerPool = 256;
// Frames a composite is kept unused.
constexpr uint64_t kUnusedFramesKept = 120;

}  // namespace

size_t Composites::KeyHasher::operator()(const Key& key) const {
  uint64_t values[4] = {key.base, key.width, key.height, uint64_t(key.format)};
  return size_t(XXH3_64bits(values, sizeof(values)));
}

Composites::~Composites() {
  Shutdown();
}

void Composites::LogOnce(uint32_t reason_bit, const char* message) {
  if (!(logged_ & reason_bit)) {
    logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

bool Composites::Initialize(Submitter& submitter, RenderTargets& render_targets) {
  submitter_ = &submitter;
  render_targets_ = &render_targets;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].pImmutableSamplers = &sampler_;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constants.size = sizeof(CompositeConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  vertex_shader_ = util::CreateShaderModule(vulkan_device, shaders::fullscreen_cw_vs,
                                            sizeof(shaders::fullscreen_cw_vs));
  if (vertex_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, kCompositeShader, spirv, error)) {
    REXGPU_ERROR("Native renderer: the composite shader failed to compile: {}", error);
    return false;
  }
  pixel_shader_ =
      util::CreateShaderModule(vulkan_device, spirv.data(), spirv.size() * sizeof(uint32_t));
  return pixel_shader_ != VK_NULL_HANDLE;
}

void Composites::Shutdown() {
  if (!submitter_) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (auto& [key, composite] : composites_) {
      for (auto& [swizzle, view] : composite.sampled_views) {
        dfn.vkDestroyImageView(device, view, nullptr);
      }
      util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, composite.framebuffer);
      util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, composite.view);
      util::DestroyAndNullHandle(dfn.vkDestroyImage, device, composite.image);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, composite.memory);
    }
    for (auto& [format, pipeline] : pipelines_) {
      if (pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline, nullptr);
      }
    }
    for (auto& [format, render_pass] : render_passes_) {
      if (render_pass != VK_NULL_HANDLE) {
        dfn.vkDestroyRenderPass(device, render_pass, nullptr);
      }
    }
    // Destroying the pools frees their sets.
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyDescriptorPool(device, pool_, nullptr);
    }
    for (auto& [submission, pool] : pools_in_flight_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (VkDescriptorPool pool : pools_free_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, pixel_shader_);
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, vertex_shader_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroySampler, device, sampler_);
  }
  composites_.clear();
  pipelines_.clear();
  render_passes_.clear();
  pool_ = VK_NULL_HANDLE;
  pool_sets_left_ = 0;
  pools_in_flight_.clear();
  pools_free_.clear();
  logged_ = 0;
  submitter_ = nullptr;
  render_targets_ = nullptr;
}

bool Composites::CanCompose(VkFormat format) const {
  constexpr VkFormatFeatureFlags kNeeded =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  return submitter_ && render_targets_ && format != VK_FORMAT_UNDEFINED &&
         (render_targets_->OptimalTilingFeatures(format) & kNeeded) == kNeeded;
}

VkRenderPass Composites::GetRenderPass(VkFormat format) {
  auto it = render_passes_.find(format);
  if (it != render_passes_.end()) {
    return it->second;
  }
  // Loaded and stored: pieces may cover only part of it, over the last base.
  VkAttachmentDescription attachment = {};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreateRenderPass(vulkan_device->device(), &render_pass_info,
                                                    nullptr, &render_pass) != VK_SUCCESS) {
    render_pass = VK_NULL_HANDLE;
    LogOnce(kLogPipeline, "failed to create a composite render pass");
  }
  render_passes_.emplace(format, render_pass);
  return render_pass;
}

VkPipeline Composites::GetPipeline(VkFormat format) {
  auto it = pipelines_.find(format);
  if (it != pipelines_.end()) {
    return it->second;
  }
  VkRenderPass render_pass = GetRenderPass(format);
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (render_pass != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel_shader_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.basePipelineIndex = -1;
    const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
    if (vulkan_device->functions().vkCreateGraphicsPipelines(
            vulkan_device->device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
  }
  if (pipeline == VK_NULL_HANDLE) {
    LogOnce(kLogPipeline, "failed to create a composite pipeline");
  }
  pipelines_.emplace(format, pipeline);
  return pipeline;
}

VkDescriptorSet Composites::AllocateSourceSet(VkImageView view) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Pools of submissions the GPU has finished, for reuse.
  uint64_t completed = submitter_->completed();
  while (!pools_in_flight_.empty() && pools_in_flight_.front().first <= completed) {
    VkDescriptorPool pool = pools_in_flight_.front().second;
    pools_in_flight_.pop_front();
    dfn.vkResetDescriptorPool(device, pool, 0);
    pools_free_.push_back(pool);
  }
  if (!pool_sets_left_) {
    if (pool_ != VK_NULL_HANDLE) {
      pools_in_flight_.emplace_back(pool_submission_, pool_);
      pool_ = VK_NULL_HANDLE;
    }
    if (!pools_free_.empty()) {
      pool_ = pools_free_.back();
      pools_free_.pop_back();
    } else {
      VkDescriptorPoolSize pool_sizes[2] = {};
      pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      pool_sizes[0].descriptorCount = kSetsPerPool;
      pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      pool_sizes[1].descriptorCount = kSetsPerPool;
      VkDescriptorPoolCreateInfo pool_info = {};
      pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pool_info.maxSets = kSetsPerPool;
      pool_info.poolSizeCount = 2;
      pool_info.pPoolSizes = pool_sizes;
      if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        LogOnce(kLogDescriptors, "failed to create a composite descriptor pool");
        return VK_NULL_HANDLE;
      }
    }
    pool_sets_left_ = kSetsPerPool;
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &set_layout_;
  VkDescriptorSet set;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) {
    LogOnce(kLogDescriptors, "failed to allocate a composite descriptor set");
    return VK_NULL_HANDLE;
  }
  --pool_sets_left_;
  // Used by the open (or next) submission.
  pool_submission_ = submitter_->current();
  VkDescriptorImageInfo image_info = {};
  image_info.imageView = view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  return set;
}

bool Composites::Create(const Key& key, uint32_t scale4, Composite& composite) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  composite.scale4 = scale4;
  composite.host_width = ScaleSize(key.width, scale4);
  composite.host_height = ScaleSize(key.height, scale4);
  VkRenderPass render_pass = GetRenderPass(key.format);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = key.format;
  image_info.extent = {composite.host_width, composite.host_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // A transfer destination for its first clear (TransitionTrackedImage).
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool created = util::CreateDedicatedAllocationImage(
      vulkan_device, image_info, util::MemoryPurpose::kDeviceLocal, composite.image,
      composite.memory);
  if (created) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = composite.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = key.format;
    view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    created = dfn.vkCreateImageView(device, &view_info, nullptr, &composite.view) == VK_SUCCESS;
  }
  if (created) {
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &composite.view;
    framebuffer_info.width = composite.host_width;
    framebuffer_info.height = composite.host_height;
    framebuffer_info.layers = 1;
    created = dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                      &composite.framebuffer) == VK_SUCCESS;
  }
  if (!created) {
    LogOnce(kLogImage, "failed to create a composite texture");
    util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, composite.framebuffer);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, composite.view);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, composite.image);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, composite.memory);
    return false;
  }
  composite.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void Composites::DestroyLater(Composite& composite) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  std::vector<VkImageView> views;
  for (auto& [swizzle, view] : composite.sampled_views) {
    views.push_back(view);
  }
  views.push_back(composite.view);
  VkFramebuffer framebuffer = composite.framebuffer;
  VkImage image = composite.image;
  VkDeviceMemory memory = composite.memory;
  // Recorded commands (this frame's draws) may still use it.
  submitter_->DestroyAfter([vulkan_device, views = std::move(views), framebuffer, image, memory]() {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);
    for (VkImageView view : views) {
      dfn.vkDestroyImageView(device, view, nullptr);
    }
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
  });
  composite = Composite();
}

VkImageView Composites::GetSampledView(const Key& key, Composite& composite,
                                       uint32_t host_swizzle) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  // Without view swizzles the shader swizzles.
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  host_swizzle &= 0xFFF;
  auto it = composite.sampled_views.find(host_swizzle);
  if (it != composite.sampled_views.end()) {
    return it->second;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = composite.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_info.format = key.format;
  view_info.components.r = GuestComponentSwizzle(host_swizzle, 0);
  view_info.components.g = GuestComponentSwizzle(host_swizzle, 1);
  view_info.components.b = GuestComponentSwizzle(host_swizzle, 2);
  view_info.components.a = GuestComponentSwizzle(host_swizzle, 3);
  view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view;
  if (vulkan_device->functions().vkCreateImageView(vulkan_device->device(), &view_info, nullptr,
                                                   &view) != VK_SUCCESS) {
    LogOnce(kLogImage, "failed to create a composite texture view");
    return VK_NULL_HANDLE;
  }
  composite.sampled_views.emplace(host_swizzle, view);
  return view;
}

VkImageView Composites::Get(const Key& key, uint32_t scale4, VkImageView base_view,
                            const std::vector<Piece>& pieces, Resolver& resolver,
                            uint32_t host_swizzle, uint64_t frame) {
  if (!submitter_ || !key.width || !key.height || !CanCompose(key.format)) {
    return VK_NULL_HANDLE;
  }
  VkPipeline pipeline = GetPipeline(key.format);
  if (pipeline == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  auto it = composites_.try_emplace(key).first;
  Composite& composite = it->second;
  if (composite.image != VK_NULL_HANDLE && composite.scale4 != scale4) {
    DestroyLater(composite);
  }
  if (composite.image == VK_NULL_HANDLE && !Create(key, scale4, composite)) {
    composites_.erase(it);
    return VK_NULL_HANDLE;
  }
  composite.last_frame = frame;

  // Drawn again when the pieces or their writes changed, or once per frame
  // with a base (guest memory under it may have changed without a resolve).
  std::vector<uint64_t> signature;
  signature.reserve(pieces.size() * 4);
  for (const Piece& piece : pieces) {
    signature.push_back(piece.texture->id);
    signature.push_back(piece.texture->serial);
    signature.push_back(uint64_t(uint32_t(piece.dest_x)) | (uint64_t(uint32_t(piece.dest_y)) << 32));
    signature.push_back(uint64_t(uint32_t(piece.source.left)) |
                        (uint64_t(uint32_t(piece.source.top)) << 16) |
                        (uint64_t(uint32_t(piece.source.right)) << 32) |
                        (uint64_t(uint32_t(piece.source.bottom)) << 48));
  }
  bool redraw = !composite.drawn || composite.drawn_base != base_view ||
                composite.drawn_signature != signature ||
                (base_view != VK_NULL_HANDLE && composite.last_drawn_frame != frame);
  if (redraw) {
    CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kComposite);
    if (command_buffer == nullptr) {
      return VK_NULL_HANDLE;
    }
    const ui::vulkan::VulkanDevice::Functions& dfn = submitter_->vulkan_device()->functions();
    // The sources' sets first (their views and layouts).
    VkDescriptorSet base_set = VK_NULL_HANDLE;
    if (base_view != VK_NULL_HANDLE) {
      base_set = AllocateSourceSet(base_view);
      if (base_set == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
      }
    }
    std::vector<VkDescriptorSet> piece_sets(pieces.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < pieces.size(); ++i) {
      ResolvedTexture& texture = *pieces[i].texture;
      if (texture.scale4 != scale4) {
        continue;
      }
      if (texture.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        resolver.Transition(command_buffer, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
      // Red and blue as the guest's memory has them (copy_dest_swap).
      VkImageView view = resolver.GetSampledView(texture, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
      piece_sets[i] = AllocateSourceSet(view);
      if (piece_sets[i] == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
      }
    }
    // A new one is cleared first (the base or a whole piece should cover it).
    TransitionTrackedImage(dfn, command_buffer, composite.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           composite.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = GetRenderPass(key.format);
    begin_info.framebuffer = composite.framebuffer;
    begin_info.renderArea.extent = {composite.host_width, composite.host_height};
    command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport = {};
    viewport.width = float(composite.host_width);
    viewport.height = float(composite.host_height);
    viewport.maxDepth = 1.0f;
    command_buffer->CmdVkSetViewport(0, 1, &viewport);
    const Rect whole = {0, 0, int32_t(composite.host_width), int32_t(composite.host_height)};
    auto draw = [&](VkDescriptorSet set, const Rect& host_rect, const CompositeConstants& c) {
      VkRect2D scissor;
      scissor.offset = {host_rect.left, host_rect.top};
      scissor.extent = {uint32_t(host_rect.right - host_rect.left),
                        uint32_t(host_rect.bottom - host_rect.top)};
      command_buffer->CmdVkSetScissor(0, 1, &scissor);
      command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                              1, &set, 0, nullptr);
      command_buffer->CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                         sizeof(c), &c);
      command_buffer->CmdVkDraw(3, 1, 0, 0);
    };
    if (base_set != VK_NULL_HANDLE) {
      CompositeConstants constants = {};
      constants.base_scale[0] = 1.0f / float(composite.host_width);
      constants.base_scale[1] = 1.0f / float(composite.host_height);
      constants.mode = 0;
      draw(base_set, whole, constants);
    }
    for (size_t i = 0; i < pieces.size(); ++i) {
      if (piece_sets[i] == VK_NULL_HANDLE) {
        continue;
      }
      const Piece& piece = pieces[i];
      const ResolvedTexture& texture = *piece.texture;
      // The piece's guest rectangle in the composite, to host pixels at the
      // shared scale; the source's host pixel is at the same offset from
      // its own origin.
      Rect dest = {piece.dest_x, piece.dest_y,
                   piece.dest_x + (piece.source.right - piece.source.left),
                   piece.dest_y + (piece.source.bottom - piece.source.top)};
      Rect dest_host = ScaleRect(dest, scale4);
      Rect source_host = ScaleRect(piece.source, scale4);
      Rect clipped = {std::max(dest_host.left, 0), std::max(dest_host.top, 0),
                      std::min({dest_host.right, whole.right,
                                dest_host.left + int32_t(texture.host_width) - source_host.left}),
                      std::min({dest_host.bottom, whole.bottom,
                                dest_host.top + int32_t(texture.host_height) - source_host.top})};
      if (clipped.empty()) {
        continue;
      }
      CompositeConstants constants = {};
      constants.src_offset[0] = source_host.left - dest_host.left;
      constants.src_offset[1] = source_host.top - dest_host.top;
      constants.mode = 1;
      draw(piece_sets[i], clipped, constants);
    }
    command_buffer->CmdVkEndRenderPass();
    ImageBarrier(dfn, command_buffer, composite.image, VK_IMAGE_ASPECT_COLOR_BIT,
                 composite.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    composite.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Pipelines and dynamic state bound in the open command buffer.
    render_targets_->NoteGraphicsStateChange();
    composite.drawn = true;
    composite.drawn_base = base_view;
    composite.drawn_signature = std::move(signature);
    composite.last_drawn_frame = frame;
  }
  return GetSampledView(key, composite, host_swizzle);
}

void Composites::EndFrame(uint64_t frame) {
  for (auto it = composites_.begin(); it != composites_.end();) {
    if (it->second.last_frame + kUnusedFramesKept < frame) {
      DestroyLater(it->second);
      it = composites_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace rex::graphics::native
