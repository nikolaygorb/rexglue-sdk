// NVIDIA NGX on Vulkan (DLSS SDK 310.9.16, cmake/rexglue_dlss_sdk.cmake): DLSS
// Super Resolution / DLAA (the native temporal upscaler) and DLSS Frame
// Generation (the Vulkan presenter). The NGX static library is linked into
// rexruntime only (one NGX per process); GPU plugins and the launcher reach it
// through these functions, without NGX headers. NGX is not thread safe: every
// call here takes one process-wide lock. The feature DLLs (nvngx_dlss.dll,
// nvngx_dlssg.dll) load from next to the executable only. NGX's project ID is
// derived from the executable's name (no title input). Title-agnostic.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
class VulkanInstance;

namespace ngx {

enum class Feature { kSuperResolution, kFrameGeneration };
const char* FeatureName(Feature feature);  // "DLSS", "DLSS Frame Generation"

// Built with the SDK (Windows x64) and the feature's DLL next to the
// executable; else why not.
bool RuntimePresent(Feature feature, std::string* reason = nullptr);
// "310.9.16"-style file version of the feature's DLL (empty without it).
std::string RuntimeVersion(Feature feature);

// Before vkCreateInstance: the instance extensions of the features whose DLLs
// are there. False (reason): NGX unavailable (no NVIDIA driver, no DLLs).
bool InstanceRequirements(std::vector<const char*>& extensions, std::string& reason);

// The device features NGX's extensions need (only those the GPU supports),
// as a chain for VulkanDevice's feature merge.
struct DeviceFeatures {
  VkPhysicalDeviceFeatures2 head{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceSynchronization2Features synchronization2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
  const void* chain() const { return &head; }
};
// Before vkCreateDevice: whether NGX supports the feature on this GPU and
// driver (GetFeatureRequirements), its device extensions (all supported by
// the device, names valid for the process) and features (into `features`,
// linked). False (reason): not on this device.
bool DeviceRequirements(const VulkanInstance& instance, VkPhysicalDevice physical_device, Feature feature,
                        std::vector<const char*>& extensions, DeviceFeatures& features, std::string& reason);

// Once per device (any thread; again: true at once): NGX initialised on it.
// The device must carry the requirements above.
bool Initialize(const VulkanDevice& device, std::string& reason);
// Before vkDestroyDevice: releases every feature and NGX's state of the device.
void Shutdown(VkDevice device);
// After Initialize: the feature runs on this device (driver version etc.).
bool Available(const VulkanDevice& device, Feature feature, std::string& reason);

// An image given to NGX: whole view, one mip and layer, color aspect.
struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  bool storage = false;  // Written by NGX (VK_IMAGE_USAGE_STORAGE_BIT).
};

// A created feature instance (its own parameter map); released after the GPU
// finished every command buffer that used it.
struct Handle;
void Release(Handle* handle);

// DLSS Super Resolution. Modes by output/input ratio (per side): DLAA 1,
// Ultra Quality ~1.3, Quality 1.5, Balanced ~1.72, Performance 2, Ultra
// Performance 3.
enum class SrMode { kDlaa, kUltraQuality, kQuality, kBalanced, kPerformance, kUltraPerformance };
const char* SrModeName(SrMode mode);
// The input sizes the mode takes for this output (dynamic range included).
struct SrRange {
  uint32_t optimal_width = 0, optimal_height = 0;
  uint32_t min_width = 0, min_height = 0, max_width = 0, max_height = 0;
};
bool SrInputRange(const VulkanDevice& device, SrMode mode, uint32_t output_width, uint32_t output_height,
                  SrRange& range);
struct SrCreateInfo {
  uint32_t render_width = 0, render_height = 0;  // The largest input.
  uint32_t output_width = 0, output_height = 0;
  SrMode mode = SrMode::kDlaa;
  bool hdr = false, depth_inverted = false, auto_exposure = false;
  uint32_t preset = 0;  // NVSDK_NGX_DLSS_Hint_Render_Preset: 0 default, 10 J, 11 K, 12 L, 13 M.
};
struct SrEvaluateInfo {
  Image color, depth, motion, output;
  uint32_t render_width = 0, render_height = 0;  // This frame's input (from the images' origin).
  float jitter_x = 0, jitter_y = 0;              // Input pixels, NGX's convention.
  float motion_scale_x = 1, motion_scale_y = 1;  // To input pixels.
  bool reset = false;
  float delta_ms = 0;
};
// Recorded into `command_buffer` (open, outside a render pass; NGX changes its
// bindings). Null / false: `error`.
Handle* SrCreate(const VulkanDevice& device, VkCommandBuffer command_buffer, const SrCreateInfo& info,
                 std::string& error);
bool SrEvaluate(VkCommandBuffer command_buffer, Handle* handle, const SrEvaluateInfo& info, std::string& error);

// DLSS Frame Generation: one interpolated frame between the previous and this
// backbuffer (the application presents both, paced; ngx does no presenting).
struct FgCreateInfo {
  uint32_t width = 0, height = 0;  // The backbuffer rectangle's size.
  VkFormat backbuffer_format = VK_FORMAT_UNDEFINED;
  uint32_t render_width = 0, render_height = 0;  // Depth and motion.
};
using Matrix = std::array<float, 16>;  // Row-major, column vectors (clip = M * view).
struct FgEvaluateInfo {
  Image backbuffer, depth, motion, hudless, output;  // hudless optional (no image).
  // The frame's rectangle in the backbuffer, the hudless image and the output.
  int32_t rect_x = 0, rect_y = 0;
  uint32_t rect_width = 0, rect_height = 0;
  // The scene in the depth and motion images (aligned with the rectangle above).
  uint32_t render_x = 0, render_y = 0, render_width = 0, render_height = 0;
  float motion_scale_x = 1, motion_scale_y = 1;  // To motion-image pixels.
  float jitter_x = 0, jitter_y = 0;  // Clip space (NGX's DLSS-G convention).
  bool reset = false, depth_inverted = false;
  // On NGX's linearized depth (1 / (1 - depth), or 1 / depth inverted), to
  // its heuristics' units (NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale).
  float depth_scale = 1.f;
  Matrix view_to_clip{}, clip_to_view{}, clip_to_previous_clip{}, previous_clip_to_clip{};
  std::array<float, 3> position{}, up{}, right{}, forward{};
  float near_distance = 0, far_distance = 0, fov_y = 0, aspect = 0;
  uint64_t frame_id = 0;  // +1 per presented real frame.
};
Handle* FgCreate(const VulkanDevice& device, VkCommandBuffer command_buffer, const FgCreateInfo& info,
                 std::string& error);
bool FgEvaluate(VkCommandBuffer command_buffer, Handle* handle, const FgEvaluateInfo& info, std::string& error);

// An NVSDK_NGX_Result as text ("feature not supported (0xbad00001)").
std::string ResultText(uint32_t result);

}  // namespace ngx
}  // namespace rex::ui::vulkan
