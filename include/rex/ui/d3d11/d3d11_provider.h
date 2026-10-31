/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 provider; the context mutex follows TheSimpsonsGameRecomp
 *              (YesterMester, commit 66bb08c0, BSD 3-Clause).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_3.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>
// For Microsoft::WRL::ComPtr.
#include <wrl/client.h>
// clang-format on

#include <rex/ui/graphics_provider.h>

namespace rex::ui::d3d11 {

// The Direct3D 11 immediate context is not thread-safe, and the GPU thread
// (command processor, guest output refresh) and the UI thread (presenter, UI
// drawers) both use it, so every use is under this lock. The epoch advances
// whenever a different thread takes the lock, so a user that caches context
// state knows when another thread may have changed it.
class ContextMutex {
 public:
  // Called with the lock held whenever a thread other than the previous owner
  // takes it, before that thread uses the context: work that must not see
  // another thread's commands (an occlusion query counts every draw on the
  // context) can be ended there. One callback, set and cleared under the lock.
  void SetOwnerChangeCallback(std::function<void()> callback) {
    owner_change_callback_ = std::move(callback);
  }

  void lock() {
    mutex_.lock();
    Acquired();
  }
  bool try_lock() {
    if (!mutex_.try_lock()) {
      return false;
    }
    Acquired();
    return true;
  }
  void unlock() {
    --depth_;
    mutex_.unlock();
  }
  // Only meaningful while the calling thread holds the lock.
  uint64_t epoch() const { return epoch_; }

 private:
  void Acquired() {
    if (depth_++) {
      return;
    }
    std::thread::id thread = std::this_thread::get_id();
    if (thread != owner_) {
      owner_ = thread;
      ++epoch_;
      if (owner_change_callback_) {
        owner_change_callback_();
      }
    }
  }

  std::recursive_mutex mutex_;
  uint32_t depth_ = 0;
  std::thread::id owner_;
  uint64_t epoch_ = 0;
  std::function<void()> owner_change_callback_;
};

class D3D11Provider : public GraphicsProvider {
 public:
  ~D3D11Provider();

  static std::unique_ptr<D3D11Provider> Create();

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  IDXGIFactory2* GetDXGIFactory() const { return dxgi_factory_.Get(); }
  ID3D11Device* GetDevice() const { return device_.Get(); }
  // Null below Direct3D 11.1 runtimes.
  ID3D11Device1* GetDevice1() const { return device_1_.Get(); }
  // Use only under context_mutex().
  ID3D11DeviceContext* GetImmediateContext() const { return immediate_context_.Get(); }
  ID3D11DeviceContext1* GetImmediateContext1() const { return immediate_context_1_.Get(); }
  ContextMutex& context_mutex() const { return context_mutex_; }

  D3D_FEATURE_LEVEL GetFeatureLevel() const { return feature_level_; }
  GpuVendorID GetAdapterVendorID() const { return adapter_vendor_id_; }
  const std::string& GetAdapterDescription() const { return adapter_description_; }
  bool IsSoftwareAdapter() const { return software_adapter_; }

  // UAVs in all shader stages and 64 UAV slots (feature level 11.1).
  bool AreUAVsAtEveryStageSupported() const { return feature_level_ >= D3D_FEATURE_LEVEL_11_1; }
  uint32_t GetUAVSlotCount() const {
    return AreUAVsAtEveryStageSupported() ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
  }
  // *SetConstantBuffers1 with offsets.
  bool IsConstantBufferOffsettingSupported() const { return constant_buffer_offsetting_; }
  // Optional Direct3D 11.3 features (masked by d3d11_optional_features).
  bool IsPSSpecifiedStencilReferenceSupported() const {
    return ps_specified_stencil_reference_supported_;
  }
  bool AreRasterizerOrderedViewsSupported() const { return rasterizer_ordered_views_supported_; }
  bool AreTypedUAVLoadAdditionalFormatsSupported() const {
    return typed_uav_load_additional_formats_supported_;
  }

  // Logs and clears the messages of the debug layer, if it's enabled.
  void LogD3D11DebugMessages() const;

 private:
  D3D11Provider() = default;

  bool Initialize();

  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Device1> device_1_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_context_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> immediate_context_1_;
  Microsoft::WRL::ComPtr<ID3D11InfoQueue> info_queue_;
  mutable ContextMutex context_mutex_;

  D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
  GpuVendorID adapter_vendor_id_ = GpuVendorID(0);
  uint32_t adapter_device_id_ = 0;
  std::string adapter_description_;
  bool software_adapter_ = false;

  bool constant_buffer_offsetting_ = false;
  bool ps_specified_stencil_reference_supported_ = false;
  bool rasterizer_ordered_views_supported_ = false;
  bool typed_uav_load_additional_formats_supported_ = false;
};

}  // namespace rex::ui::d3d11
