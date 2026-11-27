// AMD FSR frame generation for the Vulkan presenter: AMD's signed ffx-api DLLs
// (cmake/rexglue_fsr_sdk.cmake), loaded at run time. Their proxy swapchain
// paces the real and the generated frames; the presenter gives it, with each
// real frame, the scene inputs a GPU plugin published with the guest output
// (depth, motion, the frame without the HUD, camera). Title-agnostic: without
// such inputs a frame is presented as it is.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>

REXCVAR_DECLARE(std::string, native_frame_generation);
REXCVAR_DECLARE(int32_t, native_frame_generation_debug);
REXCVAR_DECLARE(bool, native_frame_generation_debug_checking);
REXCVAR_DECLARE(bool, native_frame_generation_async);

namespace rex::ui::vulkan {
class VulkanDevice;

class VulkanFrameGeneration {
 public:
  // native_frame_generation asks for AMD FSR (not off, not dlss).
  static bool Requested();
  // One line for the launcher ("Last run: ..."): logs/frame_generation.log next
  // to the executable; written only when the text changes.
  static void WriteStatus(const VulkanDevice& device, const std::string& text);
  // Loads the DLLs. Null when unavailable; `reason` says why.
  static std::unique_ptr<VulkanFrameGeneration> Create(VulkanDevice& device, std::string& reason);
  ~VulkanFrameGeneration();

  // The proxy swapchain in place of vkCreateSwapchainKHR (info.oldSwapchain
  // must be null; destroy the old one first). False: not for this surface
  // (reason set); the caller creates a regular swapchain.
  bool CreateSwapchain(VkSurfaceKHR surface, const VkSwapchainCreateInfoKHR& info,
                       VkSwapchainKHR& swapchain_out, std::string& reason);
  // Waits until the proxy's queued presents (and their generation work) are done.
  void WaitForPresents();
  // Waits for its presents, then destroys the frame generation context and the swapchain.
  void DestroySwapchain(VkSwapchainKHR swapchain);
  // The swapchain functions for a proxy swapchain.
  VkResult GetSwapchainImages(VkSwapchainKHR swapchain, uint32_t* count, VkImage* images);
  VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                            uint32_t* image_index);
  // Not under the game queue's lock: the proxy submits through it itself.
  VkResult Present(const VkPresentInfoKHR& info);

  // One real frame. Images in SHADER_READ_ONLY_OPTIMAL. A frame without the
  // title's inputs while generation is on: neutral depth and motion, no
  // hudless image, reset (generation stays on; switching costs a wait).
  struct Frame {
    bool generate = false;  // Inputs valid: generate a frame before this one.
    bool reset = false;     // History discontinuity (cut, new stream).
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;  // The frame's, before its present.
    // Depth (guest clip z/w) and motion (xy: current -> previous input pixels,
    // without jitter), covering the whole guest output.
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED, motion_format = VK_FORMAT_UNDEFINED;
    uint32_t render_width = 0, render_height = 0;
    // The frame without the HUD as painted to the backbuffer (its size and format).
    VkImage hudless = VK_NULL_HANDLE;
    VkFormat backbuffer_format = VK_FORMAT_UNDEFINED;
    uint32_t display_width = 0, display_height = 0;
    // Where the guest output is in the backbuffer (letterbox).
    int32_t rect_x = 0, rect_y = 0;
    uint32_t rect_width = 0, rect_height = 0;
    float jitter_x = 0, jitter_y = 0;  // Input pixels.
    float delta_ms = 0;
    float camera_near = 0, camera_far = 0, fov_y = 0, meters_per_unit = 0;
    bool depth_inverted = false, depth_infinite = false;
    std::array<float, 3> position{}, up{}, right{}, forward{};
  };
  // Records Prepare into frame.command_buffer (when generating) and configures
  // the next Present: call while the command buffer is recording. False: this
  // frame is presented without generation (last_error()).
  bool BeginPresent(VkSwapchainKHR swapchain, const Frame& frame);

  const std::string& version() const { return version_; }
  const std::string& last_error() const { return last_error_; }
  uint64_t generating_frames() const { return generating_frames_; }
  // Generation is on in the proxy (the last configuration).
  bool enabled() const { return enabled_; }
  // Generation runs on its own compute queue.
  bool async() const { return async_family_ != UINT32_MAX; }
  // Times generation was switched on or off (each costs the proxy a wait).
  uint64_t enable_switches() const { return enable_switches_; }
  // Presents since the swapchain was created, generated ones included.
  uint64_t PresentCount(VkSwapchainKHR swapchain) const;

 private:
  VulkanFrameGeneration() = default;
  bool PickQueues(VkSurfaceKHR surface, std::string& reason);
  bool EnsureContext(VkSwapchainKHR swapchain, const Frame& frame);
  void DestroyContext(VkSwapchainKHR swapchain);
  bool Configure(VkSwapchainKHR swapchain, bool enabled, const Frame* frame);

  VulkanDevice* device_ = nullptr;
  void* module_ = nullptr;
  struct Functions;
  std::unique_ptr<Functions> functions_;
  void* swapchain_context_ = nullptr;  // ffxContext of the proxy swapchain.
  void* context_ = nullptr;            // ffxContext of frame generation.
  uint32_t context_display_width_ = 0, context_display_height_ = 0;
  uint32_t context_render_width_ = 0, context_render_height_ = 0;
  uint32_t context_flags_ = 0;
  VkFormat context_format_ = VK_FORMAT_UNDEFINED;
  uint32_t flag_mismatch_frames_ = 0;
  bool context_failed_ = false;
  bool legacy_prepare_ = false;  // The provider takes only the FSR 3.1 Prepare.
  uint32_t present_family_ = UINT32_MAX, present_index_ = 0;
  uint32_t acquire_family_ = UINT32_MAX, acquire_index_ = 0;
  // Generation on a compute queue of its own, overlapping the next frame
  // (native_frame_generation_async, a fourth queue): UINT32_MAX without.
  uint32_t async_family_ = UINT32_MAX, async_index_ = 0;
  uint64_t frame_id_ = 1;
  uint64_t generating_frames_ = 0;
  bool enabled_ = false;
  uint64_t enable_switches_ = 0;
  std::string version_, last_error_;
  // Providers on this device, newest first: generation and its swapchain.
  std::vector<uint64_t> version_ids_, swapchain_ids_;
  std::vector<std::string> version_names_, swapchain_names_;
  int major_ = 3;  // fsr3 / fsr4 (native_frame_generation), if the GPU offers it.
  std::string fallback_note_;  // For the status line: why not FSR 4.
};

}  // namespace rex::ui::vulkan
