/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 provider.
 */

#include <rex/ui/d3d11/d3d11_provider.h>

#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/flags.h>
#include <rex/ui/gpu_vendor.h>

REXCVAR_DEFINE_BOOL(d3d11_debug, false, "UI/D3D11",
                    "Enable the Direct3D 11 debug layer (needs the Graphics Tools optional "
                    "feature of Windows)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(d3d11_adapter, -1, "UI/D3D11",
                     "Direct3D 11 adapter: -1 to choose automatically (gpu_vendor first, then "
                     "the most dedicated video memory), -2 for WARP (software), or the DXGI "
                     "adapter index")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(d3d11_feature_level, "", "UI/D3D11",
                      "Highest Direct3D 11 feature level to use: empty for the highest the "
                      "adapter supports, \"11_0\" or \"11_1\" (for testing older hardware)")
    .allowed({"", "11_0", "11_1"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d11_optional_features, true, "UI/D3D11",
                    "Use the optional Direct3D 11.3 features the adapter reports (stencil "
                    "reference output, rasterizer-ordered views, typed UAV loads); off behaves "
                    "like hardware without them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::ui::d3d11 {

namespace {

// Highest first, as D3D11CreateDevice expects.
std::vector<D3D_FEATURE_LEVEL> FeatureLevels() {
  std::vector<D3D_FEATURE_LEVEL> levels;
  if (REXCVAR_GET(d3d11_feature_level) != "11_0") {
    levels.push_back(D3D_FEATURE_LEVEL_11_1);
  }
  levels.push_back(D3D_FEATURE_LEVEL_11_0);
  return levels;
}

HRESULT CreateDevice(IDXGIAdapter* adapter, bool warp, UINT flags, ID3D11Device** device_out,
                     D3D_FEATURE_LEVEL* level_out, ID3D11DeviceContext** context_out) {
  std::vector<D3D_FEATURE_LEVEL> levels = FeatureLevels();
  D3D_DRIVER_TYPE driver_type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN;
  HRESULT result = D3D11CreateDevice(warp ? nullptr : adapter, driver_type, nullptr, flags,
                                     levels.data(), UINT(levels.size()), D3D11_SDK_VERSION,
                                     device_out, level_out, context_out);
  if (result == E_INVALIDARG && levels.front() == D3D_FEATURE_LEVEL_11_1) {
    // Direct3D 11.0 runtimes don't know feature level 11.1.
    result = D3D11CreateDevice(warp ? nullptr : adapter, driver_type, nullptr, flags,
                               levels.data() + 1, UINT(levels.size() - 1), D3D11_SDK_VERSION,
                               device_out, level_out, context_out);
  }
  return result;
}

std::string WideToUtf8(const wchar_t* text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return std::string();
  }
  std::string result(size_t(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  return result;
}

}  // namespace

std::unique_ptr<D3D11Provider> D3D11Provider::Create() {
  std::unique_ptr<D3D11Provider> provider(new D3D11Provider);
  if (!provider->Initialize()) {
    REXLOG_ERROR(
        "Unable to initialize Direct3D 11 graphics subsystem. A GPU with feature level 11_0 is "
        "required; Direct3D 12 or Vulkan are the backends for newer GPUs.");
    return nullptr;
  }
  return provider;
}

D3D11Provider::~D3D11Provider() {
  if (immediate_context_) {
    std::lock_guard<ContextMutex> lock(context_mutex_);
    immediate_context_->ClearState();
    immediate_context_->Flush();
  }
}

bool D3D11Provider::Initialize() {
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi_factory_)))) {
    REXLOG_ERROR("Failed to create a DXGI factory");
    return false;
  }

  // Choose the adapter, as the D3D12 provider does: an index or WARP is taken
  // as given, otherwise the vendor asked for with gpu_vendor first, then the
  // one with the most dedicated video memory.
  const int32_t wanted_adapter = REXCVAR_GET(d3d11_adapter);
  const bool warp = wanted_adapter == -2;
  const uint32_t wanted_vendor = PreferredGpuVendorId(REXCVAR_GET(gpu_vendor));
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  uint32_t chosen_index = 0;
  if (!warp) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    uint32_t best_vendor_match = 0;
    SIZE_T best_memory = 0;
    bool vendor_found = false;
    for (uint32_t index = 0; dxgi_factory_->EnumAdapters1(index, &candidate) == S_OK; ++index) {
      DXGI_ADAPTER_DESC1 desc;
      D3D_FEATURE_LEVEL level;
      if (FAILED(candidate->GetDesc1(&desc)) ||
          FAILED(CreateDevice(candidate.Get(), false, 0, nullptr, &level, nullptr))) {
        continue;
      }
      bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      REXLOG_INFO("DXGI adapter {}: vendor 0x{:04X}, device 0x{:04X}, {} MB dedicated{}", index,
                  desc.VendorId, desc.DeviceId, uint64_t(desc.DedicatedVideoMemory >> 20),
                  software ? ", software" : "");
      bool keep = false;
      if (wanted_adapter >= 0) {
        keep = index == uint32_t(wanted_adapter);
      } else if (!software) {
        uint32_t vendor_match = wanted_vendor && desc.VendorId == wanted_vendor ? 1 : 0;
        vendor_found = vendor_found || vendor_match;
        keep = !adapter || vendor_match > best_vendor_match ||
               (vendor_match == best_vendor_match && desc.DedicatedVideoMemory > best_memory);
        if (keep) {
          best_vendor_match = vendor_match;
          best_memory = desc.DedicatedVideoMemory;
        }
      }
      if (keep) {
        adapter = candidate;
        chosen_index = index;
        if (wanted_adapter >= 0) {
          break;
        }
      }
    }
    if (wanted_vendor && wanted_adapter < 0 && !vendor_found) {
      REXLOG_WARN("gpu_vendor '{}' matches no Direct3D 11 adapter; choosing automatically",
                  REXCVAR_GET(gpu_vendor));
    }
    if (!adapter) {
      REXLOG_ERROR("Failed to get an adapter supporting Direct3D 11 with feature level 11_0");
      return false;
    }
  }

  UINT flags = 0;
  if (REXCVAR_GET(d3d11_debug)) {
    flags |= D3D11_CREATE_DEVICE_DEBUG;
  }
  HRESULT result = CreateDevice(adapter.Get(), warp, flags, &device_, &feature_level_,
                                &immediate_context_);
  if (FAILED(result) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
    REXLOG_WARN(
        "Failed to create a Direct3D 11 device with the debug layer (Graphics Tools not "
        "installed?), creating one without it");
    flags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
    result = CreateDevice(adapter.Get(), warp, flags, &device_, &feature_level_,
                          &immediate_context_);
  }
  if (FAILED(result)) {
    REXLOG_ERROR("Failed to create a Direct3D 11 device, result 0x{:08X}", uint32_t(result));
    return false;
  }
  device_.As(&device_1_);
  immediate_context_.As(&immediate_context_1_);
  if (flags & D3D11_CREATE_DEVICE_DEBUG) {
    device_.As(&info_queue_);
  }

  // Adapter information (of the adapter the device really uses, also for WARP).
  {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(device_.As(&dxgi_device)) &&
        SUCCEEDED(dxgi_device->GetAdapter(&device_adapter)) &&
        SUCCEEDED(device_adapter->GetDesc(&desc))) {
      adapter_vendor_id_ = GpuVendorID(desc.VendorId);
      adapter_device_id_ = desc.DeviceId;
      adapter_description_ = WideToUtf8(desc.Description);
    }
    software_adapter_ = warp || uint32_t(adapter_vendor_id_) == uint32_t(GpuVendorID::kMicrosoft);
  }

  // Features.
  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                             sizeof(options)))) {
    constant_buffer_offsetting_ = options.ConstantBufferOffsetting && immediate_context_1_;
  }
  if (REXCVAR_GET(d3d11_optional_features)) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2,
                                               sizeof(options2)))) {
      ps_specified_stencil_reference_supported_ = options2.PSSpecifiedStencilRefSupported;
      rasterizer_ordered_views_supported_ = options2.ROVsSupported;
      typed_uav_load_additional_formats_supported_ = options2.TypedUAVLoadAdditionalFormats;
    }
  }

  REXGPU_INFO("Direct3D 11 adapter in use: {}: {} (vendor 0x{:04X}, device 0x{:04X})",
              warp ? std::string("WARP") : std::to_string(chosen_index), adapter_description_,
              uint32_t(adapter_vendor_id_), adapter_device_id_);
  REXGPU_INFO(
      "Direct3D 11 device: feature level {}{}, constant buffer offsetting {}, PS stencil "
      "reference {}, rasterizer-ordered views {}, typed UAV loads of additional formats {}{}",
      feature_level_ >= D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0",
      REXCVAR_GET(d3d11_feature_level).empty() ? ""
                                               : " (capped by d3d11_feature_level)",
      constant_buffer_offsetting_, ps_specified_stencil_reference_supported_,
      rasterizer_ordered_views_supported_, typed_uav_load_additional_formats_supported_,
      REXCVAR_GET(d3d11_optional_features) ? "" : " (optional features off)");
  return true;
}

