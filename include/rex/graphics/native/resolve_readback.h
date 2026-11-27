/**
 * @file        graphics/native/resolve_readback.h
 * @brief       Native renderer: resolves copied back to the CPU (readback_resolve)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanCommandProcessor's resolve readback (Xenia, Copyright 2022 Ben
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

#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class NativeSharedMemory;
class Submitter;

// Ranges of the shared memory (where resolves write the guest's bytes,
// ResolveMemory) copied into a host-visible buffer, the GPU waited for. The
// SDK's CommandProcessor decides what is copied back and writes it into guest
// memory (ShouldReadbackResolve, CopyResolveToGuest). Command processor
// thread only.
class ResolveReadback {
 public:
  ResolveReadback() = default;
  ResolveReadback(const ResolveReadback&) = delete;
  ResolveReadback& operator=(const ResolveReadback&) = delete;
  ~ResolveReadback();

  void Initialize(Submitter& submitter, NativeSharedMemory& shared_memory);
  void Shutdown();

  // Copies each (physical address, length) range to the host now, waiting
  // for the GPU. data_out gets each one's bytes (nullptr where that failed),
  // valid until the next call.
  void ReadNow(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
               std::vector<const uint8_t*>& data_out);

  // readback_resolve_full_deferred: records the copy of one resolve
  // destination (physical address, length) into a buffer of its own, not
  // waited for; CompleteDeferred gives its bytes when the guest reads it.
  // False if it couldn't be recorded (ReadNow then).
  bool RecordDeferred(uint32_t address, uint32_t length);
  // Waits for the last RecordDeferred copy of each range (the same address
  // and length) and gives its bytes, nullptr where there is none; valid until
  // the next RecordDeferred of that range.
  void CompleteDeferred(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                        std::vector<const uint8_t*>& data_out);
  // Frees the copies of destinations not resolved for a while.
  void EndFrame(uint64_t frame);

 private:
  bool EnsureBuffer(VkDeviceSize size);
  void DestroyBuffer();

  struct DeferredCopy {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    VkDeviceSize size = 0;
    // The submission holding the last copy into it.
    uint64_t submission = 0;
    uint64_t last_frame = 0;
  };
  static uint64_t DeferredKey(uint32_t address, uint32_t length) {
    return (uint64_t(address & 0x1FFFFFFF) << 32) | length;
  }
  bool CreateDeferredBuffer(DeferredCopy& copy, VkDeviceSize size);
  // After the GPU is done with it (Submitter::DestroyAfter), or now.
  void DestroyDeferredBuffer(DeferredCopy& copy, bool now);

  Submitter* submitter_ = nullptr;
  NativeSharedMemory* shared_memory_ = nullptr;
  // Every ReadNow waits for its copies: no GPU work uses it between calls.
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkDeviceSize size_ = 0;
  void* mapping_ = nullptr;
  std::vector<VkBufferCopy> regions_;
  std::vector<VkDeviceSize> offsets_;
  // RecordDeferred's copies by destination (DeferredKey).
  std::unordered_map<uint64_t, DeferredCopy> deferred_;
  uint64_t frame_ = 0;
};

}  // namespace rex::graphics::native
