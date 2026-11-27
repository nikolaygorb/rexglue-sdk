/**
 * @file        graphics/native/primitive_processor.h
 * @brief       Native renderer: guest primitives and index buffers for the host
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Modelled on the SDK's VulkanPrimitiveProcessor (Xenia, Copyright 2021 Ben
 *              Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

#include <rex/graphics/primitive_processor.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::native {

class Submitter;

// The SDK's PrimitiveProcessor (index fetch, primitive reset, conversions) for
// the native renderer. Without geometry shaders: triangle fans, line loops and
// quad lists become lists and strips in index buffers, point sprites and
// rectangle lists are expanded in the vertex shader (the translator's
// kPointListAsTriangleStrip and kRectangleListAsTriangleStrip), so every
// device, MoltenVK included, takes the same path.
class NativePrimitiveProcessor final : public PrimitiveProcessor {
 public:
  NativePrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory)
      : PrimitiveProcessor(register_file, memory, shared_memory) {}
  ~NativePrimitiveProcessor() override;

  bool Initialize(Submitter& submitter);
  void Shutdown(bool from_destructor = false);

  // Before the first draw of a frame (numbered from 1); completed_frame: the
  // last frame the GPU has finished. Converted index buffers live for a frame.
  void BeginFrame(uint64_t frame, uint64_t completed_frame);
  void EndFrame();

  // Records the upload of the built-in index buffer the first time (outside a
  // render pass): before a draw that may use it.
  void UploadBuiltinIndexBufferIfNeeded();

  std::pair<VkBuffer, VkDeviceSize> GetBuiltinIndexBuffer(size_t handle) const {
    return std::make_pair(builtin_index_buffer_,
                          VkDeviceSize(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<VkBuffer, VkDeviceSize> GetConvertedIndexBuffer(size_t handle) const {
    return frame_index_buffers_[handle];
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  Submitter* submitter_ = nullptr;

  VkDeviceSize builtin_index_buffer_size_ = 0;
  VkBuffer builtin_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_memory_ = VK_NULL_HANDLE;
  // Host-visible copy, uploaded once, destroyed after that submission.
  VkBuffer builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  bool builtin_index_buffer_uploaded_ = false;

  uint64_t frame_ = 1;
  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> frame_index_buffer_pool_;
  // Indexed by the backend handles.
  std::deque<std::pair<VkBuffer, VkDeviceSize>> frame_index_buffers_;
};

}  // namespace rex::graphics::native