std::unique_ptr<Presenter> D3D11Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D11Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D11Provider::CreateImmediateDrawer() {
  return D3D11ImmediateDrawer::Create(*this);
}

void D3D11Provider::LogD3D11DebugMessages() const {
  if (!info_queue_) {
    return;
  }
  UINT64 count = info_queue_->GetNumStoredMessages();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T length = 0;
    if (FAILED(info_queue_->GetMessage(i, nullptr, &length)) || !length) {
      continue;
    }
    std::vector<uint8_t> storage(length);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    if (FAILED(info_queue_->GetMessage(i, message, &length))) {
      continue;
    }
    const char* severity;
    switch (message->Severity) {
      case D3D11_MESSAGE_SEVERITY_CORRUPTION:
        severity = "corruption";
        break;
      case D3D11_MESSAGE_SEVERITY_ERROR:
        severity = "error";
        break;
      case D3D11_MESSAGE_SEVERITY_WARNING:
        severity = "warning";
        break;
      default:
        severity = "info";
        break;
    }
    REXLOG_INFO("D3D11 {}: [ID {}] {}", severity, int(message->ID),
                std::string_view(message->pDescription, message->DescriptionByteLength
                                                            ? message->DescriptionByteLength - 1
                                                            : 0));
  }
  info_queue_->ClearStoredMessages();
}

}  // namespace rex::ui::d3d11
