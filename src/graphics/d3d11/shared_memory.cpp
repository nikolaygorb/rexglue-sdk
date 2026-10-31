/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 shared memory.
 */

#include <rex/graphics/d3d11/shared_memory.h>

#include <algorithm>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11SharedMemory::D3D11SharedMemory(const ui::d3d11::D3D11Provider& provider,
                                     memory::Memory& memory)
    : SharedMemory(memory), provider_(provider) {}

D3D11SharedMemory::~D3D11SharedMemory() { Shutdown(true); }

bool D3D11SharedMemory::Initialize() {
  InitializeCommon();

  ID3D11Device* device = provider_.GetDevice();
  D3D11_BUFFER_DESC buffer_desc = {};
  buffer_desc.ByteWidth = kBufferSize;
  buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  buffer_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_INDEX_BUFFER;
  buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &buffer_))) {
    REXGPU_ERROR(
        "Shared memory: Failed to create the {} MB buffer (the largest Direct3D 11 resource "
        "depends on the video memory); use Direct3D 12 or Vulkan on this GPU",
        kBufferSize >> 20);
    Shutdown();
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.NumElements = kBufferSize / sizeof(uint32_t);
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = kBufferSize / sizeof(uint32_t);
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateShaderResourceView(buffer_.Get(), &srv_desc, &buffer_raw_srv_)) ||
      FAILED(device->CreateUnorderedAccessView(buffer_.Get(), &uav_desc, &buffer_raw_uav_))) {
    REXGPU_ERROR("Shared memory: Failed to create the buffer views");
    Shutdown();
    return false;
  }
  return true;
}

void D3D11SharedMemory::Shutdown(bool from_destructor) {
  buffer_raw_uav_.Reset();
  buffer_raw_srv_.Reset();
  buffer_.Reset();
  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool D3D11SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider_.context_mutex());
  std::optional<D3D11CommandProcessor::ScopedGpuSection> gpu_section;
  if (command_processor_) {
    gpu_section.emplace(*command_processor_, D3D11CommandProcessor::GpuSection::kUploads);
  }
  ID3D11DeviceContext* context = provider_.GetImmediateContext();
  const uint32_t chunk_pages = uint32_t(1) << (kUploadChunkSizeLog2 - page_size_log2());
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;

    if (upload_range_length > 0 && !REXCVAR_GET(gpu_allow_invalid_upload_range)) {
      // Unmapped memory at the start or the end of the range, as on D3D12.
      const uint32_t range_start_addr = upload_range_start << page_size_log2();
      const uint32_t range_end_addr = (upload_range_start + upload_range_length - 1)
                                      << page_size_log2();
      const rex::memory::PageAccess start_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_start_addr, range_start_addr);
      const rex::memory::PageAccess end_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_end_addr, range_end_addr);
      if (start_access == rex::memory::PageAccess::kNoAccess ||
          end_access == rex::memory::PageAccess::kNoAccess) {
        REXGPU_ERROR("Invalid upload range for GPU: {:08X} length {:08X}", upload_range_start,
                     upload_range_length);
        return false;
      }
    }

    while (upload_range_length) {
      uint32_t pages = std::min(upload_range_length, chunk_pages);
      uint32_t offset = upload_range_start << page_size_log2();
      uint32_t size = pages << page_size_log2();
      // Valid before copying, so an invalidation during the copy isn't lost.
      MakeRangeValid(offset, size, false);
      D3D11_BOX box = {};
      box.left = offset;
      box.right = offset + size;
      box.bottom = 1;
      box.back = 1;
      // UpdateSubresource takes the data during the call.
      context->UpdateSubresource(buffer_.Get(), 0, &box, memory().TranslatePhysical(offset), 0, 0);
      upload_range_start += pages;
      upload_range_length -= pages;
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d11
