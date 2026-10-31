/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 primitive processor.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/primitive_processor.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11PrimitiveProcessor final : public PrimitiveProcessor {
 public:
  D3D11PrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                          SharedMemory& shared_memory, const ui::d3d11::D3D11Provider& provider)
      : PrimitiveProcessor(register_file, memory, shared_memory), provider_(provider) {}
  ~D3D11PrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void EndFrame();

  ID3D11Buffer* GetBuiltinIndexBuffer() const { return builtin_index_buffer_.Get(); }
  uint32_t GetBuiltinIndexBufferOffset(size_t handle) const {
    return uint32_t(GetBuiltinIndexBufferOffsetBytes(handle));
  }
  // Uploads the converted indices of the handle (once per ring generation) and
  // binds them as the index buffer. Call under the context mutex.
  bool BindConvertedIndexBuffer(size_t handle, DXGI_FORMAT format);

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  // CPU memory for the indices converted in the current frame, in chunks so
  // the returned pointers stay valid for the whole frame.
  static constexpr size_t kFrameChunkSize = size_t(4) << 20;
  struct FrameChunk {
    std::unique_ptr<uint8_t[]> data;
    size_t size;
    size_t used;
  };
  struct ConvertedIndices {
    const uint8_t* data;
    uint32_t size_bytes;
    // Offset in the ring buffer, valid while ring_generation matches.
    uint32_t ring_offset;
    uint64_t ring_generation;
  };

  static constexpr uint32_t kRingBufferMinSize = uint32_t(16) << 20;

  const ui::d3d11::D3D11Provider& provider_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> builtin_index_buffer_;

  std::vector<FrameChunk> frame_chunks_;
  size_t frame_chunks_used_ = 0;
  std::vector<ConvertedIndices> frame_converted_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> ring_buffer_;
  uint32_t ring_buffer_size_ = 0;
  uint32_t ring_buffer_position_ = 0;
  // Incremented on every discard of the ring buffer.
  uint64_t ring_generation_ = 1;
};

}  // namespace rex::graphics::d3d11
