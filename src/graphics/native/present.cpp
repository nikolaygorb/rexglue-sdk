/**
 * @file        graphics/native/present.cpp
 * @brief       Native renderer: the front buffer into the presenter's guest output
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The guest output pass follows ArmyOfTwoRecompilation
 *              (aot/src/gpu/native/renderer.cpp, RendererSwap), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/present.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/util/smaa_textures.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/graphics/vulkan/swap_fxaa_shader.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

// The SDK's swap shaders (`xb buildshaders`): a clockwise fullscreen triangle,
// and the ramps applied to texels fetched by pixel index.
namespace shaders {
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_ps.h"
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
#include "../shaders/vulkan_spirv/smaa_blending_weight_calculation_cs.h"
#include "../shaders/vulkan_spirv/smaa_edge_detection_cs.h"
#include "../shaders/vulkan_spirv/smaa_neighborhood_blending_cs.h"
}  // namespace shaders

namespace {
using ui::vulkan::VulkanPresenter;
namespace util = ui::vulkan::util;
}  // namespace

Present::~Present() {
  Shutdown();
}

bool Present::Initialize(Submitter& submitter) {
  submitter_ = &submitter;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Ramps, written by the CPU each swap.
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device, VkDeviceSize(kRampSlotSize) * kSlotCount,
          VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, util::MemoryPurpose::kUpload, ramp_buffer_,
          ramp_memory_, &ramp_memory_type_, &ramp_memory_size_)) {
    REXGPU_ERROR("Native renderer: failed to create the gamma ramp buffer");
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, ramp_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to map the gamma ramp buffer");
    return false;
  }
  ramp_mapping_ = static_cast<uint8_t*>(mapping);
  VkBufferViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  view_info.buffer = ramp_buffer_;
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    for (uint32_t pwl = 0; pwl < 2; ++pwl) {
      view_info.format =
          pwl ? VK_FORMAT_R16G16_UINT : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      view_info.offset = VkDeviceSize(kRampSlotSize) * slot + (pwl ? kRampTableSize : 0);
      view_info.range = pwl ? kRampPwlSize : kRampTableSize;
      if (dfn.vkCreateBufferView(device, &view_info, nullptr, &ramp_views_[slot][pwl]) !=
          VK_SUCCESS) {
        REXGPU_ERROR("Native renderer: failed to create a gamma ramp buffer view");
        return false;
      }
    }
  }

  // Set 0: the ramp; set 1: the source (texelFetch, no sampler).
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &ramp_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &source_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2 * kSlotCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotCount},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 3 * kSlotCount;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = descriptor_pool_;
  set_info.descriptorSetCount = 1;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    set_info.pSetLayouts = &ramp_set_layout_;
    for (uint32_t pwl = 0; pwl < 2; ++pwl) {
      if (dfn.vkAllocateDescriptorSets(device, &set_info, &ramp_sets_[slot][pwl]) !=
          VK_SUCCESS) {
        return false;
      }
      write.dstSet = ramp_sets_[slot][pwl];
      write.pTexelBufferView = &ramp_views_[slot][pwl];
      dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    set_info.pSetLayouts = &source_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &set_info, &source_sets_[slot]) != VK_SUCCESS) {
      return false;
    }
  }

  VkDescriptorSetLayout set_layouts[2] = {ramp_set_layout_, source_set_layout_};
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = set_layouts;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  // The image is written whole (cleared, then drawn), so its old contents are
  // dropped (initial layout UNDEFINED, which the presenter allows), and left as
  // the presenter expects it. The same for the effects' intermediate images
  // (their compute passes are ordered after it by a barrier).
  VkAttachmentDescription attachment = {};
  attachment.format = VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
  VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_reference;
  VkSubpassDependency dependencies[2] = {};
  // After the presenter's previous reads of the image (write after read: an
  // execution dependency).
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask =
      VulkanPresenter::kGuestOutputInternalStageMask | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = 0;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  // Before the presenter reads it.
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = VulkanPresenter::kGuestOutputInternalStageMask;
  dependencies[1].dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  render_pass_info.dependencyCount = 2;
  render_pass_info.pDependencies = dependencies;
  if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create the present render pass");
    return false;
  }
  attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr, &fxaa_render_pass_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create the FXAA source render pass");
    return false;
  }

  VkShaderModule vs = util::CreateShaderModule(vulkan_device, shaders::fullscreen_cw_vs,
                                               sizeof(shaders::fullscreen_cw_vs));
  // [0: table, 1: PWL], then the same writing the luma into .a (FXAA).
  VkShaderModule ps[4] = {
      util::CreateShaderModule(vulkan_device, shaders::apply_gamma_table_ps,
                               sizeof(shaders::apply_gamma_table_ps)),
      util::CreateShaderModule(vulkan_device, shaders::apply_gamma_pwl_ps,
                               sizeof(shaders::apply_gamma_pwl_ps)),
      util::CreateShaderModule(vulkan_device, shaders::apply_gamma_table_fxaa_luma_ps,
                               sizeof(shaders::apply_gamma_table_fxaa_luma_ps)),
      util::CreateShaderModule(vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_ps,
                               sizeof(shaders::apply_gamma_pwl_fxaa_luma_ps)),
  };
  bool pipelines_created = vs != VK_NULL_HANDLE && ps[0] != VK_NULL_HANDLE &&
                           ps[1] != VK_NULL_HANDLE;
  if (pipelines_created) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
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
    pipeline_info.renderPass = render_pass_;
    pipeline_info.basePipelineIndex = -1;
    for (uint32_t pwl = 0; pwl < 2 && pipelines_created; ++pwl) {
      stages[1].module = ps[pwl];
      pipelines_created = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                        &pipeline_info, nullptr,
                                                        &pipelines_[pwl]) == VK_SUCCESS;
    }
    // FXAA's: optional (without them, no FXAA).
    pipeline_info.renderPass = fxaa_render_pass_;
    for (uint32_t pwl = 0; pwl < 2 && pipelines_created; ++pwl) {
      stages[1].module = ps[2 + pwl];
      if (stages[1].module == VK_NULL_HANDLE ||
          dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                        &fxaa_luma_pipelines_[pwl]) != VK_SUCCESS) {
        fxaa_luma_pipelines_[pwl] = VK_NULL_HANDLE;
      }
    }
  }
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, vs);
  for (VkShaderModule& module : ps) {
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, module);
  }
  if (!pipelines_created) {
    REXGPU_ERROR("Native renderer: failed to create the present pipelines");
    return false;
  }
  effects_ready_ = InitializeEffects();
  if (!effects_ready_) {
    REXGPU_WARN("Native renderer: swap_post_effect is unavailable (its pipelines failed)");
  }
  return true;
}

void Present::Shutdown() {
  if (!submitter_) {
    return;
  }
  // The caller has waited for the GPU (Submitter::AwaitAll); the submitter is
  // shut down after this.
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ShutdownEffects();
    for (VkPipeline& pipeline : fxaa_luma_pipelines_) {
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fxaa_render_pass_);
    for (Framebuffer& framebuffer : framebuffers_) {
      util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, framebuffer.framebuffer);
      framebuffer = Framebuffer();
    }
    for (VkPipeline& pipeline : pipelines_) {
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, render_pass_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
    // Destroying the pool frees its sets.
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, descriptor_pool_);
    ramp_sets_ = {};
    source_sets_ = {};
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, source_set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, ramp_set_layout_);
    for (auto& slot_views : ramp_views_) {
      for (VkBufferView& view : slot_views) {
        util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device, view);
      }
    }
    util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, ramp_buffer_);
    // Freeing the memory unmaps it.
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, ramp_memory_);
    ramp_mapping_ = nullptr;
  }
  slot_submissions_ = {};
  next_slot_ = 0;
  submitter_ = nullptr;
}

VkFramebuffer Present::GetFramebuffer(
    const VulkanPresenter::VulkanGuestOutputRefreshContext& context, uint32_t width,
    uint32_t height) {
  // The presenter rotates through a few images; keep one framebuffer per image
  // version, replacing the oldest version.
  Framebuffer* replace = &framebuffers_[0];
  for (Framebuffer& framebuffer : framebuffers_) {
    if (framebuffer.framebuffer != VK_NULL_HANDLE &&
        framebuffer.image_version == context.image_version() &&
        framebuffer.view == context.image_view() && framebuffer.width == width &&
        framebuffer.height == height) {
      return framebuffer.framebuffer;
    }
    if (framebuffer.image_version < replace->image_version) {
      replace = &framebuffer;
    }
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (replace->framebuffer != VK_NULL_HANDLE) {
    // Recorded commands of frames still in flight may use it.
    VkFramebuffer old = replace->framebuffer;
    submitter_->DestroyAfter([vulkan_device, old]() {
      vulkan_device->functions().vkDestroyFramebuffer(vulkan_device->device(), old, nullptr);
    });
    *replace = Framebuffer();
  }
  VkImageView attachment = context.image_view();
  VkFramebufferCreateInfo framebuffer_info = {};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = render_pass_;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &attachment;
  framebuffer_info.width = width;
  framebuffer_info.height = height;
  framebuffer_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  replace->image_version = context.image_version();
  replace->view = attachment;
  replace->width = width;
  replace->height = height;
  replace->framebuffer = framebuffer;
  return framebuffer;
}

bool Present::InitializeEffects() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // As the SDK's Vulkan backend's swap sampler.
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxAnisotropy = 1.0f;
  sampler_info.compareOp = VK_COMPARE_OP_NEVER;
  sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &linear_clamp_sampler_) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &combined_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &storage_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  binding.pImmutableSamplers = &linear_clamp_sampler_;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &smaa_sampler_set_layout_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding source_bindings[kSmaaSourceCount] = {};
  for (uint32_t i = 0; i < kSmaaSourceCount; ++i) {
    source_bindings[i].binding = i;
    source_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    source_bindings[i].descriptorCount = 1;
    source_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  set_layout_info.bindingCount = kSmaaSourceCount;
  set_layout_info.pBindings = source_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &smaa_sources_set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constants.size = sizeof(EffectConstants);
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_constants;
  VkDescriptorSetLayout fxaa_set_layouts[2] = {combined_set_layout_, storage_set_layout_};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = fxaa_set_layouts;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &fxaa_pipeline_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout smaa_set_layouts[3] = {smaa_sampler_set_layout_, smaa_sources_set_layout_,
                                               storage_set_layout_};
  layout_info.setLayoutCount = 3;
  layout_info.pSetLayouts = smaa_set_layouts;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &smaa_pipeline_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  // FXAA from the GLSL shared with the SDK's Vulkan backend; each effect is
  // optional on its own (a missing pipeline turns it off, logged at use).
  for (uint32_t extreme = 0; extreme < 2; ++extreme) {
    std::vector<uint32_t> spirv;
    std::string error;
    if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT,
                                    vulkan::GetSwapFxaaComputeSource(extreme != 0), spirv,
                                    error)) {
      REXGPU_WARN("Native renderer: failed to compile the FXAA shader: {}", error);
      continue;
    }
    fxaa_pipelines_[extreme] = util::CreateComputePipeline(
        vulkan_device, fxaa_pipeline_layout_, spirv.data(), sizeof(uint32_t) * spirv.size());
  }
  const std::pair<const uint32_t*, size_t> smaa_shaders[kSmaaPassCount] = {
      {shaders::smaa_edge_detection_cs, sizeof(shaders::smaa_edge_detection_cs)},
      {shaders::smaa_blending_weight_calculation_cs,
       sizeof(shaders::smaa_blending_weight_calculation_cs)},
      {shaders::smaa_neighborhood_blending_cs, sizeof(shaders::smaa_neighborhood_blending_cs)},
  };
  for (uint32_t i = 0; i < kSmaaPassCount; ++i) {
    smaa_pipelines_[i] = util::CreateComputePipeline(vulkan_device, smaa_pipeline_layout_,
                                                     smaa_shaders[i].first,
                                                     smaa_shaders[i].second);
  }

  // Sets: per slot (rewritten once the slot's frame is done), and SMAA's
  // sampler once.
  VkDescriptorPoolSize pool_sizes[4] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlotCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSlotCount * (1 + kSmaaPassCount)},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotCount * kSmaaPassCount * kSmaaSourceCount},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1 + kSlotCount * (2 + 2 * kSmaaPassCount);
  pool_info.poolSizeCount = 4;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &effect_pool_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = effect_pool_;
  set_info.descriptorSetCount = 1;
  auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set_out) {
    set_info.pSetLayouts = &layout;
    return dfn.vkAllocateDescriptorSets(device, &set_info, &set_out) == VK_SUCCESS;
  };
  if (!allocate(smaa_sampler_set_layout_, smaa_sampler_set_)) {
    return false;
  }
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    if (!allocate(combined_set_layout_, fxaa_source_sets_[slot]) ||
        !allocate(storage_set_layout_, fxaa_destination_sets_[slot])) {
      return false;
    }
    for (uint32_t pass = 0; pass < kSmaaPassCount; ++pass) {
      if (!allocate(smaa_sources_set_layout_, smaa_source_sets_[slot][pass]) ||
          !allocate(storage_set_layout_, smaa_destination_sets_[slot][pass])) {
        return false;
      }
    }
  }
  return true;
}

void Present::ShutdownEffects() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (size_t i = 0; i < kEffectImageCount; ++i) {
    DestroyEffectImage(EffectImageIndex(i), true);
  }
  // Destroying the pool frees its sets.
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, effect_pool_);
  smaa_sampler_set_ = VK_NULL_HANDLE;
  fxaa_source_sets_ = {};
  fxaa_destination_sets_ = {};
  smaa_source_sets_ = {};
  smaa_destination_sets_ = {};
  for (VkPipeline& pipeline : fxaa_pipelines_) {
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  for (VkPipeline& pipeline : smaa_pipelines_) {
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, smaa_pipeline_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, fxaa_pipeline_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, smaa_sources_set_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, smaa_sampler_set_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, storage_set_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, combined_set_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroySampler, device, linear_clamp_sampler_);
  effects_ready_ = false;
}

void Present::DestroyEffectImage(EffectImageIndex index, bool now) {
  EffectImage image = effect_images_[index];
  effect_images_[index] = EffectImage();
  if (image.image == VK_NULL_HANDLE && image.memory == VK_NULL_HANDLE) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  auto destroy = [vulkan_device, image]() {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (image.framebuffer != VK_NULL_HANDLE) {
      dfn.vkDestroyFramebuffer(device, image.framebuffer, nullptr);
    }
    if (image.view != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, image.memory, nullptr);
    }
  };
  if (now) {
    destroy();
  } else {
    // Frames still in flight may use it.
    submitter_->DestroyAfter(std::move(destroy));
  }
}

bool Present::EnsureEffectImage(EffectImageIndex index, VkFormat format, VkImageUsageFlags usage,
                                uint32_t width, uint32_t height, VkRenderPass gamma_render_pass) {
  EffectImage& image = effect_images_[index];
  if (image.image != VK_NULL_HANDLE && image.width == width && image.height == height) {
    return true;
  }
  DestroyEffectImage(index, false);
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  EffectImage created;
  if (!util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                            util::MemoryPurpose::kDeviceLocal, created.image,
                                            created.memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = created.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  bool ok = dfn.vkCreateImageView(device, &view_info, nullptr, &created.view) == VK_SUCCESS;
  if (ok && gamma_render_pass != VK_NULL_HANDLE) {
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = gamma_render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &created.view;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    ok = dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &created.framebuffer) ==
         VK_SUCCESS;
  }
  created.width = width;
  created.height = height;
  image = created;
  if (!ok) {
    DestroyEffectImage(index, true);
    return false;
  }
  return true;
}

bool Present::EnsureSmaaLookupTextures(CommandRecorder* command_buffer) {
  if (effect_images_[kEffectImageSmaaArea].image != VK_NULL_HANDLE &&
      effect_images_[kEffectImageSmaaSearch].image != VK_NULL_HANDLE) {
    return true;
  }
  struct LookupTexture {
    EffectImageIndex index;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    const uint8_t* data;
  };
  const LookupTexture lookup_textures[2] = {
      {kEffectImageSmaaArea, VK_FORMAT_R8G8_UNORM, smaa::kAreaTextureWidth,
       smaa::kAreaTextureHeight, smaa::kAreaTexturePitch, smaa::GetAreaTextureData()},
      {kEffectImageSmaaSearch, VK_FORMAT_R8_UNORM, smaa::kSearchTextureWidth,
       smaa::kSearchTextureHeight, smaa::kSearchTexturePitch, smaa::GetSearchTextureData()},
  };
  for (const LookupTexture& texture : lookup_textures) {
    if (!EnsureEffectImage(texture.index, texture.format,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           texture.width, texture.height, VK_NULL_HANDLE)) {
      DestroyEffectImage(kEffectImageSmaaArea, true);
      DestroyEffectImage(kEffectImageSmaaSearch, true);
      return false;
    }
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Copies from a buffer start at multiples of the texel size and of 4.
  const VkDeviceSize offsets[2] = {
      0, rex::round_up(VkDeviceSize(smaa::kAreaTexturePitch) * smaa::kAreaTextureHeight,
                       VkDeviceSize(4), false)};
  VkDeviceSize upload_size =
      offsets[1] + VkDeviceSize(smaa::kSearchTexturePitch) * smaa::kSearchTextureHeight;
  VkBuffer buffer;
  VkDeviceMemory memory;
  uint32_t memory_type;
  VkDeviceSize memory_size;
  if (!util::CreateDedicatedAllocationBuffer(vulkan_device, upload_size,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             util::MemoryPurpose::kUpload, buffer, memory,
                                             &memory_type, &memory_size)) {
    DestroyEffectImage(kEffectImageSmaaArea, true);
    DestroyEffectImage(kEffectImageSmaaSearch, true);
    return false;
  }
  // Destroyed after this frame's submission, whether used or not.
  submitter_->DestroyAfter([vulkan_device, buffer, memory]() {
    vulkan_device->functions().vkDestroyBuffer(vulkan_device->device(), buffer, nullptr);
    vulkan_device->functions().vkFreeMemory(vulkan_device->device(), memory, nullptr);
  });
  void* mapping;
  if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    DestroyEffectImage(kEffectImageSmaaArea, true);
    DestroyEffectImage(kEffectImageSmaaSearch, true);
    return false;
  }
  for (uint32_t i = 0; i < 2; ++i) {
    std::memcpy(static_cast<uint8_t*>(mapping) + offsets[i], lookup_textures[i].data,
                size_t(lookup_textures[i].pitch) * lookup_textures[i].height);
  }
  util::FlushMappedMemoryRange(vulkan_device, memory, memory_type, 0, memory_size);
  dfn.vkUnmapMemory(device, memory);
  for (uint32_t i = 0; i < 2; ++i) {
    const LookupTexture& texture = lookup_textures[i];
    VkImage image = effect_images_[texture.index].image;
    ImageBarrier(dfn, command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {};
    region.bufferOffset = offsets[i];
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {texture.width, texture.height, 1};
    command_buffer->CmdVkCopyBufferToImage(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                           &region);
    ImageBarrier(dfn, command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return true;
}

void Present::RecordGammaPass(CommandRecorder* command_buffer, VkRenderPass render_pass,
                              VkFramebuffer framebuffer, VkPipeline pipeline, uint32_t output_width,
                              uint32_t output_height, const Source& source, uint32_t slot,
                              uint32_t pwl, bool luma_in_alpha) {
  // Opaque black; FXAA's source has the luma in .a, black's 0.
  VkClearValue clear = {};
  clear.color.float32[3] = luma_in_alpha ? 0.0f : 1.0f;
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = framebuffer;
  begin_info.renderArea.extent = {output_width, output_height};
  begin_info.clearValueCount = 1;
  begin_info.pClearValues = &clear;
  command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  if (pipeline != VK_NULL_HANDLE) {
    // Pixels past the source stay black (the shaders fetch by pixel index).
    uint32_t width = std::min(output_width, source.width);
    uint32_t height = std::min(output_height, source.height);
    VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {width, height}};
    command_buffer->CmdVkSetViewport(0, 1, &viewport);
    command_buffer->CmdVkSetScissor(0, 1, &scissor);
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[2] = {ramp_sets_[slot][pwl], source_sets_[slot]};
    command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 2,
                                            sets, 0, nullptr);
    command_buffer->CmdVkDraw(3, 1, 0, 0);
  }
  command_buffer->CmdVkEndRenderPass();
}

bool Present::Record(VulkanPresenter::VulkanGuestOutputRefreshContext& context,
                     uint32_t output_width, uint32_t output_height, const Source& source,
                     const reg::DC_LUT_30_COLOR* ramp_table,
                     const reg::DC_LUT_PWL_DATA* ramp_pwl, PostEffect effect) {
  CommandRecorder* command_buffer = submitter_->open_command_buffer();
  if (command_buffer == nullptr || !output_width || !output_height) {
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // The ramp is 10 bits per component, and the effects write more: no 8bpc
  // output.
  context.SetIs8bpc(false);

  bool draw = source.view != VK_NULL_HANDLE && source.width && source.height;
  uint32_t pwl = source.pwl_gamma_ramp ? 1 : 0;
  uint32_t slot = next_slot_;
  if (draw) {
    // The slot's ramp and descriptors are rewritten: its last frame must be done.
    if (slot_submissions_[slot] && !submitter_->Await(slot_submissions_[slot])) {
      return false;
    }
    next_slot_ = (slot + 1) % kSlotCount;
    slot_submissions_[slot] = submitter_->current();

    uint8_t* ramp = ramp_mapping_ + size_t(kRampSlotSize) * slot;
    std::memcpy(ramp, ramp_table, kRampTableSize);
    if constexpr (std::endian::native == std::endian::little) {
      std::memcpy(ramp + kRampTableSize, ramp_pwl, kRampPwlSize);
    } else {
      // R16G16: R (the first 16 bits in memory) is the base the shader expects.
      auto* pwl_out = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(ramp + kRampTableSize);
      for (uint32_t i = 0; i < 128 * 3; ++i) {
        pwl_out[i].base = ramp_pwl[i].delta;
        pwl_out[i].delta = ramp_pwl[i].base;
      }
    }
    util::FlushMappedMemoryRange(vulkan_device, ramp_memory_, ramp_memory_type_,
                                 VkDeviceSize(kRampSlotSize) * slot, ramp_memory_size_,
                                 kRampSlotSize);

    VkDescriptorImageInfo image_info = {};
    image_info.imageView = source.view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = source_sets_[slot];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }

  // swap_post_effect, with what it needs (else the gamma pass alone).
  const bool extreme = effect == PostEffect::kFxaaExtreme;
  bool fxaa = draw && effects_ready_ &&
              (effect == PostEffect::kFxaa || effect == PostEffect::kFxaaExtreme) &&
              fxaa_luma_pipelines_[pwl] != VK_NULL_HANDLE &&
              fxaa_pipelines_[extreme ? 1 : 0] != VK_NULL_HANDLE;
  bool smaa = draw && effects_ready_ && effect == PostEffect::kSmaa;
  for (VkPipeline pipeline : smaa_pipelines_) {
    smaa = smaa && pipeline != VK_NULL_HANDLE;
  }
  if (fxaa) {
    fxaa = EnsureEffectImage(kEffectImageFxaaSource, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             output_width, output_height, fxaa_render_pass_);
  }
  if (smaa) {
    // The formats match the SMAA shaders' image format qualifiers.
    smaa = EnsureEffectImage(kEffectImageSmaaColor, VulkanPresenter::kGuestOutputFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             output_width, output_height, render_pass_) &&
           EnsureEffectImage(kEffectImageSmaaEdges, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             output_width, output_height, VK_NULL_HANDLE) &&
           EnsureEffectImage(kEffectImageSmaaWeights, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             output_width, output_height, VK_NULL_HANDLE) &&
           EnsureSmaaLookupTextures(command_buffer);
  }
  if (draw && effect != PostEffect::kNone && !fxaa && !smaa && !(effects_logged_ & 1)) {
    effects_logged_ |= 1;
    REXGPU_WARN(
        "Native renderer: swap_post_effect could not be applied (its pipelines or images are "
        "missing), the frame is shown without it");
  }

  if (!fxaa && !smaa) {
    VkFramebuffer framebuffer = GetFramebuffer(context, output_width, output_height);
    if (framebuffer == VK_NULL_HANDLE) {
      REXGPU_ERROR("Native renderer: failed to create the present framebuffer");
      return false;
    }
    RecordGammaPass(command_buffer, render_pass_, framebuffer,
                    draw ? pipelines_[pwl] : VK_NULL_HANDLE, output_width, output_height, source,
                    slot, pwl, false);
    return true;
  }

  // The gamma pass into the effect's image (after the last frame's compute
  // reads of it), then its passes write the whole guest output.
  const EffectImage& gamma_image =
      effect_images_[fxaa ? kEffectImageFxaaSource : kEffectImageSmaaColor];
  ImageBarrier(dfn, command_buffer, gamma_image.image, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  RecordGammaPass(command_buffer, fxaa ? fxaa_render_pass_ : render_pass_,
                  gamma_image.framebuffer, fxaa ? fxaa_luma_pipelines_[pwl] : pipelines_[pwl],
                  output_width, output_height, source, slot, pwl, fxaa);
  // Left SHADER_READ_ONLY_OPTIMAL by the render pass; read by compute.
  ImageBarrier(dfn, command_buffer, gamma_image.image, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ImageBarrier(dfn, command_buffer, context.image(), VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

  const EffectConstants constants = {
      {output_width, output_height},
      {1.0f / float(output_width), 1.0f / float(output_height)}};
  const uint32_t group_count_x = (output_width + 15) / 16;
  const uint32_t group_count_y = (output_height + 7) / 8;
  auto image_write = [](VkDescriptorSet set, VkDescriptorType type, uint32_t count,
                        const VkDescriptorImageInfo* infos) {
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.descriptorCount = count;
    write.descriptorType = type;
    write.pImageInfo = infos;
    return write;
  };
  if (fxaa) {
    VkDescriptorImageInfo source_info = {};
    source_info.sampler = linear_clamp_sampler_;
    source_info.imageView = gamma_image.view;
    source_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo destination_info = {};
    destination_info.imageView = context.image_view();
    destination_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2] = {
        image_write(fxaa_source_sets_[slot], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    &source_info),
        image_write(fxaa_destination_sets_[slot], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    &destination_info),
    };
    dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    VkDescriptorSet sets[2] = {fxaa_source_sets_[slot], fxaa_destination_sets_[slot]};
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                      fxaa_pipelines_[extreme ? 1 : 0]);
    command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, fxaa_pipeline_layout_,
                                            0, 2, sets, 0, nullptr);
    command_buffer->CmdVkPushConstants(fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(constants), &constants);
    command_buffer->CmdVkDispatch(group_count_x, group_count_y, 1);
  } else {
    VkImageView color = effect_images_[kEffectImageSmaaColor].view;
    VkImageView edges = effect_images_[kEffectImageSmaaEdges].view;
    VkImageView weights = effect_images_[kEffectImageSmaaWeights].view;
    // As the SDK's Vulkan backend binds them (unused sources a valid image).
    const VkImageView pass_sources[kSmaaPassCount][kSmaaSourceCount] = {
        {color, color, color},
        {edges, effect_images_[kEffectImageSmaaArea].view,
         effect_images_[kEffectImageSmaaSearch].view},
        {color, weights, weights},
    };
    const VkImageView pass_destinations[kSmaaPassCount] = {edges, weights, context.image_view()};
    // The image each pass reads that the pass before wrote.
    const VkImage pass_inputs[kSmaaPassCount] = {VK_NULL_HANDLE,
                                                 effect_images_[kEffectImageSmaaEdges].image,
                                                 effect_images_[kEffectImageSmaaWeights].image};
    VkDescriptorImageInfo infos[kSmaaPassCount * (kSmaaSourceCount + 1)] = {};
    VkWriteDescriptorSet writes[kSmaaPassCount * 2];
    for (uint32_t pass = 0; pass < kSmaaPassCount; ++pass) {
      VkDescriptorImageInfo* source_infos = &infos[pass * (kSmaaSourceCount + 1)];
      for (uint32_t i = 0; i < kSmaaSourceCount; ++i) {
        source_infos[i].imageView = pass_sources[pass][i];
        source_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      VkDescriptorImageInfo& destination_info = source_infos[kSmaaSourceCount];
      destination_info.imageView = pass_destinations[pass];
      destination_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      writes[pass * 2] = image_write(smaa_source_sets_[slot][pass],
                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSmaaSourceCount,
                                     source_infos);
      writes[pass * 2 + 1] = image_write(smaa_destination_sets_[slot][pass],
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &destination_info);
    }
    dfn.vkUpdateDescriptorSets(device, kSmaaPassCount * 2, writes, 0, nullptr);
    // Edges and weights are rewritten whole, after the last frame's reads.
    for (uint32_t pass = 1; pass < kSmaaPassCount; ++pass) {
      ImageBarrier(dfn, command_buffer, pass_inputs[pass], VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, smaa_pipeline_layout_,
                                            0, 1, &smaa_sampler_set_, 0, nullptr);
    command_buffer->CmdVkPushConstants(smaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(constants), &constants);
    for (uint32_t pass = 0; pass < kSmaaPassCount; ++pass) {
      if (pass_inputs[pass] != VK_NULL_HANDLE) {
        ImageBarrier(dfn, command_buffer, pass_inputs[pass], VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
      VkDescriptorSet sets[2] = {smaa_source_sets_[slot][pass],
                                 smaa_destination_sets_[slot][pass]};
      command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, smaa_pipelines_[pass]);
      command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, smaa_pipeline_layout_,
                                              1, 2, sets, 0, nullptr);
      command_buffer->CmdVkDispatch(group_count_x, group_count_y, 1);
    }
  }
  // As the presenter expects it.
  ImageBarrier(dfn, command_buffer, context.image(), VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_GENERAL, VulkanPresenter::kGuestOutputInternalLayout);
  return true;
}

}  // namespace rex::graphics::native
