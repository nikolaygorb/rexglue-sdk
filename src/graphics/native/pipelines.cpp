/**
 * @file        graphics/native/pipelines.cpp
 * @brief       Native renderer: descriptor set layouts, draw render passes and pipelines
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The layouts are the SDK's SPIR-V translator's; pipeline creation follows the
 *              SDK's VulkanPipelineCache::EnsurePipelineCreated for host render targets
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/pipelines.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/graphics/vulkan/tessellation_shaders.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(native_push_descriptors, true, "GPU",
                    "Native renderer: the pixel shader's textures and samplers are pushed into "
                    "the command buffer (VK_KHR_push_descriptor) instead of written into a "
                    "descriptor set for each new combination; off: descriptor sets. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

namespace {

uint64_t HashRenderPassKey(const Pipelines::RenderPassKey& key) {
  return XXH3_64bits(&key, sizeof(key));
}

}  // namespace

static_assert(sizeof(Pipelines::RenderPassKey) ==
                  sizeof(VkFormat) * (xenos::kMaxColorRenderTargets + 1) +
                      sizeof(VkSampleCountFlagBits),
              "RenderPassKey is compared and hashed as memory");

size_t Pipelines::KeyHasher::operator()(const PipelineKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

bool Pipelines::KeyEqual::operator()(const PipelineKey& a, const PipelineKey& b) const {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

Pipelines::~Pipelines() {
  Shutdown();
}

bool Pipelines::Initialize(Submitter& submitter) {
  submitter_ = &submitter;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& properties = vulkan_device->properties();

  caps_.fill_mode_non_solid = properties.fillModeNonSolid;
  caps_.depth_clamp = properties.depthClamp;
  caps_.constant_alpha_color_blend_factors = properties.constantAlphaColorBlendFactors;
  caps_.separate_stencil_mask_ref = properties.separateStencilMaskRef;
  // The guest's vertex shader also runs as the tessellation evaluation
  // shader, after the host's vertex and control shaders (which read the
  // system constants), where the device has tessellation.
  tessellation_supported_ = properties.tessellationShader;
  guest_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  guest_stages_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  if (tessellation_supported_) {
    guest_vertex_stages_ |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    guest_stages_ |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
#if REX_PLATFORM_MAC
  caps_.strip_restart_always = true;
#endif
  push_descriptors_ = REXCVAR_GET(native_push_descriptors) &&
                      vulkan_device->extensions().ext_KHR_push_descriptor &&
                      dfn.vkCmdPushDescriptorSetKHR != nullptr;
  max_push_descriptors_ = push_descriptors_ ? properties.maxPushDescriptors : 0;

  // As the translator splits the 512 MB (its Features read the same limit).
  shared_memory_binding_count_ =
      uint32_t(1) << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          properties.maxStorageBufferRange);

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &empty_set_layout_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create the empty descriptor set layout");
    return false;
  }

  VkDescriptorSetLayoutBinding shared_memory_binding = {};
  shared_memory_binding.binding = 0;
  shared_memory_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_binding.descriptorCount = shared_memory_binding_count_;
  shared_memory_binding.stageFlags = guest_stages_;
  VkDescriptorSetLayoutBinding shared_memory_bindings[2] = {shared_memory_binding, {}};
  if (motion_capture_binding_) {
    // Object motion: the vertex shaders' position capture.
    shared_memory_bindings[1].binding = SpirvShaderTranslator::kMotionCaptureBinding;
    shared_memory_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_bindings[1].descriptorCount = 1;
    shared_memory_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  layout_info.bindingCount = motion_capture_binding_ ? 2 : 1;
  layout_info.pBindings = shared_memory_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &shared_memory_set_layout_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create the shared memory descriptor set layout");
    return false;
  }

  std::array<VkDescriptorSetLayoutBinding, SpirvShaderTranslator::kConstantBufferCount>
      constant_bindings = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages_;
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
      guest_vertex_stages_;
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  layout_info.bindingCount = uint32_t(constant_bindings.size());
  layout_info.pBindings = constant_bindings.data();
  if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &constants_set_layout_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create the constants descriptor set layout");
    return false;
  }

  VkPipelineCacheCreateInfo pipeline_cache_info = {};
  pipeline_cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  if (dfn.vkCreatePipelineCache(device, &pipeline_cache_info, nullptr, &pipeline_cache_) !=
      VK_SUCCESS) {
    // Pipelines are made without one.
    pipeline_cache_ = VK_NULL_HANDLE;
  }

  // Background creation: 3/4 of the logical processors (as the SDK's D3D11
  // backend), at least 1, at most 8.
  uint32_t hardware_threads = std::max(std::thread::hardware_concurrency(), 1u);
  uint32_t worker_count = std::clamp(hardware_threads * 3 / 4, 1u, 8u);
  workers_exit_ = false;
  for (uint32_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this]() { WorkerMain(); });
  }
  return true;
}

void Pipelines::WorkerMain() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      jobs_cv_.wait(lock, [this]() { return workers_exit_ || !jobs_.empty(); });
      if (workers_exit_) {
        return;
      }
      job = jobs_.front();
      jobs_.pop_front();
      ++jobs_running_;
    }
    VkPipeline pipeline = CreatePipeline(job.key, job.render_pass_key);
    job.entry->pipeline.store(pipeline, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(done_mutex_);
      job.entry->done.store(true, std::memory_order_release);
    }
    done_cv_.notify_all();
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      --jobs_running_;
    }
    idle_cv_.notify_all();
  }
}

void Pipelines::AwaitIdle() {
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  idle_cv_.wait(lock, [this]() { return workers_.empty() || (jobs_.empty() && !jobs_running_); });
}

bool Pipelines::IsIdle() {
  std::lock_guard<std::mutex> lock(jobs_mutex_);
  return jobs_.empty() && !jobs_running_;
}

bool Pipelines::GetPipelineCacheData(std::vector<uint8_t>& data_out) {
  data_out.clear();
  if (!submitter_ || pipeline_cache_ == VK_NULL_HANDLE) {
    return false;
  }
  AwaitIdle();
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  size_t size = 0;
  if (dfn.vkGetPipelineCacheData(vulkan_device->device(), pipeline_cache_, &size, nullptr) !=
      VK_SUCCESS) {
    return false;
  }
  data_out.resize(size);
  if (size && dfn.vkGetPipelineCacheData(vulkan_device->device(), pipeline_cache_, &size,
                                         data_out.data()) != VK_SUCCESS) {
    data_out.clear();
    return false;
  }
  data_out.resize(size);
  return true;
}

void Pipelines::ReplacePipelineCache(const std::vector<uint8_t>& data) {
  if (!submitter_) {
    return;
  }
  // No worker may be creating with the old one.
  AwaitIdle();
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkPipelineCacheCreateInfo cache_info = {};
  cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  cache_info.initialDataSize = data.size();
  cache_info.pInitialData = data.empty() ? nullptr : data.data();
  VkPipelineCache new_cache = VK_NULL_HANDLE;
  if (dfn.vkCreatePipelineCache(device, &cache_info, nullptr, &new_cache) != VK_SUCCESS) {
    // Data from another driver or GPU: the cache it would have warmed starts
    // empty.
    if (data.empty()) {
      return;
    }
    REXGPU_WARN("Native renderer: the stored pipeline cache was not accepted, starting empty");
    cache_info.initialDataSize = 0;
    cache_info.pInitialData = nullptr;
    if (dfn.vkCreatePipelineCache(device, &cache_info, nullptr, &new_cache) != VK_SUCCESS) {
      return;
    }
  }
  // Pipelines made with the old one don't refer to it.
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineCache, device, pipeline_cache_);
  pipeline_cache_ = new_cache;
}

void Pipelines::StopWorkers() {
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    workers_exit_ = true;
    // Not started: their entries stay without a pipeline.
    jobs_.clear();
  }
  jobs_cv_.notify_all();
  for (std::thread& worker : workers_) {
    worker.join();
  }
  workers_.clear();
}

void Pipelines::Shutdown() {
  if (!submitter_) {
    return;
  }
  // Before anything they use is destroyed.
  StopWorkers();
  jobs_running_ = 0;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (auto& [key, entry] : pipelines_) {
      VkPipeline pipeline = entry->pipeline.load(std::memory_order_acquire);
      if (pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline, nullptr);
      }
    }
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineCache, device, pipeline_cache_);
    for (VkShaderModule& shader_module : tessellation_vertex_shaders_) {
      util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, shader_module);
    }
    for (auto& [key, shader_module] : tessellation_control_shaders_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, shader_module, nullptr);
      }
    }
    for (auto& [hash, render_pass] : render_passes_) {
      dfn.vkDestroyRenderPass(device, render_pass.second, nullptr);
    }
    for (auto& [key, layout] : layouts_) {
      dfn.vkDestroyPipelineLayout(device, layout->pipeline_layout, nullptr);
    }
    for (auto& [key, set_layout] : texture_set_layouts_) {
      dfn.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, constants_set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                               shared_memory_set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, empty_set_layout_);
  }
  pipelines_.clear();
  tessellation_vertex_shader_attempted_[0] = false;
  tessellation_vertex_shader_attempted_[1] = false;
  tessellation_control_shaders_.clear();
  render_passes_.clear();
  layouts_.clear();
  texture_set_layouts_.clear();
  submitter_ = nullptr;
}

VkDescriptorSetLayout Pipelines::GetTextureSetLayout(bool vertex, uint32_t texture_count,
                                                     uint32_t sampler_count) {
  if (!texture_count && !sampler_count) {
    return empty_set_layout_;
  }
  uint64_t key = uint64_t(texture_count & 0xFFFF) | (uint64_t(sampler_count & 0xFFFF) << 16) |
                 (uint64_t(vertex) << 32);
  auto it = texture_set_layouts_.find(key);
  if (it != texture_set_layouts_.end()) {
    return it->second;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkShaderStageFlags stage = vertex ? guest_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  // Images first, then the samplers (the translator's binding order).
  std::vector<VkDescriptorSetLayoutBinding> bindings(texture_count + sampler_count);
  for (uint32_t i = 0; i < texture_count + sampler_count; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i < texture_count ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = stage;
    bindings[i].pImmutableSamplers = nullptr;
  }
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (!vertex && PixelTexturesPushed(texture_count, sampler_count)) {
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  }
  layout_info.bindingCount = uint32_t(bindings.size());
  layout_info.pBindings = bindings.data();
  VkDescriptorSetLayout set_layout;
  if (vulkan_device->functions().vkCreateDescriptorSetLayout(vulkan_device->device(), &layout_info,
                                                             nullptr, &set_layout) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a descriptor set layout for {} textures and "
                 "{} samplers",
                 texture_count, sampler_count);
    return VK_NULL_HANDLE;
  }
  texture_set_layouts_.emplace(key, set_layout);
  return set_layout;
}

const Pipelines::Layout* Pipelines::GetLayout(uint32_t vertex_textures, uint32_t vertex_samplers,
                                              uint32_t pixel_textures,
                                              uint32_t pixel_samplers) {
  uint64_t key = uint64_t(vertex_textures & 0xFFFF) | (uint64_t(vertex_samplers & 0xFFFF) << 16) |
                 (uint64_t(pixel_textures & 0xFFFF) << 32) |
                 (uint64_t(pixel_samplers & 0xFFFF) << 48);
  auto it = layouts_.find(key);
  if (it != layouts_.end()) {
    return it->second.get();
  }
  auto layout = std::make_unique<Layout>();
  layout->textures_vertex = GetTextureSetLayout(true, vertex_textures, vertex_samplers);
  layout->textures_pixel = GetTextureSetLayout(false, pixel_textures, pixel_samplers);
  layout->textures_pixel_pushed = PixelTexturesPushed(pixel_textures, pixel_samplers);
  if (layout->textures_vertex == VK_NULL_HANDLE || layout->textures_pixel == VK_NULL_HANDLE) {
    return nullptr;
  }
  VkDescriptorSetLayout set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  set_layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_memory_set_layout_;
  set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] = constants_set_layout_;
  set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = layout->textures_vertex;
  set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = layout->textures_pixel;
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  pipeline_layout_info.pSetLayouts = set_layouts;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device->functions().vkCreatePipelineLayout(vulkan_device->device(),
                                                        &pipeline_layout_info, nullptr,
                                                        &layout->pipeline_layout) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a pipeline layout");
    return nullptr;
  }
  const Layout* result = layout.get();
  layouts_.emplace(key, std::move(layout));
  return result;
}

VkRenderPass Pipelines::GetRenderPass(const RenderPassKey& key) {
  uint64_t hash = HashRenderPassKey(key);
  auto range = render_passes_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second.first, &key, sizeof(key))) {
      return it->second.second;
    }
  }

  // Loaded and stored in the attachment layouts: layout changes are barriers
  // outside (RenderTargets::Transition). The dependencies order these
  // attachment accesses after and before those of the passes, clears and
  // copies around this one.
  VkAttachmentDescription attachments[xenos::kMaxColorRenderTargets + 1] = {};
  VkAttachmentReference color_references[xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t color_reference_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (key.color[i] == VK_FORMAT_UNDEFINED) {
      color_references[i].attachment = VK_ATTACHMENT_UNUSED;
      color_references[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
      continue;
    }
    VkAttachmentDescription& attachment = attachments[attachment_count];
    attachment.format = key.color[i];
    attachment.samples = key.samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_references[i].attachment = attachment_count++;
    color_references[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_reference_count = i + 1;
  }
  VkAttachmentReference depth_reference = {};
  bool depth = key.depth != VK_FORMAT_UNDEFINED;
  if (depth) {
    VkAttachmentDescription& attachment = attachments[attachment_count];
    attachment.format = key.depth;
    attachment.samples = key.samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_reference.attachment = attachment_count++;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  // No attachments: draws only exporting to memory (single-sampled).

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = color_reference_count;
  subpass.pColorAttachments = color_reference_count ? color_references : nullptr;
  subpass.pDepthStencilAttachment = depth ? &depth_reference : nullptr;

  constexpr VkPipelineStageFlags kAttachmentStages =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  constexpr VkAccessFlags kAttachmentWrites =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  constexpr VkAccessFlags kAttachmentAccesses =
      kAttachmentWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = kAttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependencies[0].dstStageMask = kAttachmentStages;
  dependencies[0].srcAccessMask = kAttachmentWrites | VK_ACCESS_TRANSFER_WRITE_BIT;
  dependencies[0].dstAccessMask = kAttachmentAccesses;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = kAttachmentStages;
  dependencies[1].dstStageMask = kAttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].srcAccessMask = kAttachmentWrites;
  dependencies[1].dstAccessMask = kAttachmentAccesses | VK_ACCESS_TRANSFER_READ_BIT |
                                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = attachment_count;
  render_pass_info.pAttachments = attachments;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  render_pass_info.dependencyCount = 2;
  render_pass_info.pDependencies = dependencies;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkRenderPass render_pass;
  if (vulkan_device->functions().vkCreateRenderPass(vulkan_device->device(), &render_pass_info,
                                                    nullptr, &render_pass) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a draw render pass");
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(hash, std::make_pair(key, render_pass));
  return render_pass;
}

VkPipeline Pipelines::GetPipeline(VkShaderModule vertex_shader, VkShaderModule pixel_shader,
                                  VkShaderModule tessellation_vertex_shader,
                                  VkShaderModule tessellation_control_shader,
                                  const RenderPassKey& render_pass_key, const Layout& layout,
                                  const PipelineState& state, bool allow_async, uint32_t wait_ms,
                                  bool& pending_out, bool* requested_first_out) {
  pending_out = false;
  if (requested_first_out) {
    *requested_first_out = false;
  }
  VkRenderPass render_pass = GetRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_shader = vertex_shader;
  key.pixel_shader = pixel_shader;
  key.tessellation_vertex_shader = tessellation_vertex_shader;
  key.tessellation_control_shader = tessellation_control_shader;
  key.render_pass = render_pass;
  key.pipeline_layout = layout.pipeline_layout;
  key.state = state;
  Entry* entry;
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    entry = it->second.get();
  } else {
    // A failure is kept too: not tried again for every draw.
    if (requested_first_out) {
      *requested_first_out = true;
    }
    auto new_entry = std::make_unique<Entry>();
    entry = new_entry.get();
    pipelines_.emplace(key, std::move(new_entry));
    if (allow_async && !workers_.empty()) {
      {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.push_back(Job{key, render_pass_key, entry});
      }
      jobs_cv_.notify_one();
    } else {
      entry->pipeline.store(CreatePipeline(key, render_pass_key), std::memory_order_relaxed);
      entry->done.store(true, std::memory_order_release);
    }
  }
  if (!entry->done.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(done_mutex_);
    auto done = [entry]() { return entry->done.load(std::memory_order_acquire); };
    if (!allow_async) {
      // Needed now (being created in the background since an earlier draw).
      done_cv_.wait(lock, done);
    } else if (!wait_ms || !done_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), done)) {
      pending_out = true;
      return VK_NULL_HANDLE;
    }
  }
  return entry->pipeline.load(std::memory_order_relaxed);
}

VkShaderModule Pipelines::GetTessellationVertexShader(bool adaptive) {
  uint32_t index = adaptive ? 1 : 0;
  if (!tessellation_supported_ || tessellation_vertex_shader_attempted_[index]) {
    return tessellation_vertex_shaders_[index];
  }
  tessellation_vertex_shader_attempted_[index] = true;
  std::string source = adaptive ? vulkan::GetTessellationAdaptiveVertexShaderGlsl()
                                : vulkan::GetTessellationIndexedVertexShaderGlsl();
  std::vector<uint32_t> spirv;
  std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_VERTEX_BIT, source, spirv, error)) {
    REXGPU_ERROR("Native renderer: failed to compile the {} tessellation vertex shader: {}",
                 adaptive ? "adaptive" : "indexed", error);
    return VK_NULL_HANDLE;
  }
  tessellation_vertex_shaders_[index] = util::CreateShaderModule(
      submitter_->vulkan_device(), spirv.data(), sizeof(uint32_t) * spirv.size());
  return tessellation_vertex_shaders_[index];
}

VkShaderModule Pipelines::GetTessellationControlShader(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode) {
  if (!tessellation_supported_) {
    return VK_NULL_HANDLE;
  }
  uint32_t key = (uint32_t(host_vertex_shader_type) << 2) | uint32_t(tessellation_mode);
  auto it = tessellation_control_shaders_.find(key);
  if (it != tessellation_control_shaders_.end()) {
    return it->second;
  }
  // A failure is kept too.
  VkShaderModule& shader_module = tessellation_control_shaders_[key];
  shader_module = VK_NULL_HANDLE;
  uint32_t input_control_points =
      vulkan::TessellationInputControlPoints(host_vertex_shader_type, tessellation_mode);
  uint32_t output_control_points =
      vulkan::TessellationOutputControlPoints(host_vertex_shader_type, tessellation_mode);
  if (!input_control_points || !output_control_points) {
    return VK_NULL_HANDLE;
  }
  std::string source = vulkan::GetTessellationControlShaderGlsl(
      host_vertex_shader_type, tessellation_mode, input_control_points, output_control_points);
  std::vector<uint32_t> spirv;
  std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, source, spirv,
                                  error)) {
    REXGPU_ERROR("Native renderer: failed to compile a tessellation control shader ({:X}): {}",
                 key, error);
    return VK_NULL_HANDLE;
  }
  shader_module = util::CreateShaderModule(submitter_->vulkan_device(), spirv.data(),
                                           sizeof(uint32_t) * spirv.size());
  return shader_module;
}

VkPipeline Pipelines::CreatePipeline(const PipelineKey& key,
                                     const RenderPassKey& render_pass_key) const {
  const PipelineState& state = key.state;

  VkPipelineShaderStageCreateInfo stages[4] = {};
  uint32_t stage_count = 0;
  auto add_stage = [&](VkShaderStageFlagBits stage, VkShaderModule shader_module) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = stage;
    stages[stage_count].module = shader_module;
    stages[stage_count].pName = "main";
    ++stage_count;
  };
  // Tessellated: the host's vertex and control shaders, then the guest's
  // vertex shader as the evaluation shader.
  const bool tessellated = key.tessellation_vertex_shader != VK_NULL_HANDLE;
  if (tessellated) {
    add_stage(VK_SHADER_STAGE_VERTEX_BIT, key.tessellation_vertex_shader);
    add_stage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, key.tessellation_control_shader);
    add_stage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, key.vertex_shader);
  } else {
    add_stage(VK_SHADER_STAGE_VERTEX_BIT, key.vertex_shader);
  }
  // Depth-only draws have no fragment shader: the write masks are 0.
  if (key.pixel_shader != VK_NULL_HANDLE) {
    add_stage(VK_SHADER_STAGE_FRAGMENT_BIT, key.pixel_shader);
  }

  // Vertices are fetched by the shaders from the shared memory.
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VkPrimitiveTopology(state.topology);
  input_assembly.primitiveRestartEnable = state.primitive_restart ? VK_TRUE : VK_FALSE;

  VkPipelineTessellationStateCreateInfo tessellation = {};
  tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  tessellation.patchControlPoints = state.patch_control_points;

  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.depthClampEnable = state.depth_clamp ? VK_TRUE : VK_FALSE;
  rasterization.rasterizerDiscardEnable = state.rasterizer_discard ? VK_TRUE : VK_FALSE;
  rasterization.polygonMode = VkPolygonMode(state.polygon_mode);
  rasterization.cullMode = VkCullModeFlags(state.cull_mode);
  rasterization.frontFace = VkFrontFace(state.front_face);
  rasterization.depthBiasEnable = state.depth_bias ? VK_TRUE : VK_FALSE;
  rasterization.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = render_pass_key.samples;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = state.depth_test ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = state.depth_write ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = VkCompareOp(state.depth_compare);
  depth_stencil.stencilTestEnable = state.stencil_test ? VK_TRUE : VK_FALSE;
  depth_stencil.front.failOp = VkStencilOp(state.stencil_front[0]);
  depth_stencil.front.passOp = VkStencilOp(state.stencil_front[1]);
  depth_stencil.front.depthFailOp = VkStencilOp(state.stencil_front[2]);
  depth_stencil.front.compareOp = VkCompareOp(state.stencil_front[3]);
  depth_stencil.back.failOp = VkStencilOp(state.stencil_back[0]);
  depth_stencil.back.passOp = VkStencilOp(state.stencil_back[1]);
  depth_stencil.back.depthFailOp = VkStencilOp(state.stencil_back[2]);
  depth_stencil.back.compareOp = VkCompareOp(state.stencil_back[3]);

  // One blend state per colour attachment slot of the subpass, holes
  // included.
  VkPipelineColorBlendAttachmentState blend_attachments[xenos::kMaxColorRenderTargets] = {};
  uint32_t blend_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (render_pass_key.color[i] == VK_FORMAT_UNDEFINED) {
      continue;
    }
    const PipelineState::ColorTarget& target = state.color[i];
    VkPipelineColorBlendAttachmentState& blend = blend_attachments[i];
    blend.blendEnable = target.blend ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = VkBlendFactor(target.src_color);
    blend.dstColorBlendFactor = VkBlendFactor(target.dst_color);
    blend.colorBlendOp = VkBlendOp(target.color_op);
    blend.srcAlphaBlendFactor = VkBlendFactor(target.src_alpha);
    blend.dstAlphaBlendFactor = VkBlendFactor(target.dst_alpha);
    blend.alphaBlendOp = VkBlendOp(target.alpha_op);
    blend.colorWriteMask = key.pixel_shader != VK_NULL_HANDLE ? target.write_mask : 0;
    blend_attachment_count = i + 1;
  }
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend.attachmentCount = blend_attachment_count;
  color_blend.pAttachments = blend_attachment_count ? blend_attachments : nullptr;

  // All of it dynamic, even where a pipeline doesn't use it: binding a
  // pipeline without a state dynamic would invalidate what was set.
  static const VkDynamicState kDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(std::size(kDynamicStates));
  dynamic.pDynamicStates = kDynamicStates;

  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = stage_count;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pTessellationState = tessellated ? &tessellation : nullptr;
  pipeline_info.pViewportState = &viewport;
  pipeline_info.pRasterizationState = &rasterization;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = key.pipeline_layout;
  pipeline_info.renderPass = key.render_pass;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vulkan_device->functions().vkCreateGraphicsPipelines(
      vulkan_device->device(), pipeline_cache_, 1, &pipeline_info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a graphics pipeline ({}, topology {})",
                 int32_t(result), uint32_t(state.topology));
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

}  // namespace rex::graphics::native
