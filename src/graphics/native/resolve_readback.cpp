/**
 * @file        graphics/native/resolve_readback.cpp
 * @brief       Native renderer: resolves copied back to the CPU (readback_resolve)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanCommandProcessor's resolve readback (Xenia, Copyright 2022 Ben
 *              Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/resolve_readback.h>

#include <algorithm>
#include <utility>

#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

namespace {
// Each range from an offset aligned for any texel size and for
// nonCoherentAtomSize in practice.
constexpr VkDeviceSize kRangeAlignment = 256;
// A deferred copy's buffer grows in these steps.
constexpr VkDeviceSize kDeferredAlignment = VkDeviceSize(64) << 10;
// Frames a destination's deferred copy is kept without a resolve to it.
constexpr uint64_t kDeferredKeepFrames = 120;
}  // namespace

ResolveReadback::~ResolveReadback() {
  Shutdown();
}

void ResolveReadback::Initialize(Submitter& submitter, NativeSharedMemory& shared_memory) {
  submitter_ = &submitter;
  shared_memory_ = &shared_memory;
}

void ResolveReadback::Shutdown() {
  if (submitter_) {
    DestroyBuffer();
    // The caller has waited for the GPU.
    for (auto& [key, copy] : deferred_) {
      DestroyDeferredBuffer(copy, true);
    }
  }
  deferred_.clear();
  submitter_ = nullptr;
  shared_memory_ = nullptr;
}

void ResolveReadback::DestroyBuffer() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (mapping_) {
    dfn.vkUnmapMemory(device, memory_);
    mapping_ = nullptr;
  }
  util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, memory_);
  size_ = 0;
}

bool ResolveReadback::EnsureBuffer(VkDeviceSize size) {
  if (buffer_ != VK_NULL_HANDLE && size_ >= size) {
    return true;
  }
  // The last ReadNow waited for its copies: nothing in flight uses it.
  DestroyBuffer();
  // Grown in steps of 1 MB, not one resolve at a time.
  VkDeviceSize new_size = rex::align(size, VkDeviceSize(1) << 20);
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!util::CreateDedicatedAllocationBuffer(vulkan_device, new_size,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             util::MemoryPurpose::kReadback, buffer_, memory_)) {
    REXGPU_ERROR("Native renderer: failed to create a {} MB resolve readback buffer",
                 new_size >> 20);
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    return false;
  }
  if (vulkan_device->functions().vkMapMemory(vulkan_device->device(), memory_, 0, VK_WHOLE_SIZE,
                                             0, &mapping_) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to map the resolve readback buffer");
    mapping_ = nullptr;
    DestroyBuffer();
    return false;
  }
  size_ = new_size;
  return true;
}

void ResolveReadback::ReadNow(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                              std::vector<const uint8_t*>& data_out) {
  data_out.assign(count, nullptr);
  if (!submitter_ || !count) {
    return;
  }
  // Where each range goes in the buffer; UINT64_MAX for one outside the
  // shared memory.
  offsets_.assign(count, UINT64_MAX);
  VkDeviceSize total = 0;
  for (size_t i = 0; i < count; ++i) {
    auto [address, length] = ranges[i];
    address &= 0x1FFFFFFF;
    if (!length || length > SharedMemory::kBufferSize - address) {
      continue;
    }
    offsets_[i] = total;
    total += rex::align(VkDeviceSize(length), kRangeAlignment);
  }
  if (!total || !EnsureBuffer(total)) {
    return;
  }

  // After the resolves' compute writes into the shared memory.
  shared_memory_->Use(NativeSharedMemory::Usage::kRead);
  CommandRecorder* command_buffer =
      submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve, GpuKind::kReadback);
  if (command_buffer == nullptr) {
    return;
  }
  regions_.clear();
  for (size_t i = 0; i < count; ++i) {
    if (offsets_[i] == UINT64_MAX) {
      continue;
    }
    VkBufferCopy& region = regions_.emplace_back();
    region.srcOffset = VkDeviceSize(ranges[i].first & 0x1FFFFFFF);
    region.dstOffset = offsets_[i];
    region.size = VkDeviceSize(ranges[i].second);
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  command_buffer->CmdVkCopyBuffer(shared_memory_->buffer(), buffer_, uint32_t(regions_.size()),
                                  regions_.data());
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  // Submitted and waited for here.
  if (!submitter_->Await(submitter_->current())) {
    return;
  }
  VkMappedMemoryRange memory_range = {};
  memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  memory_range.memory = memory_;
  memory_range.offset = 0;
  memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
  const uint8_t* mapping = static_cast<const uint8_t*>(mapping_);
  for (size_t i = 0; i < count; ++i) {
    if (offsets_[i] != UINT64_MAX) {
      data_out[i] = mapping + offsets_[i];
    }
  }
}

bool ResolveReadback::CreateDeferredBuffer(DeferredCopy& copy, VkDeviceSize size) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!util::CreateDedicatedAllocationBuffer(vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             util::MemoryPurpose::kReadback, copy.buffer,
                                             copy.memory)) {
    copy.buffer = VK_NULL_HANDLE;
    copy.memory = VK_NULL_HANDLE;
    return false;
  }
  if (vulkan_device->functions().vkMapMemory(vulkan_device->device(), copy.memory, 0,
                                             VK_WHOLE_SIZE, 0, &copy.mapping) != VK_SUCCESS) {
    copy.mapping = nullptr;
    DestroyDeferredBuffer(copy, true);
    return false;
  }
  copy.size = size;
  return true;
}

void ResolveReadback::DestroyDeferredBuffer(DeferredCopy& copy, bool now) {
  VkBuffer buffer = copy.buffer;
  VkDeviceMemory memory = copy.memory;
  bool mapped = copy.mapping != nullptr;
  copy = DeferredCopy();
  if (buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  auto destroy = [vulkan_device, buffer, memory, mapped]() {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (mapped) {
      dfn.vkUnmapMemory(device, memory);
    }
    if (buffer != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, buffer, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, memory, nullptr);
    }
  };
  if (now) {
    destroy();
  } else {
    submitter_->DestroyAfter(std::move(destroy));
  }
}

bool ResolveReadback::RecordDeferred(uint32_t address, uint32_t length) {
  address &= 0x1FFFFFFF;
  if (!submitter_ || !length || length > SharedMemory::kBufferSize - address) {
    return false;
  }
  DeferredCopy& copy = deferred_[DeferredKey(address, length)];
  if (copy.buffer == VK_NULL_HANDLE || copy.size < length) {
    // A copy into the old one may still be queued.
    DestroyDeferredBuffer(copy, false);
    if (!CreateDeferredBuffer(copy, rex::align(VkDeviceSize(length), kDeferredAlignment))) {
      REXGPU_ERROR("Native renderer: failed to create a deferred resolve readback buffer");
      deferred_.erase(DeferredKey(address, length));
      return false;
    }
  }
  // After the resolves' compute writes into the shared memory.
  shared_memory_->Use(NativeSharedMemory::Usage::kRead);
  CommandRecorder* command_buffer =
      submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve, GpuKind::kReadback);
  if (command_buffer == nullptr) {
    return false;
  }
  VkBufferCopy region = {};
  region.srcOffset = VkDeviceSize(address);
  region.dstOffset = 0;
  region.size = VkDeviceSize(length);
  command_buffer->CmdVkCopyBuffer(shared_memory_->buffer(), copy.buffer, 1, &region);
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  copy.submission = submitter_->current();
  copy.last_frame = frame_;
  return true;
}

void ResolveReadback::CompleteDeferred(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                                       std::vector<const uint8_t*>& data_out) {
  data_out.assign(count, nullptr);
  if (!submitter_ || !count) {
    return;
  }
  // The latest of the copies, waited for once (a submission's fence covers
  // everything submitted before it).
  uint64_t await_submission = 0;
  for (size_t i = 0; i < count; ++i) {
    auto it = deferred_.find(DeferredKey(ranges[i].first, ranges[i].second));
    if (it != deferred_.end() && it->second.mapping && it->second.submission) {
      await_submission = std::max(await_submission, it->second.submission);
    }
  }
  if (!await_submission || !submitter_->Await(await_submission)) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (size_t i = 0; i < count; ++i) {
    auto it = deferred_.find(DeferredKey(ranges[i].first, ranges[i].second));
    if (it == deferred_.end() || !it->second.mapping || !it->second.submission) {
      continue;
    }
    VkMappedMemoryRange memory_range = {};
    memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memory_range.memory = it->second.memory;
    memory_range.offset = 0;
    memory_range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
    data_out[i] = static_cast<const uint8_t*>(it->second.mapping);
  }
}

void ResolveReadback::EndFrame(uint64_t frame) {
  frame_ = frame;
  if (deferred_.empty() || frame < kDeferredKeepFrames) {
    return;
  }
  for (auto it = deferred_.begin(); it != deferred_.end();) {
    if (it->second.last_frame + kDeferredKeepFrames < frame) {
      DestroyDeferredBuffer(it->second, false);
      it = deferred_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace rex::graphics::native
