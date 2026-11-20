/**
 * @file        graphics/native/shared_memory.cpp
 * @brief       Native renderer: the guest physical memory on the GPU
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Modelled on the SDK's VulkanSharedMemory (Xenia, Copyright 2020 Ben Vanik,
 *              BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/shared_memory.h>

#include <algorithm>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native/submission.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

namespace util = ui::vulkan::util;

NativeSharedMemory::~NativeSharedMemory() {
  Shutdown(true);
}

bool NativeSharedMemory::Initialize(Submitter& submitter) {
  submitter_ = &submitter;
  InitializeCommon();
  EnableHotPages();
  EnableUploadHashing();

  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device, kBufferSize,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          util::MemoryPurpose::kDeviceLocal, buffer_, buffer_memory_)) {
    REXGPU_ERROR("Native renderer: failed to create the {} MB shared memory buffer",
                 kBufferSize >> 20);
    Shutdown();
    return false;
  }
  REXGPU_INFO("Native renderer: shared memory is a full {} MB buffer (not sparse yet)",
              kBufferSize >> 20);

  // The first usage is an upload.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = {0, 0};

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                 size_t(1) << page_size_log2()));
  submitter.AddBeforeSubmit([this]() {
    if (upload_buffer_pool_) {
      upload_buffer_pool_->FlushWrites();
    }
  });
  size_t read_words = (size_t(kBufferSize) >> page_size_log2()) / 64;
  render_pass_read_pages_.assign(read_words, 0);
  // No render pass has serial 0... until one does: none has begun yet.
  render_pass_read_serials_.assign(read_words, UINT64_MAX);
  return true;
}

void NativeSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();
  upload_regions_.clear();
  if (submitter_) {
    // The caller has waited for the GPU.
    const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
    if (vulkan_device) {
      const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
      const VkDevice device = vulkan_device->device();
      util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, buffer_memory_);
    }
    submitter_ = nullptr;
    // From the destructor, ~SharedMemory calls it.
    if (!from_destructor) {
      ShutdownCommon();
    }
  }
}

void NativeSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) {
  if (usage == Usage::kTransferDestination) {
    stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
    return;
  }
  if (usage == Usage::kComputeWrite) {
    stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    return;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
               kGuestShaderStages;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  if (usage == Usage::kRead) {
    stage_mask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
  } else {
    access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
  }
}

void NativeSharedMemory::RecordBarrier(CommandRecorder* command_buffer, Usage from, Usage to,
                                       VkDeviceSize offset, VkDeviceSize size) const {
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  GetUsageMasks(from, src_stage_mask, barrier.srcAccessMask);
  GetUsageMasks(to, dst_stage_mask, barrier.dstAccessMask);
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer_;
  barrier.offset = offset;
  barrier.size = size;
  command_buffer->CmdVkPipelineBarrier(src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1, &barrier,
                                       0, nullptr);
}

void NativeSharedMemory::MarkReadInRenderPass(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize || render_pass_read_pages_.empty()) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = (start + length - 1) >> page_size_log2();
  uint64_t serial = submitter_->render_pass_serial();
  for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
    uint64_t mask = UINT64_MAX;
    if (word == (page_first >> 6)) {
      mask &= UINT64_MAX << (page_first & 63);
    }
    if (word == (page_last >> 6) && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (render_pass_read_serials_[word] != serial) {
      render_pass_read_serials_[word] = serial;
      render_pass_read_pages_[word] = 0;
    }
    render_pass_read_pages_[word] |= mask;
  }
}

bool NativeSharedMemory::IsReadInRenderPass(uint32_t page_first, uint32_t page_last) const {
  uint64_t serial = submitter_->render_pass_serial();
  for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
    if (render_pass_read_serials_[word] != serial) {
      continue;
    }
    uint64_t mask = UINT64_MAX;
    if (word == (page_first >> 6)) {
      mask &= UINT64_MAX << (page_first & 63);
    }
    if (word == (page_last >> 6) && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (render_pass_read_pages_[word] & mask) {
      return true;
    }
  }
  return false;
}

void NativeSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  if (last_usage_ != usage || last_written_range_.second) {
    CommandRecorder* command_buffer =
        submitter_->OpenOutsideRenderPass(RenderPassEnd::kMemoryBarrier);
    if (command_buffer != nullptr) {
      VkBufferMemoryBarrier barrier = {};
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      VkPipelineStageFlags src_stage_mask, dst_stage_mask;
      GetUsageMasks(last_usage_, src_stage_mask, barrier.srcAccessMask);
      GetUsageMasks(usage, dst_stage_mask, barrier.dstAccessMask);
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = buffer_;
      if (last_usage_ == usage) {
        // Only the previous writes, made visible to the same usage.
        barrier.offset = last_written_range_.first;
        barrier.size = last_written_range_.second;
      } else {
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
      }
      command_buffer->CmdVkPipelineBarrier(src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1,
                                           &barrier, 0, nullptr);
    }
    last_usage_ = usage;
  }
  last_written_range_ = written_range;
}

bool NativeSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  // The ranges are sorted: the first and the last bound what is written.
  std::pair<uint32_t, uint32_t> written_range =
      std::make_pair(upload_page_ranges.front().first << page_size_log2(),
                     (upload_page_ranges.back().first + upload_page_ranges.back().second -
                      upload_page_ranges.front().first)
                         << page_size_log2());
  // Before the open render pass (native_hoist_uploads) when its draws only
  // read (no memory export written yet) and none read these pages yet: the
  // render pass stays open. The barriers are only around the copy then.
  bool before_render_pass = false;
  if (last_usage_ == Usage::kRead && !last_written_range_.second &&
      submitter_->can_record_before_render_pass()) {
    before_render_pass = true;
    for (const std::pair<uint32_t, uint32_t>& range : upload_page_ranges) {
      if (range.second && IsReadInRenderPass(range.first, range.first + range.second - 1)) {
        before_render_pass = false;
        break;
      }
    }
  }
  CommandRecorder* command_buffer;
  if (before_render_pass) {
    // Recorded at the render pass's hoist point until EndBeforeRenderPass.
    command_buffer = submitter_->BeginBeforeRenderPass(GpuKind::kUploads);
    RecordBarrier(command_buffer, Usage::kRead, Usage::kTransferDestination, written_range.first,
                  written_range.second);
    submitter_->CountUploadBeforeRenderPass();
  } else {
    Use(Usage::kTransferDestination, written_range);
    command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kUpload);
    if (command_buffer == nullptr) {
      return false;
    }
  }
  upload_buffer_pool_->Reclaim(submitter_->completed());
  uint64_t submission = submitter_->current();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    if (upload_range_length && !REXCVAR_GET(gpu_allow_invalid_upload_range)) {
      // Pages the guest can't access (an invalid fetch constant, for instance).
      uint32_t range_start_address = upload_range_start << page_size_log2();
      uint32_t range_last_address = (upload_range_start + upload_range_length - 1)
                                    << page_size_log2();
      if (memory().GetPhysicalHeap()->QueryRangeAccess(range_start_address, range_start_address) ==
              rex::memory::PageAccess::kNoAccess ||
          memory().GetPhysicalHeap()->QueryRangeAccess(range_last_address, range_last_address) ==
              rex::memory::PageAccess::kNoAccess) {
        REXGPU_ERROR("Native renderer: invalid shared memory upload range {:08X} length {:08X}",
                     upload_range_start, upload_range_length);
        successful = false;
        break;
      }
    }
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission, size_t(upload_range_length) << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (!upload_buffer_mapping) {
        REXGPU_ERROR("Native renderer: failed to get a shared memory upload buffer");
        successful = false;
        break;
      }
      // Valid before the copy: a write during it invalidates the pages again.
      MakeUploadRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size));
      CopyPagesForUpload(upload_buffer_mapping, upload_range_start,
                         uint32_t(upload_buffer_size >> page_size_log2()));
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        command_buffer->CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                        uint32_t(upload_regions_.size()), upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start) << page_size_log2();
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    command_buffer->CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                    uint32_t(upload_regions_.size()), upload_regions_.data());
    upload_regions_.clear();
  }
  if (before_render_pass) {
    // Read by the render pass's draws, which run after it; the usage stays a
    // read.
    RecordBarrier(command_buffer, Usage::kTransferDestination, Usage::kRead, written_range.first,
                  written_range.second);
    submitter_->EndBeforeRenderPass();
  } else {
    // Read by the draws next; done here, outside their render pass.
    Use(Usage::kRead);
  }
  return successful;
}

}  // namespace rex::graphics::native
