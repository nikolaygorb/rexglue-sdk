// NVIDIA DLSS Frame Generation for the Vulkan presenter (native_frame_generation
// = dlss; ui/vulkan/ngx.h, cmake/rexglue_dlss_sdk.cmake; Windows x64, GeForce
// RTX 40+). NGX generates a frame between the previous real frame and this one
// from the final frame, the frame without the HUD and the title's depth and
// motion (the inputs AMD FSR frame generation takes). NGX doesn't present: on
// the regular swapchain the presenter shows the generated frame at once and the
// real one, held, about half a real frame later, from a presenting thread of
// this class (FIFO shows them on consecutive vblanks). Title-agnostic: without
// the title's inputs a frame is presented as it is.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>

REXCVAR_DECLARE(bool, native_dlss_frame_generation_hudless);
REXCVAR_DECLARE(double, native_dlss_frame_generation_depth_scale);

namespace rex::ui::vulkan {
class VulkanDevice;
namespace ngx {
struct Handle;
}

class VulkanDlssFrameGeneration {
 public:
  // native_frame_generation is dlss.
  static bool Requested();
  // NGX ready on the device for frame generation; null: `reason`.
  static std::unique_ptr<VulkanDlssFrameGeneration> Create(VulkanDevice& device, std::string& reason);
  ~VulkanDlssFrameGeneration();

  // What the swapchain needs: the copies in and out of its images, and two of
  // them acquired at once (the real and the generated frame).
  static constexpr VkImageUsageFlags kSwapchainUsage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  static constexpr uint32_t kExtraSwapchainImages = 2;

  // A new swapchain (after DetachSwapchain): false (reason) if frames can't be
  // generated on it (images, usage); frames are then shown as they are.
  bool AttachSwapchain(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent, uint32_t image_count,
                       uint32_t surface_min_image_count, VkImageUsageFlags usage, bool fifo,
                       uint32_t present_queue_family, size_t paint_submission_count, std::string& reason);
  // Before the swapchain goes, with the presenter's GPU work done: the held
  // present finished, the feature and images released.
  void DetachSwapchain();
  bool attached() const { return swapchain_ != VK_NULL_HANDLE; }

  // The real frame's generation inputs (images in SHADER_READ_ONLY_OPTIMAL).
  struct Frame {
    // Depth (guest clip z/w) and motion (xy: current -> previous pixels of these
    // images, without jitter), covering the guest output.
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE, motion_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED, motion_format = VK_FORMAT_UNDEFINED;
    uint32_t render_width = 0, render_height = 0;
    // The frame without the HUD as painted to the backbuffer (swapchain size and format).
    VkImage hudless = VK_NULL_HANDLE;
    VkImageView hudless_view = VK_NULL_HANDLE;
    // Where the guest output is in the backbuffer (the depth and motion images cover it).
    int32_t rect_x = 0, rect_y = 0;
    uint32_t rect_width = 0, rect_height = 0;
    float jitter_x = 0, jitter_y = 0;  // Depth/motion pixels.
    bool reset = false, depth_inverted = false;
    // The producer frame, and the one its motion refers to (0: none): history
    // continues when that is the frame generated from last, frames without
    // inputs in between (a scene drawn every other frame) or not.
    uint64_t frame = 0, previous_frame = 0;
    std::array<float, 16> view_to_clip{}, clip_to_view{}, clip_to_previous_clip{}, previous_clip_to_clip{};
    std::array<float, 3> position{}, up{}, right{}, forward{};
    float camera_near = 0, camera_far = 0, fov_y = 0, aspect = 0;
    float meters_per_unit = 0;  // 0: unknown (1).
  };

  // The feature for this frame's sizes exists, or must be (re)created: a size
  // change first needs the presenter's GPU work done (then ReleaseFeature).
  bool FeatureMatches(const Frame& frame) const;
  void ReleaseFeature();

  // The second image's acquire semaphore for a paint submission slot.
  VkSemaphore acquire_semaphore(size_t submission_slot) const {
    return acquire_semaphores_.empty() ? VK_NULL_HANDLE
                                       : acquire_semaphores_[submission_slot % acquire_semaphores_.size()];
  }

