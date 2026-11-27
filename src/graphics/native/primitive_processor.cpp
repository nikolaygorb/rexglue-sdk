/**
 * @file        graphics/native/primitive_processor.cpp
 * @brief       Native renderer: guest primitives and index buffers for the host
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Modelled on the SDK's VulkanPrimitiveProcessor (Xenia, Copyright 2021 Ben
 *              Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/primitive_processor.h>

#include <algorithm>

#include <rex/cvar.h>
#include <rex/graphics/native/submission.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

// The SDK's (vulkan/command_processor.cpp, in rexgpu-core): converted indices
// in video memory through the resizable BAR.
REXCVAR_DECLARE(bool, vulkan_upload_rebar);

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

NativePrimitiveProcessor::~NativePrimitiveProcessor() {
  Shutdown(true);
}

bool NativePrimitiveProcessor::Initialize(Submitter& submitter) {
  submitter_ = &submitter;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  // No conversion is left to geometry shaders (see the class comment).
  if (!InitializeCommon(vulkan_device->properties().fullDrawIndexUint32, false, false, false,
                        false, false)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      std::max(size_t(kMinRequiredConvertedIndexBufferSize),
               ui::GraphicsUploadBufferPool::kDefaultPageSize),
      REXCVAR_GET(vulkan_upload_rebar));
  submitter.AddBeforeSubmit([this]() {
    if (frame_index_buffer_pool_) {
      frame_index_buffer_pool_->FlushWrites();
    }
  });
  return true;
}

void NativePrimitiveProcessor::Shutdown(bool from_destructor) {
  frame_index_buffers_.clear();
  frame_index_buffer_pool_.reset();
  if (submitter_) {
    // The caller has waited for the GPU.
    const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
    if (vulkan_device) {
      const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
      const VkDevice device = vulkan_device->device();
      util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_upload_);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_upload_memory_);
      util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    }
    submitter_ = nullptr;
    if (!from_destructor) {
      ShutdownCommon();
    }
  }
}

void NativePrimitiveProcessor::BeginFrame(uint64_t frame, uint64_t completed_frame) {
  frame_ = frame;
  if (frame_index_buffer_pool_) {
    frame_index_buffer_pool_->Reclaim(completed_frame);
  }
}

void NativePrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

void NativePrimitiveProcessor::UploadBuiltinIndexBufferIfNeeded() {
  if (builtin_index_buffer_uploaded_ || builtin_index_buffer_upload_ == VK_NULL_HANDLE) {
    return;
  }
  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kUpload);
  if (command_buffer == nullptr) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkBufferCopy copy_region = {};
  copy_region.size = builtin_index_buffer_size_;
  command_buffer->CmdVkCopyBuffer(builtin_index_buffer_upload_, builtin_index_buffer_, 1,
                                  &copy_region);
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = builtin_index_buffer_;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1,
                                       &barrier, 0, nullptr);
  builtin_index_buffer_uploaded_ = true;
  // Not needed once this submission is done.
  VkBuffer upload_buffer = builtin_index_buffer_upload_;
  VkDeviceMemory upload_memory = builtin_index_buffer_upload_memory_;
  builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  submitter_->DestroyAfter([vulkan_device, upload_buffer, upload_memory]() {
    vulkan_device->functions().vkDestroyBuffer(vulkan_device->device(), upload_buffer, nullptr);
    vulkan_device->functions().vkFreeMemory(vulkan_device->device(), upload_memory, nullptr);
  });
}

bool NativePrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  builtin_index_buffer_size_ = VkDeviceSize(size_bytes);
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          util::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    REXGPU_ERROR("Native renderer: failed to create the {}-byte built-in index buffer",
                 size_bytes);
    return false;
  }
  uint32_t upload_memory_type;
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          util::MemoryPurpose::kUpload, builtin_index_buffer_upload_,
          builtin_index_buffer_upload_memory_, &upload_memory_type)) {
    REXGPU_ERROR("Native renderer: failed to create the {}-byte built-in index upload buffer",
                 size_bytes);
    return false;
  }
  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapping) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to map the built-in index upload buffer");
    return false;
  }
  fill_callback(mapping);
  util::FlushMappedMemoryRange(vulkan_device, builtin_index_buffer_upload_memory_,
                               upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);
  builtin_index_buffer_uploaded_ = false;
  return true;
}

void* NativePrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  VkBuffer buffer;
  VkDeviceSize offset;
  // By frame, not by submission: the base caches converted buffers for the
  // frame, across submissions.
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      frame_,
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = VkDeviceSize(offset + coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(buffer, offset);
  return mapping;
}

}  // namespace rex::graphics::native
