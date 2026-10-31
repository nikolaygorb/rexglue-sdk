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

#include <rex/graphics/d3d11/primitive_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::d3d11 {

D3D11PrimitiveProcessor::~D3D11PrimitiveProcessor() { Shutdown(true); }

bool D3D11PrimitiveProcessor::Initialize() {
  // Direct3D 11 restarts strips at all-ones indices whatever the guest asks.
  SetHostStripPrimitiveResetAlwaysEnabled(true);
  // Same host capabilities as Direct3D 12: full 32-bit indices, no fans or
  // line loops, quads, points and rectangles through geometry shaders.
  if (!InitializeCommon(true, false, false, true, true, true)) {
    Shutdown();
    return false;
  }
  return true;
}

void D3D11PrimitiveProcessor::Shutdown(bool from_destructor) {
  frame_converted_.clear();
  frame_chunks_.clear();
  frame_chunks_used_ = 0;
  ring_buffer_.Reset();
  ring_buffer_size_ = 0;
  ring_buffer_position_ = 0;
  builtin_index_buffer_.Reset();
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D11PrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_converted_.clear();
  for (FrameChunk& chunk : frame_chunks_) {
    chunk.used = 0;
  }
  frame_chunks_used_ = 0;
}

bool D3D11PrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_null(builtin_index_buffer_);
  std::vector<uint8_t> data(size_bytes);
  fill_callback(data.data());
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(size_bytes);
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  D3D11_SUBRESOURCE_DATA initial_data = {};
  initial_data.pSysMem = data.data();
  if (FAILED(provider_.GetDevice()->CreateBuffer(&desc, &initial_data, &builtin_index_buffer_))) {
    REXGPU_ERROR(
        "D3D11 primitive processor: Failed to create the built-in index buffer with {} bytes",
        size_bytes);
    return false;
  }
  return true;
}

void* D3D11PrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t size_bytes = index_size * index_count;
  size_t allocation_size =
      size_bytes + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0) + 16;
  // First chunk with enough space from the current one on.
  FrameChunk* chunk = nullptr;
  while (frame_chunks_used_ < frame_chunks_.size()) {
    FrameChunk& candidate = frame_chunks_[frame_chunks_used_];
    if (candidate.size - candidate.used >= allocation_size) {
      chunk = &candidate;
      break;
    }
    ++frame_chunks_used_;
  }
  if (!chunk) {
    FrameChunk& new_chunk = frame_chunks_.emplace_back();
    new_chunk.size = std::max(kFrameChunkSize, allocation_size);
    new_chunk.data = std::make_unique<uint8_t[]>(new_chunk.size);
    new_chunk.used = 0;
    frame_chunks_used_ = frame_chunks_.size() - 1;
    chunk = &new_chunk;
  }
  uint8_t* mapping = chunk->data.get() + rex::align(chunk->used, size_t(16));
  if (coalign_for_simd) {
    mapping += GetSimdCoalignmentOffset(mapping, coalignment_original_address);
  }
  chunk->used = size_t(mapping - chunk->data.get()) + size_bytes;
  backend_handle_out = frame_converted_.size();
  frame_converted_.push_back({mapping, uint32_t(size_bytes), 0, 0});
  return mapping;
}

bool D3D11PrimitiveProcessor::BindConvertedIndexBuffer(size_t handle, DXGI_FORMAT format) {
  assert_true(handle < frame_converted_.size());
  ConvertedIndices& converted = frame_converted_[handle];
  ID3D11DeviceContext* context = provider_.GetImmediateContext();
  if (converted.ring_generation != ring_generation_) {
    uint32_t size_bytes = std::max(converted.size_bytes, uint32_t(sizeof(uint32_t)));
    if (ring_buffer_size_ < size_bytes) {
      ring_buffer_.Reset();
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = std::max(kRingBufferMinSize, rex::next_pow2(size_bytes));
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(provider_.GetDevice()->CreateBuffer(&desc, nullptr, &ring_buffer_))) {
        REXGPU_ERROR("D3D11 primitive processor: Failed to create a {} byte index buffer",
                     desc.ByteWidth);
        ring_buffer_size_ = 0;
        return false;
      }
      ring_buffer_size_ = desc.ByteWidth;
      ring_buffer_position_ = ring_buffer_size_;
    }
    uint32_t offset = rex::align(ring_buffer_position_, uint32_t(sizeof(uint32_t)));
    D3D11_MAP map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (offset > ring_buffer_size_ || ring_buffer_size_ - offset < size_bytes) {
      // Full: start over, the GPU keeps the old contents for the earlier draws.
      offset = 0;
      map_type = D3D11_MAP_WRITE_DISCARD;
      ++ring_generation_;
    }
    D3D11_MAPPED_SUBRESOURCE mapping;
    if (FAILED(context->Map(ring_buffer_.Get(), 0, map_type, 0, &mapping))) {
      return false;
    }
    std::memcpy(reinterpret_cast<uint8_t*>(mapping.pData) + offset, converted.data,
                converted.size_bytes);
    context->Unmap(ring_buffer_.Get(), 0);
    ring_buffer_position_ = offset + size_bytes;
    converted.ring_offset = offset;
    converted.ring_generation = ring_generation_;
  }
  context->IASetIndexBuffer(ring_buffer_.Get(), format, converted.ring_offset);
  return true;
}

}  // namespace rex::graphics::d3d11
