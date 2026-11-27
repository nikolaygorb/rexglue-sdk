/**
 * @file        graphics/native/plugin_main.cpp
 * @brief       rexgpu-native plugin entry points
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string_view>

#include <rex/graphics/native/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-native: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-native: invalid GpuCreateInfo");
    return nullptr;
  }
  // Always Vulkan. A title passes its graphics_backend setting on, which may
  // name an emulation backend; that is for rexgpu-xenos.
  std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "vulkan") {
    REXLOG_WARN("rexgpu-native: draws with Vulkan, backend '{}' is ignored", backend);
  }
  return new rex::graphics::native::NativeGraphicsSystem();
}
