// Intel XeSS Super Resolution on Vulkan: libxess.dll next to the executable
// (cmake/rexglue_xess_sdk.cmake, Windows), loaded once per process. XeSS's
// instance and device extensions and features must be enabled when those
// Vulkan objects are created, so VulkanInstance and VulkanDevice (GPU
// emulation devices) add them whenever the DLL is there: the native temporal
// upscaler can then switch to XeSS while the game runs. Shared with the
// launcher (XeSS offered only for a GPU that can run it). Title-agnostic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {

// The loaded libxess.dll (an HMODULE), or null with `reason` (no DLL, platform,
// missing Visual C++ runtime, entry points).
void* XessModule(std::string* reason = nullptr);

// "3.0.2"-style version of the loaded runtime (empty without it).
std::string XessVersion();

// Before vkCreateInstance: the extensions XeSS needs and the lowest
// VkApplicationInfo::apiVersion it works with. False (reason) when unavailable.
bool XessInstanceRequirements(std::vector<const char*>& extensions, uint32_t& api_version,
                              std::string& reason);

// Before vkCreateDevice: the device extensions and a feature chain owned by
// XeSS (VkPhysicalDeviceFeatures2 and further structures, to be merged into
// the device's own chain). False (reason): this GPU/driver can't run XeSS.
bool XessDeviceRequirements(VkInstance instance, VkPhysicalDevice physical_device,
                            std::vector<const char*>& extensions, const void*& features,
                            std::string& reason);

// An xess_result_t as text ("unsupported device (-1)").
std::string XessResultText(int result);

}  // namespace rex::ui::vulkan
