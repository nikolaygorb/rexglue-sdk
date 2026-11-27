/**
 * @file        graphics/native/shared_memory.h
 * @brief       Native renderer: the guest physical memory on the GPU
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Modelled on the SDK's VulkanSharedMemory (Xenia, Copyright 2020 Ben Vanik,
 *              BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/shared_memory.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::native {

class Submitter;

// The 512 MB of guest physical memory as one Vulkan buffer: vertex fetch and
// memory export of the translated shaders (set 0, binding 0) and DMA index
// buffers read it. The SDK's SharedMemory decides what is out of date (write
// watches, hot pages, upload hashing); this uploads it. Not sparse yet: the
// whole buffer is allocated (milestone N7).
class NativeSharedMemory : public SharedMemory {
 public:
  explicit NativeSharedMemory(memory::Memory& memory) : SharedMemory(memory) {}
  ~NativeSharedMemory() override;

  bool Initialize(Submitter& submitter);
  void Shutdown(bool from_destructor = false);

  VkBuffer buffer() const { return buffer_; }

  enum class Usage {
    // Index buffer, vertex fetch, transfer source, compute reads (texture
    // loading).
    kRead,
    // Draws with memory export: reads and shader writes.
    kGuestDrawReadWrite,
    kTransferDestination,
    // Compute shader writes (resolves into memory).
    kComputeWrite,
  };
  // A barrier from the last usage to `usage` when needed (outside the draws'
  // render pass, which it ends then); written_range is what the new usage
  // writes, ordered before the next one.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});
  // Read only since the last barrier (no upload or memory export pending a
  // barrier): commands that only read it may be recorded before the open
  // render pass (Submitter::BeginBeforeRenderPass) without one.
  bool IsReadOnly() const { return last_usage_ == Usage::kRead && !last_written_range_.second; }

  // What a draw of the open render pass reads (just before it's recorded).
  // An upload of none of these pages is recorded before the render pass when
  // the submitter allows it (Submitter::BeginBeforeRenderPass), and the
  // render pass stays open.
  void MarkReadInRenderPass(uint32_t start, uint32_t length);

  // The stages the translated shaders run in.
  static constexpr VkPipelineStageFlags kGuestShaderStages =
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  static void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask);
  void RecordBarrier(CommandRecorder* command_buffer, Usage from, Usage to, VkDeviceSize offset,
                     VkDeviceSize size) const;
  bool IsReadInRenderPass(uint32_t page_first, uint32_t page_last) const;

  Submitter* submitter_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory_ = VK_NULL_HANDLE;

  Usage last_usage_ = Usage::kTransferDestination;
  std::pair<uint32_t, uint32_t> last_written_range_ = {0, 0};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // Pages read by the open render pass's draws: a bit per page, valid in the
  // words whose serial is the render pass's (Submitter::render_pass_serial),
  // so nothing is cleared between render passes.
  std::vector<uint64_t> render_pass_read_pages_;
  std::vector<uint64_t> render_pass_read_serials_;
};

}  // namespace rex::graphics::native