  // After the frame's swapchain pass (the real image in PRESENT_SRC_KHR, the
  // generated one acquired, its semaphore awaited by the submission at the
  // transfer stage): records the copy of the real frame, the generation and
  // the generated frame's copy into `generated` (left in PRESENT_SRC_KHR; the
  // real frame itself on failure). True: a frame was generated.
  bool Record(VkCommandBuffer command_buffer, const Frame& frame, VkImage real, VkImage generated);
  // A real frame presented without generation (no inputs). The history
  // continues if the next frame's motion refers to the last generated one.
  void NoteFrameWithoutGeneration();

  // The real image's present, held about half a real frame after the
  // generated one (that the presenter just presented): on the presenting thread.
  void ScheduleRealPresent(uint32_t image_index, VkSemaphore wait_semaphore);
  // Waits for the held present (before acquiring again, before the swapchain
  // goes); its result (VK_SUCCESS without one).
  VkResult WaitForHeldPresent();

  const std::string& version() const { return version_; }
  const std::string& last_error() const { return last_error_; }
  uint64_t generated_frames() const { return generated_frames_; }
  // Generated frames that started a new history (copies of the real frame).
  uint64_t resets() const { return resets_; }
  // By reason (totals): the producer's reset, after a failed evaluation, no
  // previous frame, the motion's previous frame not the one NGX had (a real
  // frame between went without generation).
  std::array<uint64_t, 4> reset_reasons() const { return reset_reasons_; }
  uint64_t held_presents() const { return held_presents_; }
  // A frame was generated recently (the refresher holds the real frame rate).
  bool generating() const { return generating_; }

 private:
  VulkanDlssFrameGeneration() = default;
  bool CreateImage(VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                   VkImageView& view);
  void DestroyImages();
  void PresentLoop();

  VulkanDevice* device_ = nullptr;
  std::string version_, last_error_;

  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkFormat format_ = VK_FORMAT_UNDEFINED;  // The copies' (storage) format.
  VkExtent2D extent_{};
  bool fifo_ = true;
  uint32_t present_queue_family_ = UINT32_MAX;
  std::vector<VkSemaphore> acquire_semaphores_;
  // The real frame (NGX's backbuffer input) and the generated one.
  VkImage backbuffer_ = VK_NULL_HANDLE, output_ = VK_NULL_HANDLE;
  VkDeviceMemory backbuffer_memory_ = VK_NULL_HANDLE, output_memory_ = VK_NULL_HANDLE;
  VkImageView backbuffer_view_ = VK_NULL_HANDLE, output_view_ = VK_NULL_HANDLE;

  ngx::Handle* feature_ = nullptr;
  uint32_t feature_width_ = 0, feature_height_ = 0, feature_render_width_ = 0, feature_render_height_ = 0;
  bool feature_failed_ = false;
  uint64_t frame_id_ = 1;  // +1 per backbuffer given to NGX (consecutive: one history).
  uint64_t last_frame_ = 0;  // The producer frame of that backbuffer.
  bool reset_next_ = true;
  uint64_t generated_frames_ = 0, held_presents_ = 0, resets_ = 0;
  std::array<uint64_t, 4> reset_reasons_{};
  uint32_t logged_gaps_ = 0;
  bool generating_ = false;
  float logged_depth_scale_ = 0;

  // The presenting thread: one held present at a time.
  std::thread present_thread_;
  std::mutex present_mutex_;
  std::condition_variable present_condition_;
  bool present_pending_ = false, present_stop_ = false;
  VkSwapchainKHR present_swapchain_ = VK_NULL_HANDLE;
  uint32_t present_index_ = 0;
  VkSemaphore present_semaphore_ = VK_NULL_HANDLE;
  std::chrono::steady_clock::time_point present_due_{};
  VkResult present_result_ = VK_SUCCESS;
  // Pacing: the real frames' interval (smoothed).
  std::chrono::steady_clock::time_point last_schedule_{};
  double interval_seconds_ = 0;
};

}  // namespace rex::ui::vulkan
