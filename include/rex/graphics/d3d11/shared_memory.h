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

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// The guest physical memory copy: one 512 MB raw buffer, readable as a raw
// SRV (vertex fetch, texture loading), writable as a raw UAV (memexport,
// resolves) and usable as an index buffer (guest indices used directly). A
// buffer of this size can be created at feature level 11.0 on the tested
// hardware (RTX 4080, UHD Graphics 770 with 128 MB of dedicated memory, WARP);
// creation failing ends the initialization with an error.
class D3D11SharedMemory : public SharedMemory {
 public:
  D3D11SharedMemory(const ui::d3d11::D3D11Provider& provider, memory::Memory& memory);
  ~D3D11SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  ID3D11Buffer* GetBuffer() const { return buffer_.Get(); }
  ID3D11ShaderResourceView* GetRawSRV() const { return buffer_raw_srv_.Get(); }
  ID3D11UnorderedAccessView* GetRawUAV() const { return buffer_raw_uav_.Get(); }

  // For the GPU time of uploads (D3D11CommandProcessor::SetGpuSection).
  void SetCommandProcessor(D3D11CommandProcessor* command_processor) {
    command_processor_ = command_processor;
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  // Data copied with UpdateSubresource per call at most, so one call doesn't
  // make the driver stage a huge allocation.
  static constexpr uint32_t kUploadChunkSizeLog2 = 22;

  const ui::d3d11::D3D11Provider& provider_;
  D3D11CommandProcessor* command_processor_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> buffer_raw_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> buffer_raw_uav_;
};

}  // namespace rex::graphics::d3d11
