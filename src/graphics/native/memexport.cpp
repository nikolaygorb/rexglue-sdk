/**
 * @file        graphics/native/memexport.cpp
 * @brief       Native renderer: memory export written back to the CPU (readback_memexport)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanCommandProcessor's memexport readback (Xenia, Copyright 2022 Ben
 *              Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/memexport.h>

#include <cstring>

#include <rex/graphics/native/resolve_readback.h>
#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

namespace {
// Streams whose fast buffers are freed after this many frames unused.
constexpr uint64_t kStreamUnusedFrames = 120;
}  // namespace

MemexportReadback::~MemexportReadback() {
  Shutdown();
}

void MemexportReadback::Initialize(Submitter& submitter, NativeSharedMemory& shared_memory,
                                   ResolveReadback& readback, memory::Memory& memory) {
  submitter_ = &submitter;
  shared_memory_ = &shared_memory;
  readback_ = &readback;
  memory_ = &memory;
}

void MemexportReadback::Shutdown() {
  // The caller has waited for the GPU.
  for (auto& [key, stream] : streams_) {
    for (Slot& slot : stream.slots) {
      DestroySlotNow(slot);
    }
  }
  streams_.clear();
  snapshot_.clear();
  snapshot_valid_ = false;
  submitter_ = nullptr;
  shared_memory_ = nullptr;
  readback_ = nullptr;
  memory_ = nullptr;
}

void MemexportReadback::TakeSnapshot(const std::vector<draw_util::MemExportRange>& ranges) {
  snapshot_valid_ = false;
  if (!readback_ || ranges.empty()) {
    return;
  }
  ranges_scratch_.clear();
  uint32_t total_size = 0;
  for (const draw_util::MemExportRange& range : ranges) {
    ranges_scratch_.emplace_back(range.base_address_dwords << 2, range.size_bytes);
    total_size += range.size_bytes;
  }
  readback_->ReadNow(ranges_scratch_.data(), ranges_scratch_.size(), data_scratch_);
  snapshot_.resize(total_size);
  uint32_t offset = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (!data_scratch_[i]) {
      return;
    }
    std::memcpy(snapshot_.data() + offset, data_scratch_[i], ranges[i].size_bytes);
    offset += ranges[i].size_bytes;
  }
  snapshot_valid_ = true;
}

void MemexportReadback::ReadBack(const std::vector<draw_util::MemExportRange>& ranges, bool fast,
                                 uint64_t frame) {
  if (!submitter_ || ranges.empty()) {
    return;
  }
  uint32_t total_size = 0;
  for (const draw_util::MemExportRange& range : ranges) {
    total_size += range.size_bytes;
  }
  if (!total_size) {
    return;
  }
  if (fast) {
    ReadBackFast(ranges, total_size, frame);
  } else {
    ReadBackFull(ranges);
  }
}

void MemexportReadback::ReadBackFull(const std::vector<draw_util::MemExportRange>& ranges) {
  ranges_scratch_.clear();
  for (const draw_util::MemExportRange& range : ranges) {
    ranges_scratch_.emplace_back(range.base_address_dwords << 2, range.size_bytes);
  }
  readback_->ReadNow(ranges_scratch_.data(), ranges_scratch_.size(), data_scratch_);
  const uint8_t* before = snapshot_valid_ ? snapshot_.data() : nullptr;
  for (size_t i = 0; i < ranges.size(); ++i) {
    const draw_util::MemExportRange& range = ranges[i];
    uint8_t* guest = memory_->TranslatePhysical(range.base_address_dwords << 2);
    if (data_scratch_[i] && guest) {
      if (before) {
        draw_util::WriteBackChangedBytes(guest, before, data_scratch_[i], range.size_bytes);
      } else {
        std::memcpy(guest, data_scratch_[i], range.size_bytes);
      }
    }
    if (before) {
      before += range.size_bytes;
    }
  }
  snapshot_valid_ = false;
}

void MemexportReadback::ReadBackFast(const std::vector<draw_util::MemExportRange>& ranges,
                                     uint32_t total_size, uint64_t frame) {
  // One stream per first address and total size, as the SDK's backend keys it.
  uint64_t key = (uint64_t(ranges.front().base_address_dwords) << 32) | total_size;
  Stream& stream = streams_[key];
  stream.last_used_frame = frame;
  uint32_t write_index = stream.current;
  uint32_t read_index = 1 - write_index;
  Slot& write_slot = stream.slots[write_index];
  if (!EnsureSlot(write_slot, total_size)) {
    ReadBackFull(ranges);
    return;
  }

  // After the draw's shader writes.
  shared_memory_->Use(NativeSharedMemory::Usage::kRead);
  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kMemexport);
  if (command_buffer == nullptr) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDeviceSize offset = 0;
  for (const draw_util::MemExportRange& range : ranges) {
    VkBufferCopy region = {};
    region.srcOffset = VkDeviceSize(range.base_address_dwords) << 2;
    region.dstOffset = offset;
    region.size = range.size_bytes;
    command_buffer->CmdVkCopyBuffer(shared_memory_->buffer(), write_slot.buffer, 1, &region);
    offset += range.size_bytes;
  }
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  write_slot.submission = submitter_->current();
  write_slot.written_size = total_size;

  // The other buffer, if an earlier draw of the stream filled it and the GPU
  // has finished it; else this one, waited for.
  const Slot& read_slot = stream.slots[read_index];
  const Slot* source = &read_slot;
  if (read_slot.buffer == VK_NULL_HANDLE || read_slot.written_size < total_size ||
      !read_slot.submission || read_slot.submission > submitter_->completed()) {
    if (!submitter_->Await(write_slot.submission)) {
      REXGPU_ERROR("Native renderer: failed to wait for a memory export readback");
      stream.current = read_index;
      return;
    }
    source = &write_slot;
  }
  VkMappedMemoryRange memory_range = {};
  memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  memory_range.memory = source->memory;
  memory_range.offset = 0;
  memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(vulkan_device->device(), 1, &memory_range);
  WriteToGuest(ranges, static_cast<const uint8_t*>(source->mapping));
  stream.current = read_index;
}

void MemexportReadback::WriteToGuest(const std::vector<draw_util::MemExportRange>& ranges,
                                     const uint8_t* data) {
  for (const draw_util::MemExportRange& range : ranges) {
    uint8_t* guest = memory_->TranslatePhysical(range.base_address_dwords << 2);
    if (guest) {
      std::memcpy(guest, data, range.size_bytes);
    }
    data += range.size_bytes;
  }
}

bool MemexportReadback::EnsureSlot(Slot& slot, uint32_t size) {
  if (slot.buffer != VK_NULL_HANDLE && slot.size >= size) {
    return true;
  }
  DestroySlotLater(slot);
  // Grown in steps of 64 KB.
  VkDeviceSize new_size = rex::align(VkDeviceSize(size), VkDeviceSize(1) << 16);
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!util::CreateDedicatedAllocationBuffer(vulkan_device, new_size,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             util::MemoryPurpose::kReadback, slot.buffer,
                                             slot.memory)) {
    REXGPU_ERROR("Native renderer: failed to create a memory export readback buffer");
    slot = Slot();
    return false;
  }
  if (vulkan_device->functions().vkMapMemory(vulkan_device->device(), slot.memory, 0,
                                             VK_WHOLE_SIZE, 0, &slot.mapping) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to map a memory export readback buffer");
    slot.mapping = nullptr;
    DestroySlotNow(slot);
    return false;
  }
  slot.size = new_size;
  slot.submission = 0;
  slot.written_size = 0;
  return true;
}

void MemexportReadback::DestroySlotLater(Slot& slot) {
  if (slot.buffer == VK_NULL_HANDLE && slot.memory == VK_NULL_HANDLE) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkBuffer buffer = slot.buffer;
  VkDeviceMemory memory = slot.memory;
  bool mapped = slot.mapping != nullptr;
  submitter_->DestroyAfter([vulkan_device, buffer, memory, mapped]() {
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
  });
  slot = Slot();
}

void MemexportReadback::DestroySlotNow(Slot& slot) {
  if (!submitter_ || (slot.buffer == VK_NULL_HANDLE && slot.memory == VK_NULL_HANDLE)) {
    slot = Slot();
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (slot.mapping) {
    dfn.vkUnmapMemory(device, slot.memory);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, slot.buffer);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, slot.memory);
  slot = Slot();
}

void MemexportReadback::EndFrame(uint64_t frame) {
  for (auto it = streams_.begin(); it != streams_.end();) {
    if (frame - it->second.last_used_frame <= kStreamUnusedFrames) {
      ++it;
      continue;
    }
    for (Slot& slot : it->second.slots) {
      DestroySlotLater(slot);
    }
    it = streams_.erase(it);
  }
}

}  // namespace rex::graphics::native
