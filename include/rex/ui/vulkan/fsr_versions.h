// AMD FSR provider versions on a GPU: which major version (3 or 4) it offers
// and the order to try them in. Shared by the native renderer's upscaler, the
// presenter's frame generation and the launcher (FSR 4 shown only where the
// GPU offers it). Title-agnostic.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui::vulkan {
class VulkanDevice;

// The major number of an ffx-api version name ("3.1.6" -> 3); 0 without one.
inline int FsrMajor(std::string_view name) {
  size_t i = 0;
  while (i < name.size() && (name[i] < '0' || name[i] > '9')) ++i;
  int major = 0;
  for (; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i) major = major * 10 + (name[i] - '0');
  return major;
}

inline bool FsrHasMajor(const std::vector<std::string>& names, int major) {
  for (const std::string& name : names)
    if (FsrMajor(name) == major) return true;
  return false;
}

// Indices of `names` (as listed: newest first) to try for the wanted major
// version: its own, then the older ones down to 3 (FSR 4 -> FSR 3.1).
inline std::vector<size_t> FsrVersionOrder(const std::vector<std::string>& names, int wanted) {
  std::vector<size_t> order;
  for (int major = wanted; major >= 3; --major)
    for (size_t i = 0; i < names.size(); ++i)
      if (FsrMajor(names[i]) == major) order.push_back(i);
  return order;
}

// The providers AMD's loader (amd_fidelityfx_loader_vk.dll next to the
// executable) offers for this device, newest first; the effect DLLs that are
// missing list nothing. False (reason set): no loader, or not this platform.
struct FsrVersions {
  std::vector<std::string> upscaler, frame_generation, swapchain;
};
bool QueryFsrVersions(const VulkanDevice& device, FsrVersions& out, std::string& reason);

}  // namespace rex::ui::vulkan
