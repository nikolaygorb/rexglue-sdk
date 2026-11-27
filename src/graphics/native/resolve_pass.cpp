/**
 * @file        graphics/native/resolve_pass.cpp
 * @brief       Native renderer: resolves that convert, drawn into the destination
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              kResolveShader, InitResolvePipelines, ResolveWithShader), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/resolve_pass.h>

#include <algorithm>
#include <string>
#include <vector>

#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/vulkan/glsl_compile.h>
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
  kLogFramebuffer = 1u << 3,
};

// After "#version 460", the samplerless texture extension and SRC_DEPTH /
// SRC_MS (0 or 1).
constexpr char kResolveShaderBody[] = R"glsl(
layout(push_constant) uniform Constants {
  // Colour: 2^copy_dest_exp_bias, times 32 for 16-bit fixed sources. Depth:
  // 2 for float24 (held at half its value on the host), else 1.
  float scale;
  // copy_sample_select's samples.
  uint sample_mask;
  uint sample_count;
  uint padding;
  // Source pixel = destination pixel + this.
  ivec2 source_offset;
} c;

#if SRC_MS
layout(set = 0, binding = 0) uniform texture2DMS src;
#else
layout(set = 0, binding = 0) uniform texture2D src;
#endif

layout(location = 0) out vec4 out_color;

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) + c.source_offset;
  uint samples = c.sample_mask & ((1u << c.sample_count) - 1u);
#if SRC_DEPTH
  // One sample: depth doesn't average.
#if SRC_MS
  int sample_index = max(findLSB(samples), 0);
  float depth = texelFetch(src, pixel, sample_index).r;
#else
  float depth = texelFetch(src, pixel, 0).r;
#endif
  out_color = vec4(depth * c.scale, 0.0, 0.0, 0.0);
#else
#if SRC_MS
  vec4 sum = vec4(0.0);
  uint count = 0u;
  for (int i = 0; i < 4; ++i) {
    if (((samples >> uint(i)) & 1u) != 0u) {
      sum += texelFetch(src, pixel, i);
      ++count;
    }
  }
  vec4 color = sum / float(max(count, 1u));
#else
  vec4 color = texelFetch(src, pixel, 0);
#endif
  out_color = color * c.scale;
#endif
}
)glsl";

struct ResolveConstants {
  float scale;
  uint32_t sample_mask;
  uint32_t sample_count;
  uint32_t padding;
  int32_t source_offset[2];  // std430: an ivec2 at offset 16.
};
static_assert(sizeof(ResolveConstants) == 24);

constexpr uint32_t kSetsPerPool = 64;

}  // namespace

ResolvePass::~ResolvePass() {
  Shutdown();
}

void ResolvePass::LogOnce(uint32_t reason_bit, const char* message) {
  if (!(logged_ & reason_bit)) {
    logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

bool ResolvePass::Initialize(Submitter& submitter, RenderTargets& render_targets) {
  submitter_ = &submitter;
  render_targets_ = &render_targets;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constants.size = sizeof(ResolveConstants);
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
  for (uint32_t variant = 0; variant < 4; ++variant) {
    std::string source = "#version 460\n#extension GL_EXT_samplerless_texture_functions : require\n";
    source += "#define SRC_DEPTH " + std::to_string(variant >> 1) + "\n";
    source += "#define SRC_MS " + std::to_string(variant & 1) + "\n";
    source += kResolveShaderBody;
    std::vector<uint32_t> spirv;
    std::string error;
    if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, source, spirv, error)) {
      REXGPU_ERROR("Native renderer: resolve shader {} failed to compile: {}", variant, error);
      return false;
    }
    pixel_shaders_[variant] =
        util::CreateShaderModule(vulkan_device, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (pixel_shaders_[variant] == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

void ResolvePass::Shutdown() {
  if (!submitter_) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (auto& [key, pipeline] : pipelines_) {
      if (pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline, nullptr);
      }
    }
    for (auto& [format, render_pass] : render_passes_) {
      if (render_pass != VK_NULL_HANDLE) {
        dfn.vkDestroyRenderPass(device, render_pass, nullptr);
      }
    }
    for (auto& [target, source] : sources_) {
      if (source.depth_view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, source.depth_view, nullptr);
      }
    }
    // Destroying the pools frees their sets.
    for (VkDescriptorPool pool : pools_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (VkShaderModule& module : pixel_shaders_) {
      util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, module);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, vertex_shader_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, set_layout_);
  }
  pipelines_.clear();
  render_passes_.clear();
  sources_.clear();
  pools_.clear();
  pool_sets_left_ = 0;
  logged_ = 0;
  submitter_ = nullptr;
  render_targets_ = nullptr;
}

bool ResolvePass::CanDrawInto(VkFormat format) const {
  return submitter_ && render_targets_ &&
         (render_targets_->OptimalTilingFeatures(format) &
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
}

const ResolvePass::Source* ResolvePass::GetSource(RenderTarget& src) {
  auto it = sources_.find(&src);
  if (it != sources_.end()) {
    return it->second.set != VK_NULL_HANDLE ? &it->second : nullptr;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  Source& source = sources_[&src];
  VkImageView view = src.view;
  if (src.depth) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = src.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = src.vk_format;
    view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT);
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &source.depth_view) != VK_SUCCESS) {
      source.depth_view = VK_NULL_HANDLE;
      return nullptr;
    }
    view = source.depth_view;
  }
  if (!pool_sets_left_) {
    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_size.descriptorCount = kSetsPerPool;
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kSetsPerPool;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
      LogOnce(kLogDescriptors, "failed to create a resolve descriptor pool");
      return nullptr;
    }
    pools_.push_back(pool);
    pool_sets_left_ = kSetsPerPool;
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = pools_.back();
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &source.set) != VK_SUCCESS) {
    source.set = VK_NULL_HANDLE;
    LogOnce(kLogDescriptors, "failed to allocate a resolve descriptor set");
    return nullptr;
  }
  --pool_sets_left_;
  VkDescriptorImageInfo image_info = {};
  image_info.imageView = view;
  // The layout Draw moves the source to.
  image_info.imageLayout = src.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = source.set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  return &source;
}

VkRenderPass ResolvePass::GetRenderPass(VkFormat format) {
  auto it = render_passes_.find(format);
  if (it != render_passes_.end()) {
    return it->second;
  }
  // Loaded and stored: only the scissored rectangle changes.
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
    LogOnce(kLogPipeline, "failed to create a resolve render pass");
  }
  render_passes_.emplace(format, render_pass);
  return render_pass;
}

VkPipeline ResolvePass::GetPipeline(bool src_depth, bool src_multisampled, VkFormat dest_format) {
  uint64_t key = uint64_t(uint32_t(dest_format)) | (uint64_t(src_depth) << 32) |
                 (uint64_t(src_multisampled) << 33);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }
  VkRenderPass render_pass = GetRenderPass(dest_format);
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (render_pass != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel_shaders_[(uint32_t(src_depth) << 1) | uint32_t(src_multisampled)];
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
    LogOnce(kLogPipeline, "failed to create a resolve pipeline");
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

VkFramebuffer ResolvePass::GetFramebuffer(ResolvedTexture& dest, VkRenderPass render_pass) {
  if (dest.framebuffer != VK_NULL_HANDLE) {
    return dest.framebuffer;
  }
  VkFramebufferCreateInfo framebuffer_info = {};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = render_pass;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &dest.view;
  framebuffer_info.width = dest.host_width;
  framebuffer_info.height = dest.host_height;
  framebuffer_info.layers = 1;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device->functions().vkCreateFramebuffer(vulkan_device->device(), &framebuffer_info,
                                                     nullptr, &dest.framebuffer) != VK_SUCCESS) {
    dest.framebuffer = VK_NULL_HANDLE;
    LogOnce(kLogFramebuffer, "failed to create a resolve framebuffer");
  }
  return dest.framebuffer;
}

bool ResolvePass::Draw(RenderTarget& src, ResolvedTexture& dest, const Rect& host_rect,
                       float scale, uint32_t sample_mask, int32_t source_x, int32_t source_y) {
  if (!submitter_ || host_rect.empty() || !dest.attachment) {
    return false;
  }
  bool multisampled = src.samples != VK_SAMPLE_COUNT_1_BIT;
  VkPipeline pipeline = GetPipeline(src.depth, multisampled, dest.vk_format);
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }
  VkRenderPass render_pass = GetRenderPass(dest.vk_format);
  VkFramebuffer framebuffer = GetFramebuffer(dest, render_pass);
  const Source* source = GetSource(src);
  if (framebuffer == VK_NULL_HANDLE || !source) {
    return false;
  }
  CommandRecorder* command_buffer =
      submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve, GpuKind::kResolveImage);
  if (command_buffer == nullptr) {
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = submitter_->vulkan_device()->functions();
  render_targets_->Transition(command_buffer, src,
                              src.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // A new destination is cleared first: the pass draws only the rectangle.
  TransitionTrackedImage(dfn, command_buffer, dest.image, VK_IMAGE_ASPECT_COLOR_BIT, dest.layout,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = framebuffer;
  begin_info.renderArea.extent = {dest.host_width, dest.host_height};
  command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkViewport viewport = {};
  viewport.width = float(dest.host_width);
  viewport.height = float(dest.host_height);
  viewport.maxDepth = 1.0f;
  command_buffer->CmdVkSetViewport(0, 1, &viewport);
  VkRect2D scissor;
  scissor.offset = {host_rect.left, host_rect.top};
  scissor.extent = {uint32_t(host_rect.right - host_rect.left),
                    uint32_t(host_rect.bottom - host_rect.top)};
  command_buffer->CmdVkSetScissor(0, 1, &scissor);
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                          &source->set, 0, nullptr);
  ResolveConstants constants = {};
  constants.scale = scale;
  constants.sample_mask = sample_mask;
  constants.sample_count = uint32_t(src.samples);
  constants.source_offset[0] = source_x;
  constants.source_offset[1] = source_y;
  command_buffer->CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                     sizeof(constants), &constants);
  command_buffer->CmdVkDraw(3, 1, 0, 0);
  command_buffer->CmdVkEndRenderPass();
  // Pipelines and dynamic state bound in the open command buffer.
  render_targets_->NoteGraphicsStateChange();
  return true;
}

}  // namespace rex::graphics::native
