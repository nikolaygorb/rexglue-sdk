/**
 * @file        launcher/launcher_devices.cpp
 * @brief       Rendering adapters for Vulkan and Direct3D launch settings
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 */

#include "launcher_devices.h"

#include <filesystem>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#if REX_HAS_VULKAN
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/ngx.h>
#include <rex/ui/vulkan/xess.h>
#endif
#ifdef _WIN32
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace rex::launcher {

RenderDevices EnumerateRenderDevices() {
  RenderDevices result;
  result.vulkan.push_back(Choice{"-1", "Auto", ""});
  result.dxgi.push_back(Choice{"-1", "Auto", ""});

#if REX_HAS_VULKAN
  // Use the same loader/instance helper and enumeration as VulkanProvider,
  // without requiring presentation, validation, or a selected GPU's features.
  if (auto instance = rex::ui::vulkan::VulkanInstance::Create(false, false)) {
    std::vector<VkPhysicalDevice> devices;
    instance->EnumeratePhysicalDevices(devices);
    for (size_t i = 0; i < devices.size(); ++i) {
      VkPhysicalDeviceProperties properties{};
      instance->functions().vkGetPhysicalDeviceProperties(devices[i], &properties);
      result.vulkan.push_back(
          Choice{std::to_string(i), fmt::format("{}: {}", i, properties.deviceName), ""});
    }
#ifdef _WIN32
    // AMD FSR versions per GPU (the launcher offers FSR 4 only where it is):
    // AMD's loader asks the device, so each GPU gets a logical device briefly.
    std::error_code ec;
    if (std::filesystem::exists(
            rex::filesystem::GetExecutableFolder() / "amd_fidelityfx_loader_vk.dll", ec)) {
      result.fsr.resize(devices.size());
      for (size_t i = 0; i < devices.size(); ++i) {
        auto device = rex::ui::vulkan::VulkanDevice::CreateIfSupported(instance.get(), devices[i],
                                                                        false, false);
        std::string reason;
        if (!device || !rex::ui::vulkan::QueryFsrVersions(*device, result.fsr[i], reason)) {
          REXLOG_WARN("Launcher: AMD FSR versions of device {} unknown: {}", i,
                      device ? reason : std::string("no logical device"));
          continue;
        }
        auto join = [](const std::vector<std::string>& names) {
          return names.empty() ? std::string("none") : fmt::format("{}", fmt::join(names, ", "));
        };
        REXLOG_INFO("Launcher: AMD FSR on device {}: upscaler {}; frame generation {}; swapchain {}", i,
                    join(result.fsr[i].upscaler), join(result.fsr[i].frame_generation),
                    join(result.fsr[i].swapchain));
      }
    }
    // Intel XeSS SR per GPU (the launcher offers it only where it runs): its
    // requirements query fails where the device or driver can't.
    if (std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "libxess.dll", ec)) {
      result.xess.resize(devices.size());
      for (size_t i = 0; i < devices.size(); ++i) {
        std::vector<const char*> extensions;
        const void* features = nullptr;
        std::string reason = instance->xess_note();
        result.xess[i] = instance->xess_ready() &&
                         rex::ui::vulkan::XessDeviceRequirements(instance->instance(), devices[i], extensions,
                                                                 features, reason);
        if (result.xess[i]) {
          REXLOG_INFO("Launcher: Intel XeSS {} on device {}: available", rex::ui::vulkan::XessVersion(), i);
        } else {
          REXLOG_INFO("Launcher: Intel XeSS on device {}: unavailable ({})", i, reason);
        }
      }
    }
    // NVIDIA DLSS SR and DLSS Frame Generation per GPU: NGX's requirements
    // (GeForce RTX, driver, OS) for the physical device.
    namespace ngx = rex::ui::vulkan::ngx;
    for (const ngx::Feature feature : {ngx::Feature::kSuperResolution, ngx::Feature::kFrameGeneration}) {
      if (!ngx::RuntimePresent(feature)) continue;
      auto& runs = feature == ngx::Feature::kSuperResolution ? result.dlss : result.dlss_frame_generation;
      runs.resize(devices.size());
      for (size_t i = 0; i < devices.size(); ++i) {
        std::vector<const char*> extensions;
        ngx::DeviceFeatures features;
        std::string reason = instance->ngx_note();
        runs[i] = instance->ngx_ready() &&
                  ngx::DeviceRequirements(*instance, devices[i], feature, extensions, features, reason);
        REXLOG_INFO("Launcher: NVIDIA {} {} on device {}: {}", ngx::FeatureName(feature),
                    ngx::RuntimeVersion(feature), i, runs[i] ? std::string("available") : reason);
      }
    }
#endif
  } else {
    REXLOG_WARN("Launcher: Vulkan rendering devices could not be listed");
  }
#endif

#ifdef _WIN32
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    for (UINT i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, &adapter) != S_OK) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc{};
      if (FAILED(adapter->GetDesc1(&desc))) {
        continue;
      }
      // Windows WCHAR is UTF-16, as in the SDK's Win32 path conversions.
      const std::string name = rex::string::to_utf8(
          std::u16string_view(reinterpret_cast<const char16_t*>(desc.Description)));
      result.dxgi.push_back(Choice{std::to_string(i), fmt::format("{}: {}", i, name), ""});
    }
  } else {
    REXLOG_WARN("Launcher: Direct3D rendering devices could not be listed");
  }
  result.dxgi.push_back(Choice{"-2", "Software (WARP)", ""});
#endif
  return result;
}

}  // namespace rex::launcher
