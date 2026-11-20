/**
 * @file        graphics/native/memexport.h
 * @brief       Native renderer: memory export written back to the CPU (readback_memexport)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanCommandProcessor's memexport readback (Xenia, Copyright 2022 Ben
 *              Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/util/draw.h>
#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics::native {

class NativeSharedMemory;
class ResolveReadback;
class Submitter;

// What draws with memory export wrote into the shared memory, copied into
// guest memory for the CPU, as the SDK's Vulkan backend does it:
// - full (readback_memexport_fast off): the ranges read before the draw
//   (TakeSnapshot) and after it, the GPU waited for both times; only the
//   bytes the draw changed are written, so CPU writes in between survive;
// - fast: the ranges copied into one of two host buffers per export stream
//   (by its first address and size); the other one, from an earlier draw of
//   the stream that the GPU has finished, is written whole (a frame late);
//   the GPU is waited for only the first time.
// Command processor thread only.
class MemexportReadback {
 public:
  MemexportReadback() = default;
  MemexportReadback(const MemexportReadback&) = delete;
  MemexportReadback& operator=(const MemexportReadback&) = delete;
  ~MemexportReadback();

  void Initialize(Submitter& submitter, NativeSharedMemory& shared_memory,
                  ResolveReadback& readback, memory::Memory& memory);
  void Shutdown();

  // Full mode, before the draw (outside the render pass, which it ends).
  void TakeSnapshot(const std::vector<draw_util::MemExportRange>& ranges);
  // After the draw: into guest memory (fast or full).
  void ReadBack(const std::vector<draw_util::MemExportRange>& ranges, bool fast, uint64_t frame);
  // Frees the fast buffers of streams unused for a while.
  void EndFrame(uint64_t frame);

 private:
  struct Slot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    VkDeviceSize size = 0;
    // The submission that copied into it, and how much.
    uint64_t submission = 0;
    uint32_t written_size = 0;
  };
  struct Stream {
    Slot slots[2];
    uint32_t current = 0;
    uint64_t last_used_frame = 0;
  };

  void ReadBackFull(const std::vector<draw_util::MemExportRange>& ranges);
  void ReadBackFast(const std::vector<draw_util::MemExportRange>& ranges, uint32_t total_size,
                    uint64_t frame);
  bool EnsureSlot(Slot& slot, uint32_t size);
  // Freed once the GPU is done with it.
  void DestroySlotLater(Slot& slot);
  void DestroySlotNow(Slot& slot);
  void WriteToGuest(const std::vector<draw_util::MemExportRange>& ranges, const uint8_t* data);

  Submitter* submitter_ = nullptr;
  NativeSharedMemory* shared_memory_ = nullptr;
  ResolveReadback* readback_ = nullptr;
  memory::Memory* memory_ = nullptr;

  // Full mode: the bytes before the draw, all ranges one after another.
  std::vector<uint8_t> snapshot_;
  bool snapshot_valid_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> ranges_scratch_;
  std::vector<const uint8_t*> data_scratch_;

  std::unordered_map<uint64_t, Stream> streams_;
};

}  // namespace rex::graphics::native
