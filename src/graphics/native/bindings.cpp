/**
 * @file        graphics/native/bindings.cpp
 * @brief       Native renderer: what the translated shaders read (constant buffers, shared
 *              memory, textures), as descriptor sets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The constant buffers follow the SDK's VulkanCommandProcessor::UpdateBindings
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/bindings.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include <rex/chrono/clock.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/util/float_constants.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

// The SDK's (vulkan/command_processor.cpp, in rexgpu-core): constants in
// video memory through the resizable BAR.
REXCVAR_DECLARE(bool, vulkan_upload_rebar);
REXCVAR_DEFINE_BOOL(native_profile_bind, false, "GPU",
                    "Native renderer: sample Bind CPU phases with probability 1/64")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

namespace {

// A single clock read at each boundary; early returns close the current phase.
// Disabled and unsampled calls never read the clock.
class BindPhaseTimer {
 public:
  explicit BindPhaseTimer(Bindings::Stats* stats) : stats_(stats) {
    if (stats_) {
      ++stats_->bind_profile_samples;
      start_ = rex::chrono::Clock::QueryHostTickCount();
    }
  }
  ~BindPhaseTimer() { Next(phase_); }
  void Next(Bindings::BindPhase phase) {
    if (!stats_) return;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    stats_->bind_phase_ticks[size_t(phase_)] += now - start_;
    start_ = now;
    phase_ = phase;
  }
  BindPhaseTimer(const BindPhaseTimer&) = delete;
  BindPhaseTimer& operator=(const BindPhaseTimer&) = delete;

 private:
  Bindings::Stats* stats_;
  Bindings::BindPhase phase_ = Bindings::BindPhase::kUniforms;
  uint64_t start_ = 0;
};

// Sets per transient pool: each has the five constant buffers.
constexpr uint32_t kTransientPoolSets = 256;
constexpr uint32_t kPersistentPoolSets = 256;

// For textures that aren't there (an invalid fetch constant, an unsupported
// format): what the SDK's backends read then.
constexpr VkClearColorValue kPlaceholderColor = {{0.0f, 0.0f, 0.0f, 0.0f}};

}  // namespace

Bindings::~Bindings() {
  Shutdown();
}

bool Bindings::Initialize(Submitter& submitter, Pipelines& pipelines,
                          VkBuffer shared_memory_buffer, uint32_t shared_memory_size) {
  profile_bind_enabled_ = REXCVAR_GET(native_profile_bind);
  profile_bind_random_ = 0x6D2B79F5u;
  submitter_ = &submitter;
  pipelines_ = &pipelines;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t uniform_alignment = size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      rex::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize, size_t(16384)),
                 std::max(uniform_alignment, size_t(1))),
      REXCVAR_GET(vulkan_upload_rebar));
  submitter.AddBeforeSubmit([this]() {
    if (uniform_buffer_pool_) {
      uniform_buffer_pool_->FlushWrites();
    }
  });

  shared_memory_set_ = AllocatePersistentSet(pipelines.shared_memory_set_layout());
  empty_set_ = AllocatePersistentSet(pipelines.empty_set_layout());
  if (shared_memory_set_ == VK_NULL_HANDLE || empty_set_ == VK_NULL_HANDLE) {
    REXGPU_ERROR("Native renderer: failed to allocate the shared memory descriptor set");
    return false;
  }
  // The 512 MB split into equal storage buffers when the device's range is
  // smaller (the translator indexes them the same way).
  uint32_t binding_count = pipelines.shared_memory_binding_count();
  VkDeviceSize binding_range = VkDeviceSize(shared_memory_size / binding_count);
  std::vector<VkDescriptorBufferInfo> buffer_infos(binding_count);
  for (uint32_t i = 0; i < binding_count; ++i) {
    buffer_infos[i].buffer = shared_memory_buffer;
    buffer_infos[i].offset = binding_range * i;
    buffer_infos[i].range = binding_range;
  }
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = shared_memory_set_;
  write.dstBinding = 0;
  write.descriptorCount = binding_count;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = buffer_infos.data();
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

  if (!CreatePlaceholderTextures()) {
    REXGPU_ERROR("Native renderer: failed to create the placeholder textures");
    return false;
  }
  return true;
}

void Bindings::Shutdown() {
  if (!submitter_) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    // Destroying a pool frees its sets.
    for (VkDescriptorPool pool : persistent_pools_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (VkDescriptorPool pool : transient_pools_frame_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (auto& [submission, pool] : transient_pools_retired_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (VkDescriptorPool pool : transient_pools_free_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    util::DestroyAndNullHandle(dfn.vkDestroySampler, device, placeholder_sampler_);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, placeholder_view_3d_);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, placeholder_view_cube_);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, placeholder_view_2d_array_);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, placeholder_3d_);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, placeholder_3d_memory_);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, placeholder_2d_cube_);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, placeholder_2d_cube_memory_);
  }
  persistent_pools_.clear();
  transient_pools_frame_.clear();
  transient_pools_retired_.clear();
  transient_pools_free_.clear();
  for (TextureSetMemo& memo : texture_set_memos_) {
    memo = TextureSetMemo();
  }
  shared_memory_set_ = VK_NULL_HANDLE;
  empty_set_ = VK_NULL_HANDLE;
  constants_set_ = VK_NULL_HANDLE;
  std::memset(constant_buffer_infos_, 0, sizeof(constant_buffer_infos_));
  uniform_buffer_pool_.reset();
  submitter_ = nullptr;
  pipelines_ = nullptr;
}

VkDescriptorPool Bindings::CreatePool(bool transient) {
  VkDescriptorPoolSize sizes[3];
  uint32_t size_count = 0;
  if (transient) {
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[size_count++].descriptorCount =
        kTransientPoolSets * SpirvShaderTranslator::kConstantBufferCount;
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[size_count++].descriptorCount = kTransientPoolSets * 8;
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[size_count++].descriptorCount = kTransientPoolSets * 8;
  } else {
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[size_count++].descriptorCount = 8;
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[size_count++].descriptorCount = kPersistentPoolSets * 16;
    sizes[size_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[size_count++].descriptorCount = kPersistentPoolSets * 16;
  }
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = transient ? kTransientPoolSets : kPersistentPoolSets;
  pool_info.poolSizeCount = size_count;
  pool_info.pPoolSizes = sizes;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkDescriptorPool pool;
  if (vulkan_device->functions().vkCreateDescriptorPool(vulkan_device->device(), &pool_info,
                                                        nullptr, &pool) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a descriptor pool");
    return VK_NULL_HANDLE;
  }
  return pool;
}

VkDescriptorSet Bindings::AllocatePersistentSet(VkDescriptorSetLayout layout) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &layout;
  VkDescriptorSet set;
  if (!persistent_pools_.empty()) {
    allocate_info.descriptorPool = persistent_pools_.back();
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) == VK_SUCCESS) {
      return set;
    }
  }
  // Full: another pool.
  VkDescriptorPool pool = CreatePool(false);
  if (pool == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  persistent_pools_.push_back(pool);
  allocate_info.descriptorPool = pool;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return set;
}

VkDescriptorSet Bindings::AllocateTransientSet(VkDescriptorSetLayout layout) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &layout;
  VkDescriptorSet set;
  if (!transient_pools_frame_.empty()) {
    allocate_info.descriptorPool = transient_pools_frame_.back();
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) == VK_SUCCESS) {
      return set;
    }
  }
  // Another pool: first the pools of frames the GPU has finished (asking for
  // that only here, not for every set).
  uint64_t completed_submission = submitter_->completed();
  while (!transient_pools_retired_.empty() &&
         transient_pools_retired_.front().first <= completed_submission) {
    VkDescriptorPool retired_pool = transient_pools_retired_.front().second;
    transient_pools_retired_.pop_front();
    dfn.vkResetDescriptorPool(device, retired_pool, 0);
    transient_pools_free_.push_back(retired_pool);
  }
  VkDescriptorPool pool;
  if (!transient_pools_free_.empty()) {
    pool = transient_pools_free_.back();
    transient_pools_free_.pop_back();
  } else {
    pool = CreatePool(true);
    if (pool == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  transient_pools_frame_.push_back(pool);
  allocate_info.descriptorPool = pool;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return set;
}

void Bindings::EndFrame(uint64_t last_submission) {
  for (VkDescriptorPool pool : transient_pools_frame_) {
    transient_pools_retired_.emplace_back(last_submission, pool);
  }
  transient_pools_frame_.clear();
  // In a retired pool now.
  constants_set_ = VK_NULL_HANDLE;
  texture_sets_frame_.clear();
  for (TextureSetMemo& memo : texture_set_memos_) {
    memo = TextureSetMemo();
  }
}

bool Bindings::CreatePlaceholderTextures() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {1, 1, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 6;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                            util::MemoryPurpose::kDeviceLocal, placeholder_2d_cube_,
                                            placeholder_2d_cube_memory_)) {
    return false;
  }
  image_info.flags = 0;
  image_info.imageType = VK_IMAGE_TYPE_3D;
  image_info.arrayLayers = 1;
  if (!util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                            util::MemoryPurpose::kDeviceLocal, placeholder_3d_,
                                            placeholder_3d_memory_)) {
    return false;
  }

  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  view_info.image = placeholder_2d_cube_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &placeholder_view_2d_array_) !=
      VK_SUCCESS) {
    return false;
  }
  view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &placeholder_view_cube_) != VK_SUCCESS) {
    return false;
  }
  view_info.image = placeholder_3d_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &placeholder_view_3d_) != VK_SUCCESS) {
    return false;
  }

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &placeholder_sampler_) != VK_SUCCESS) {
    return false;
  }

  // Filled in the first command buffer, before any draw.
  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass();
  if (command_buffer == nullptr) {
    return false;
  }
  VkImageSubresourceRange range = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  for (VkImage image : {placeholder_2d_cube_, placeholder_3d_}) {
    ImageBarrier(dfn, command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    command_buffer->CmdVkClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &kPlaceholderColor, 1, &range);
    ImageBarrier(dfn, command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return true;
}

VkDescriptorSet Bindings::GetTextureSet(const vulkan::VulkanShader& shader, bool vertex,
                                        const std::vector<VkImageView>& views,
                                        const std::vector<VkSampler>& samplers) {
  const std::vector<SpirvShader::TextureBinding>& texture_bindings =
      shader.GetTextureBindingsAfterTranslation();
  const std::vector<SpirvShader::SamplerBinding>& sampler_bindings =
      shader.GetSamplerBindingsAfterTranslation();
  if (texture_bindings.empty() && sampler_bindings.empty()) {
    return empty_set_;
  }
  if (views.size() != texture_bindings.size() || samplers.size() != sampler_bindings.size()) {
    return VK_NULL_HANDLE;
  }
  VkDescriptorSetLayout layout = pipelines_->GetTextureSetLayout(
      vertex, uint32_t(texture_bindings.size()), uint32_t(sampler_bindings.size()));
  if (layout == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  // The same as the stage's last set in this frame: reused.
  TextureSetMemo& memo = texture_set_memos_[vertex ? 0 : 1];
  if (memo.set != VK_NULL_HANDLE && memo.layout == layout && memo.views == views &&
      memo.samplers == samplers) {
    return memo.set;
  }
  // Or one written earlier in the frame with the same contents.
  XXH3_state_t hash_state;
  XXH3_64bits_reset(&hash_state);
  XXH3_64bits_update(&hash_state, &layout, sizeof(layout));
  XXH3_64bits_update(&hash_state, views.data(), sizeof(VkImageView) * views.size());
  XXH3_64bits_update(&hash_state, samplers.data(), sizeof(VkSampler) * samplers.size());
  uint64_t hash = XXH3_64bits_digest(&hash_state);
  auto [cached_begin, cached_end] = texture_sets_frame_.equal_range(hash);
  for (auto it = cached_begin; it != cached_end; ++it) {
    const TextureSetMemo& cached = it->second;
    if (cached.layout == layout && cached.views == views && cached.samplers == samplers) {
      memo = cached;
      ++stats_.texture_sets_found;
      return cached.set;
    }
  }
  VkDescriptorSet set = AllocateTransientSet(layout);
  if (set == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  VkWriteDescriptorSet writes[2];
  uint32_t write_count = WriteTextureInfos(shader, views, samplers, set, writes);
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  vulkan_device->functions().vkUpdateDescriptorSets(vulkan_device->device(), write_count, writes,
                                                    0, nullptr);
  memo.layout = layout;
  memo.views = views;
  memo.samplers = samplers;
  memo.set = set;
  texture_sets_frame_.emplace(hash, memo);
  ++stats_.texture_sets_written;
  return set;
}

uint32_t Bindings::WriteTextureInfos(const vulkan::VulkanShader& shader,
                                     const std::vector<VkImageView>& views,
                                     const std::vector<VkSampler>& samplers,
                                     VkDescriptorSet dst_set, VkWriteDescriptorSet writes_out[2]) {
  const std::vector<SpirvShader::TextureBinding>& texture_bindings =
      shader.GetTextureBindingsAfterTranslation();
  const std::vector<SpirvShader::SamplerBinding>& sampler_bindings =
      shader.GetSamplerBindingsAfterTranslation();
  image_infos_.clear();
  image_infos_.resize(texture_bindings.size() + sampler_bindings.size());
  for (size_t i = 0; i < texture_bindings.size(); ++i) {
    VkDescriptorImageInfo& image_info = image_infos_[i];
    image_info.sampler = VK_NULL_HANDLE;
    image_info.imageView = views[i];
    if (image_info.imageView == VK_NULL_HANDLE) {
      switch (texture_bindings[i].dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          image_info.imageView = placeholder_view_3d_;
          break;
        case xenos::FetchOpDimension::kCube:
          image_info.imageView = placeholder_view_cube_;
          break;
        default:
          // 1D and 2D are 2D arrays in the translated shaders.
          image_info.imageView = placeholder_view_2d_array_;
          break;
      }
    }
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  for (size_t i = 0; i < sampler_bindings.size(); ++i) {
    VkDescriptorImageInfo& sampler_info = image_infos_[texture_bindings.size() + i];
    sampler_info.sampler = samplers[i] != VK_NULL_HANDLE ? samplers[i] : placeholder_sampler_;
    sampler_info.imageView = VK_NULL_HANDLE;
    sampler_info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  uint32_t write_count = 0;
  if (!texture_bindings.empty()) {
    VkWriteDescriptorSet& write = writes_out[write_count++];
    write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = 0;
    write.descriptorCount = uint32_t(texture_bindings.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = image_infos_.data();
  }
  if (!sampler_bindings.empty()) {
    VkWriteDescriptorSet& write = writes_out[write_count++];
    write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = uint32_t(texture_bindings.size());
    write.descriptorCount = uint32_t(sampler_bindings.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = image_infos_.data() + texture_bindings.size();
  }
  return write_count;
}

void Bindings::ConstantRegisterWritten(uint32_t index) {
  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    uint32_t float_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    // Only if the buffer holding the stage's constants has this one.
    const uint64_t* map = current_float_map_vertex_;
    uint32_t buffer = SpirvShaderTranslator::kConstantBufferFloatVertex;
    if (float_index >= 256) {
      float_index -= 256;
      map = current_float_map_pixel_;
      buffer = SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    if (map[float_index >> 6] & (uint64_t(1) << (float_index & 63))) {
      constants_up_to_date_ &= ~(uint32_t(1) << buffer);
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    constants_up_to_date_ &= ~(uint32_t(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    constants_up_to_date_ &= ~(uint32_t(1) << SpirvShaderTranslator::kConstantBufferFetch);
  }
}

void Bindings::ConstantRegistersWritten(uint32_t first, uint32_t last) {
  if (first > last) {
    return;
  }
  if (first < XE_GPU_REG_SHADER_CONSTANT_000_X || first > XE_GPU_REG_SHADER_CONSTANT_511_W) {
    // Fetch, or bool and loop: one buffer each, whichever register it is.
    ConstantRegisterWritten(first);
    return;
  }
  // Float constants: a stage's buffer only if its map has one of the range's.
  auto any_in_map = [](const uint64_t* map, uint32_t first_constant, uint32_t last_constant) {
    for (uint32_t word = first_constant >> 6; word <= (last_constant >> 6); ++word) {
      uint64_t mask = UINT64_MAX;
      if (word == (first_constant >> 6)) {
        mask &= UINT64_MAX << (first_constant & 63);
      }
      if (word == (last_constant >> 6)) {
        mask &= UINT64_MAX >> (63 - (last_constant & 63));
      }
      if (map[word] & mask) {
        return true;
      }
    }
    return false;
  };
  uint32_t first_constant = (first - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
  uint32_t last_constant = (last - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
  if (first_constant < 256 &&
      any_in_map(current_float_map_vertex_, first_constant, std::min(last_constant, 255u))) {
    constants_up_to_date_ &= ~(uint32_t(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
  }
  if (last_constant >= 256 &&
      any_in_map(current_float_map_pixel_, first_constant >= 256 ? first_constant - 256 : 0,
                 last_constant - 256)) {
    constants_up_to_date_ &= ~(uint32_t(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
  }
}

bool Bindings::Bind(CommandRecorder* command_buffer, const Pipelines::Layout& layout,
                    const RegisterFile& regs,
                    const SpirvShaderTranslator::SystemConstants& system_constants,
                    const vulkan::VulkanShader& vertex_shader,
                    const vulkan::VulkanShader* pixel_shader, const DrawTextures& textures) {
  bool sampled = false;
  if (profile_bind_enabled_) {
    ++stats_.bind_profile_calls;
    // Xorshift32 avoids locking the sample to a repeating 64-draw sequence.
    // Do not reset at frame/report boundaries.
    profile_bind_random_ ^= profile_bind_random_ << 13;
    profile_bind_random_ ^= profile_bind_random_ >> 17;
    profile_bind_random_ ^= profile_bind_random_ << 5;
    sampled = (profile_bind_random_ & 63u) == 0;
  }
  BindPhaseTimer timer(sampled ? &stats_ : nullptr);
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  size_t alignment =
      std::max(size_t(vulkan_device->properties().minUniformBufferOffsetAlignment), size_t(1));

  uint64_t submission = submitter_->current();
  if (submission != constants_submission_) {
    // The buffers of an earlier submission may be reclaimed once it is done.
    uniform_buffer_pool_->Reclaim(submitter_->completed());
    constants_submission_ = submission;
    constants_up_to_date_ = 0;
  }
  VkDescriptorBufferInfo* infos = current_infos_;
  uint32_t* offsets = current_offsets_;
  auto request = [&](uint32_t index, size_t size) -> uint8_t* {
    ++stats_.uniform_requests;
    stats_.uniform_bytes += size;
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = uniform_buffer_pool_->Request(submission, size, alignment, buffer, offset);
    if (!mapping) {
      return nullptr;
    }
    infos[index].buffer = buffer;
    infos[index].offset = 0;
    infos[index].range = VkDeviceSize(size);
    offsets[index] = uint32_t(offset);
    return mapping;
  };
  auto up_to_date = [&](uint32_t index) {
    return (constants_up_to_date_ & (uint32_t(1) << index)) != 0;
  };
  auto written = [&](uint32_t index) { constants_up_to_date_ |= uint32_t(1) << index; };

  uint8_t* mapping;
  if (!up_to_date(SpirvShaderTranslator::kConstantBufferSystem) ||
      std::memcmp(&current_system_constants_, &system_constants,
                  sizeof(SpirvShaderTranslator::SystemConstants))) {
    mapping = request(SpirvShaderTranslator::kConstantBufferSystem,
                      sizeof(SpirvShaderTranslator::SystemConstants));
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &system_constants, sizeof(SpirvShaderTranslator::SystemConstants));
    std::memcpy(&current_system_constants_, &system_constants,
                sizeof(SpirvShaderTranslator::SystemConstants));
    written(SpirvShaderTranslator::kConstantBufferSystem);
  }

  // The float constants each shader reads, packed in register order. A buffer
  // is bound even without any. Written again for another map, or when a
  // constant of the map was written (ConstantRegisterWritten).
  constexpr size_t kFloat4 = sizeof(float) * 4;
  static const uint64_t kNoFloats[4] = {};
  const uint64_t* vertex_bitmap = vertex_shader.constant_register_map().float_bitmap;
  uint32_t vertex_float_count = vertex_shader.constant_register_map().float_count;
  if (!vertex_float_count) {
    vertex_bitmap = kNoFloats;
  }
  if (!up_to_date(SpirvShaderTranslator::kConstantBufferFloatVertex) ||
      std::memcmp(current_float_map_vertex_, vertex_bitmap, sizeof(current_float_map_vertex_))) {
    mapping = request(SpirvShaderTranslator::kConstantBufferFloatVertex,
                      kFloat4 * std::max(vertex_float_count, uint32_t(1)));
    if (!mapping) {
      return false;
    }
    if (vertex_float_count) {
      CopyFloatConstantRuns(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X], vertex_bitmap);
    } else {
      std::memset(mapping, 0, kFloat4);
    }
    std::memcpy(current_float_map_vertex_, vertex_bitmap, sizeof(current_float_map_vertex_));
    written(SpirvShaderTranslator::kConstantBufferFloatVertex);
  }
  uint32_t pixel_float_count = pixel_shader ? pixel_shader->constant_register_map().float_count : 0;
  const uint64_t* pixel_bitmap =
      pixel_float_count ? pixel_shader->constant_register_map().float_bitmap : kNoFloats;
  if (!up_to_date(SpirvShaderTranslator::kConstantBufferFloatPixel) ||
      std::memcmp(current_float_map_pixel_, pixel_bitmap, sizeof(current_float_map_pixel_))) {
    mapping = request(SpirvShaderTranslator::kConstantBufferFloatPixel,
                      kFloat4 * std::max(pixel_float_count, uint32_t(1)));
    if (!mapping) {
      return false;
    }
    if (pixel_float_count) {
      CopyFloatConstantRuns(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_256_X], pixel_bitmap);
    } else {
      std::memset(mapping, 0, kFloat4);
    }
    std::memcpy(current_float_map_pixel_, pixel_bitmap, sizeof(current_float_map_pixel_));
    written(SpirvShaderTranslator::kConstantBufferFloatPixel);
  }

  // Bool (8 dwords) and loop (32) constants, then the 32 fetch constants of 6
  // dwords: as they are in the registers.
  constexpr size_t kBoolLoopSize = sizeof(uint32_t) * (8 + 32);
  if (!up_to_date(SpirvShaderTranslator::kConstantBufferBoolLoop)) {
    mapping = request(SpirvShaderTranslator::kConstantBufferBoolLoop, kBoolLoopSize);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopSize);
    written(SpirvShaderTranslator::kConstantBufferBoolLoop);
  }
  constexpr size_t kFetchSize = sizeof(uint32_t) * 6 * 32;
  if (!up_to_date(SpirvShaderTranslator::kConstantBufferFetch)) {
    mapping = request(SpirvShaderTranslator::kConstantBufferFetch, kFetchSize);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchSize);
    written(SpirvShaderTranslator::kConstantBufferFetch);
  }

  timer.Next(BindPhase::kConstants);
  // A new set only when a buffer or a size changed: the offsets are dynamic.
  bool rewrite = constants_set_ == VK_NULL_HANDLE;
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    rewrite |= infos[i].buffer != constant_buffer_infos_[i].buffer ||
               infos[i].range != constant_buffer_infos_[i].range;
  }
  if (rewrite) {
    ++stats_.constants_sets_written;
    timer.Next(BindPhase::kConstantAllocate);
    VkDescriptorSet set = AllocateTransientSet(pipelines_->constants_set_layout());
    timer.Next(BindPhase::kConstants);
    if (set == VK_NULL_HANDLE) {
      REXGPU_ERROR("Native renderer: failed to allocate a constants descriptor set");
      return false;
    }
    // One write per binding: their stages differ.
    VkWriteDescriptorSet writes[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[i].pBufferInfo = &infos[i];
    }
    timer.Next(BindPhase::kConstantUpdate);
    dfn.vkUpdateDescriptorSets(device, SpirvShaderTranslator::kConstantBufferCount, writes, 0,
                               nullptr);
    timer.Next(BindPhase::kConstants);
    std::memcpy(constant_buffer_infos_, infos, sizeof(constant_buffer_infos_));
    constants_set_ = set;
  }

  timer.Next(BindPhase::kTextureSets);
  // A push layout has textures, so a pixel shader.
  const bool pixel_pushed = layout.textures_pixel_pushed && pixel_shader;
  VkDescriptorSet vertex_textures =
      GetTextureSet(vertex_shader, true, textures.vertex_views, textures.vertex_samplers);
  VkDescriptorSet pixel_textures = empty_set_;
  if (pixel_shader && !pixel_pushed) {
    pixel_textures =
        GetTextureSet(*pixel_shader, false, textures.pixel_views, textures.pixel_samplers);
  }
  if (vertex_textures == VK_NULL_HANDLE || pixel_textures == VK_NULL_HANDLE) {
    REXGPU_ERROR("Native renderer: failed to get a texture descriptor set");
    return false;
  }

  timer.Next(BindPhase::kBindPrepare);
  static_assert(SpirvShaderTranslator::kDescriptorSetTexturesPixel + 1 ==
                    SpirvShaderTranslator::kDescriptorSetCount,
                "a pushed pixel texture set is the last, left out of the bound range");
  VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_memory_set_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_set_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = vertex_textures;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      pixel_pushed ? VK_NULL_HANDLE : pixel_textures;
  const uint32_t set_count = pixel_pushed ? SpirvShaderTranslator::kDescriptorSetTexturesPixel
                                          : SpirvShaderTranslator::kDescriptorSetCount;
  // From the first set that differs from the bound ones (all with another
  // pipeline layout); the dynamic offsets only when the constants' set is in
  // the range.
  uint32_t first_set = 0;
  if (bound_.valid && bound_.layout == layout.pipeline_layout) {
    first_set = set_count;
    for (uint32_t i = 0; i < set_count; ++i) {
      bool differs = sets[i] != bound_.sets[i];
      if (i == SpirvShaderTranslator::kDescriptorSetConstants) {
        differs |= std::memcmp(offsets, bound_.offsets, sizeof(bound_.offsets)) != 0;
      }
      if (differs) {
        first_set = i;
        break;
      }
    }
  }
  if (first_set < set_count) {
    ++stats_.descriptor_binds;
    static_assert(SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram <
                      SpirvShaderTranslator::kDescriptorSetConstants,
                  "the dynamic offsets follow the sets bound");
    bool with_constants = first_set <= SpirvShaderTranslator::kDescriptorSetConstants;
    timer.Next(BindPhase::kBindApi);
    command_buffer->CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline_layout, first_set, set_count - first_set,
        sets + first_set, with_constants ? SpirvShaderTranslator::kConstantBufferCount : 0,
        with_constants ? offsets : nullptr);
    timer.Next(BindPhase::kBindPrepare);
    bound_.valid = true;
    bound_.layout = layout.pipeline_layout;
    std::memcpy(bound_.sets, sets, sizeof(bound_.sets));
    std::memcpy(bound_.offsets, offsets, sizeof(bound_.offsets));
    // A bound pixel set, or the lower ones under another layout, replace
    // what was pushed.
    if (!pixel_pushed || pushed_pixel_.layout != layout.pipeline_layout) {
      pushed_pixel_.valid = false;
    }
  }

  timer.Next(BindPhase::kPushPrepare);
  // The pixel shader's textures pushed where they differ from the pushed ones
  // (the views and samplers compared as the texture set memo does).
  if (pixel_pushed &&
      (!pushed_pixel_.valid || pushed_pixel_.layout != layout.pipeline_layout ||
       pushed_pixel_.views != textures.pixel_views ||
       pushed_pixel_.samplers != textures.pixel_samplers)) {
    if (textures.pixel_views.size() !=
            pixel_shader->GetTextureBindingsAfterTranslation().size() ||
        textures.pixel_samplers.size() !=
            pixel_shader->GetSamplerBindingsAfterTranslation().size()) {
      REXGPU_ERROR("Native renderer: the pixel shader's textures don't match its bindings");
      return false;
    }
    if (!pushed_pixel_.valid) ++stats_.pushes_invalid;
    else if (pushed_pixel_.layout != layout.pipeline_layout) ++stats_.pushes_layout;
    else ++stats_.pushes_content;
    VkWriteDescriptorSet writes[2];
    uint32_t write_count = WriteTextureInfos(*pixel_shader, textures.pixel_views,
                                             textures.pixel_samplers, VK_NULL_HANDLE, writes);
    timer.Next(BindPhase::kPushApi);
    command_buffer->CmdVkPushDescriptorSetKHR(
        VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline_layout,
        SpirvShaderTranslator::kDescriptorSetTexturesPixel, write_count, writes);
    timer.Next(BindPhase::kPushPrepare);
    pushed_pixel_.valid = true;
    pushed_pixel_.layout = layout.pipeline_layout;
    pushed_pixel_.views = textures.pixel_views;
    pushed_pixel_.samplers = textures.pixel_samplers;
    ++stats_.texture_sets_pushed;
  }
  return true;
}

}  // namespace rex::graphics::native
