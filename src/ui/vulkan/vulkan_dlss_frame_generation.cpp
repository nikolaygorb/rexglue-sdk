#include <rex/ui/vulkan/dlss_frame_generation.h>

#include <algorithm>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/frame_generation.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/ngx.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(native_dlss_frame_generation_hudless, true, "GPU",
    "DLSS frame generation gets the frame without the HUD (it keeps the HUD out of the interpolation). Off: "
    "only the final frame, for a title whose frame without the HUD differs from the final one outside the HUD "
    "(fades, letterbox drawn after it), which shows as flicker in generated frames.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(native_dlss_frame_generation_depth_scale, 0.0, "GPU",
    "DLSS frame generation's scale on its linearized depth (distance / near; its near/far and object "
    "separation heuristics): 0 automatic (near plane in meters / 0.2 mm, from the title's meters per "
    "unit), else the value (NGX default 1). Too small shows as z-fighting-like shimmer in generated "
    "frames.")
    .range(0.0, 10000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui::vulkan {

bool VulkanDlssFrameGeneration::Requested() { return REXCVAR_GET(native_frame_generation) == "dlss"; }

std::unique_ptr<VulkanDlssFrameGeneration> VulkanDlssFrameGeneration::Create(VulkanDevice& device,
                                                                             std::string& reason) {
  if (!ngx::RuntimePresent(ngx::Feature::kFrameGeneration, &reason)) return nullptr;
  if (!device.ngx_fg_ready()) {
    reason = "the Vulkan device lacks its requirements: " +
             (device.ngx_fg_note().empty() ? std::string("unknown") : device.ngx_fg_note());
    return nullptr;
  }
  if (!ngx::Initialize(device, reason) || !ngx::Available(device, ngx::Feature::kFrameGeneration, reason))
    return nullptr;
  std::unique_ptr<VulkanDlssFrameGeneration> generation(new VulkanDlssFrameGeneration());
  generation->device_ = &device;
  const std::string version = ngx::RuntimeVersion(ngx::Feature::kFrameGeneration);
  generation->version_ = version.empty() ? std::string("(version unknown)") : version;
  generation->present_thread_ = std::thread([g = generation.get()] { g->PresentLoop(); });
  REXLOG_INFO("DLSS frame generation: {} available on '{}'", generation->version_,
              device.properties().deviceName);
  return generation;
}

VulkanDlssFrameGeneration::~VulkanDlssFrameGeneration() {
  WaitForHeldPresent();
  {
    std::lock_guard lock(present_mutex_);
    present_stop_ = true;
  }
  present_condition_.notify_all();
  if (present_thread_.joinable()) present_thread_.join();
  // The presenter detached the swapchain (its GPU work done) before.
  ReleaseFeature();
  DestroyImages();
  const auto& dfn = device_->functions();
  for (VkSemaphore semaphore : acquire_semaphores_) dfn.vkDestroySemaphore(device_->device(), semaphore, nullptr);
}

bool VulkanDlssFrameGeneration::CreateImage(VkFormat format, VkImageUsageFlags usage, VkImage& image,
                                            VkDeviceMemory& memory, VkImageView& view) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {extent_.width, extent_.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(device_, info, util::MemoryPurpose::kDeviceLocal, image, memory))
    return false;
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr, &view) == VK_SUCCESS;
}

void VulkanDlssFrameGeneration::DestroyImages() {
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, backbuffer_view_);
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, backbuffer_);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, backbuffer_memory_);
  util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, output_view_);
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, output_);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, output_memory_);
}

bool VulkanDlssFrameGeneration::AttachSwapchain(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent,
                                                uint32_t image_count, uint32_t surface_min_image_count,
                                                VkImageUsageFlags usage, bool fifo, uint32_t present_queue_family,
                                                size_t paint_submission_count, std::string& reason) {
  DetachSwapchain();
  // Two images held at once (the generated one acquired while the real one is).
  if (image_count < surface_min_image_count + 1) {
    reason = fmt::format("{} swapchain images, {} needed", image_count, surface_min_image_count + 1);
    return false;
  }
  if ((usage & kSwapchainUsage) != kSwapchainUsage) {
    reason = "the swapchain images can't be copied";
    return false;
  }
  const auto& ifn = device_->vulkan_instance()->functions();
  const auto& dfn = device_->functions();
  // NGX writes the generated frame as a storage image: the swapchain's format
  // where it can be one, else RGBA8 (blits convert).
  VkFormatProperties properties{};
  ifn.vkGetPhysicalDeviceFormatProperties(device_->physical_device(), format, &properties);
  const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  format_ = (properties.optimalTilingFeatures & needed) == needed ? format : VK_FORMAT_R8G8B8A8_UNORM;
  swapchain_format_ = format;
  extent_ = extent;
  const VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  if (!CreateImage(format_, image_usage, backbuffer_, backbuffer_memory_, backbuffer_view_) ||
      !CreateImage(format_, image_usage, output_, output_memory_, output_view_)) {
    DestroyImages();
    reason = "images unavailable";
    return false;
  }
  if (acquire_semaphores_.size() < paint_submission_count) {
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    while (acquire_semaphores_.size() < paint_submission_count) {
      VkSemaphore semaphore = VK_NULL_HANDLE;
      if (dfn.vkCreateSemaphore(device_->device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
        DestroyImages();
        reason = "semaphores unavailable";
        return false;
      }
      acquire_semaphores_.push_back(semaphore);
    }
  }
  swapchain_ = swapchain;
  fifo_ = fifo;
  present_queue_family_ = present_queue_family;
  reset_next_ = true;
  feature_failed_ = false;  // Another size or format may create.
  last_schedule_ = {};
  interval_seconds_ = 0;
  REXLOG_INFO("DLSS frame generation: swapchain {}x{}, {} images, format {} (generated in {}), {}", extent.width,
              extent.height, image_count, uint32_t(format), uint32_t(format_),
              fifo ? "FIFO (vblank pacing)" : "held real present (timed pacing)");
  return true;
}

void VulkanDlssFrameGeneration::DetachSwapchain() {
  WaitForHeldPresent();
  ReleaseFeature();
  DestroyImages();
  swapchain_ = VK_NULL_HANDLE;
  generating_ = false;
}

bool VulkanDlssFrameGeneration::FeatureMatches(const Frame& frame) const {
  return !feature_ || (feature_width_ == frame.rect_width && feature_height_ == frame.rect_height &&
                       feature_render_width_ == frame.render_width && feature_render_height_ == frame.render_height);
}

void VulkanDlssFrameGeneration::ReleaseFeature() {
  if (feature_) ngx::Release(feature_);
  feature_ = nullptr;
  feature_width_ = feature_height_ = feature_render_width_ = feature_render_height_ = 0;
  reset_next_ = true;
}

void VulkanDlssFrameGeneration::NoteFrameWithoutGeneration() { generating_ = false; }

namespace {
void Barrier(const VulkanDevice::Functions& dfn, VkCommandBuffer command_buffer, VkImage image,
             VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkImageLayout old_layout,
             VkPipelineStageFlags dst_stage, VkAccessFlags dst_access, VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Whole image to whole image (same size): a copy, or a blit between formats.
void Copy(const VulkanDevice::Functions& dfn, VkCommandBuffer command_buffer, VkImage from, VkFormat from_format,
          VkImage to, VkFormat to_format, VkExtent2D extent) {
  const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (from_format == to_format) {
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = layers;
    region.extent = {extent.width, extent.height, 1};
    dfn.vkCmdCopyImage(command_buffer, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return;
  }
  VkImageBlit region{};
  region.srcSubresource = region.dstSubresource = layers;
  region.srcOffsets[1] = region.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
  dfn.vkCmdBlitImage(command_buffer, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
}
}  // namespace

bool VulkanDlssFrameGeneration::Record(VkCommandBuffer command_buffer, const Frame& frame, VkImage real,
                                       VkImage generated) {
  const auto& dfn = device_->functions();
  constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
  // The real frame (its pass ended, PRESENT_SRC_KHR) into NGX's backbuffer.
  Barrier(dfn, command_buffer, real, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, kTransfer,
          VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Barrier(dfn, command_buffer, backbuffer_, kCompute, 0, VK_IMAGE_LAYOUT_UNDEFINED, kTransfer,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Copy(dfn, command_buffer, real, swapchain_format_, backbuffer_, format_, extent_);
  Barrier(dfn, command_buffer, backbuffer_, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kCompute, VK_ACCESS_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Barrier(dfn, command_buffer, output_, kTransfer, 0, VK_IMAGE_LAYOUT_UNDEFINED, kCompute,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
  // The inputs painted or written before (the frame without the HUD, depth, motion).
  VkMemoryBarrier inputs{};
  inputs.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  inputs.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  inputs.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           kCompute, 0, 1, &inputs, 0, nullptr, 0, nullptr);

  bool generated_frame = false;
  const bool inputs_ok = frame.depth && frame.motion && frame.render_width && frame.render_height &&
                         frame.rect_width && frame.rect_height;
  if (!feature_ && !feature_failed_ && inputs_ok) {
    ngx::FgCreateInfo create;
    create.width = frame.rect_width;
    create.height = frame.rect_height;
    create.backbuffer_format = format_;
    create.render_width = frame.render_width;
    create.render_height = frame.render_height;
    feature_ = ngx::FgCreate(*device_, command_buffer, create, last_error_);
    if (feature_) {
      feature_width_ = frame.rect_width; feature_height_ = frame.rect_height;
      feature_render_width_ = frame.render_width; feature_render_height_ = frame.render_height;
      reset_next_ = true;
      REXLOG_INFO("DLSS frame generation: feature {}x{} at {},{} of {}x{}, depth/motion {}x{}", frame.rect_width,
                  frame.rect_height, frame.rect_x, frame.rect_y, extent_.width, extent_.height, frame.render_width,
                  frame.render_height);
      VulkanFrameGeneration::WriteStatus(*device_, "NVIDIA DLSS Frame Generation " + version_);
    } else {
      feature_failed_ = true;  // Not again each frame.
      REXLOG_ERROR("DLSS frame generation: {}", last_error_);
      VulkanFrameGeneration::WriteStatus(*device_, "NVIDIA DLSS Frame Generation failed (" + last_error_ + ")");
    }
  }
  if (feature_ && inputs_ok) {
    auto image = [](VkImage i, VkImageView v, VkFormat f, uint32_t w, uint32_t h, bool storage) {
      ngx::Image n;
      n.image = i; n.view = v; n.format = f; n.width = w; n.height = h; n.storage = storage;
      return n;
    };
    ngx::FgEvaluateInfo info;
    info.backbuffer = image(backbuffer_, backbuffer_view_, format_, extent_.width, extent_.height, false);
    info.output = image(output_, output_view_, format_, extent_.width, extent_.height, true);
    info.depth = image(frame.depth, frame.depth_view, frame.depth_format, frame.render_width, frame.render_height,
                       false);
    info.motion = image(frame.motion, frame.motion_view, frame.motion_format, frame.render_width,
                        frame.render_height, false);
    if (frame.hudless && REXCVAR_GET(native_dlss_frame_generation_hudless))
      info.hudless = image(frame.hudless, frame.hudless_view, swapchain_format_, extent_.width, extent_.height, false);
    info.rect_x = frame.rect_x; info.rect_y = frame.rect_y;
    info.rect_width = frame.rect_width; info.rect_height = frame.rect_height;
    // The depth and motion images cover the rectangle (their pixels: the motion's units).
    info.render_width = frame.render_width;
    info.render_height = frame.render_height;
    info.motion_scale_x = info.motion_scale_y = 1.f;
    // Clip space (NGX's DLSS-G: not pixels as DLSS SR): pixels of the depth
    // image, content moved +x right, +y down; clip y up.
    info.jitter_x = 2.f * frame.jitter_x / float(frame.render_width);
    info.jitter_y = -2.f * frame.jitter_y / float(frame.render_height);
    // Linearized depth (about distance / near) to NVIDIA's typical units
    // (distance / 0.1 m: a 10 cm near plane), unless set.
    const double meters = frame.meters_per_unit > 0 ? frame.meters_per_unit : 1.0;
    // NGX's linearized depth is distance / near: scaled by near in meters /
    // 0.2 mm it depends on distance only (measured: generated frames stop
    // shimmering at 130-200 with a 3.6-3.9 cm near plane; 1, NGX's default,
    // and below shimmer).
    const double automatic =
        frame.camera_near > 0 ? std::clamp(frame.camera_near * meters / 2e-4, 1e-3, 1e4) : 1.0;
    const double configured = REXCVAR_GET(native_dlss_frame_generation_depth_scale);
    info.depth_scale = float(configured > 0 ? configured : automatic);
    if (logged_depth_scale_ <= 0 || info.depth_scale > logged_depth_scale_ * 2.f ||
        info.depth_scale < logged_depth_scale_ * 0.5f) {
      REXLOG_INFO("DLSS frame generation: camera near {:g} far {:g} ({:g} m per unit), linearized depth scale {:g}{}",
                  frame.camera_near, frame.camera_far, meters, info.depth_scale, configured > 0 ? " (set)" : "");
      logged_depth_scale_ = info.depth_scale;
    }
    // A new history unless this frame's motion refers to the frame NGX has.
    info.reset = frame.reset || reset_next_ || !frame.previous_frame || frame.previous_frame != last_frame_;
    if (info.reset) {
      ++resets_;
      const size_t reason = frame.reset ? 0 : reset_next_ ? 1 : !frame.previous_frame ? 2 : 3;
      ++reset_reasons_[reason];
      if (reason == 3 && logged_gaps_ < 16) {
        ++logged_gaps_;
        REXLOG_INFO("DLSS frame generation: new history at frame {}: its motion refers to frame {}, NGX last had {}",
                    frame.frame, frame.previous_frame, last_frame_);
      }
    }
    info.depth_inverted = frame.depth_inverted;
    info.view_to_clip = frame.view_to_clip; info.clip_to_view = frame.clip_to_view;
    info.clip_to_previous_clip = frame.clip_to_previous_clip;
    info.previous_clip_to_clip = frame.previous_clip_to_clip;
    info.position = frame.position; info.up = frame.up; info.right = frame.right; info.forward = frame.forward;
    // An infinite far plane (0): a large distance.
    info.near_distance = frame.camera_near;
    info.far_distance = frame.camera_far > 0 ? frame.camera_far : 1.0e6f;
    info.fov_y = frame.fov_y; info.aspect = frame.aspect;
    // Consecutive per backbuffer NGX gets (a jump would read as a cut).
    info.frame_id = ++frame_id_;
    last_frame_ = frame.frame;
    std::string error;
    if (ngx::FgEvaluate(command_buffer, feature_, info, error)) {
      generated_frame = true;
      reset_next_ = false;
    } else {
      reset_next_ = true;
      if (error != last_error_) {
        last_error_ = error;
        REXLOG_ERROR("DLSS frame generation: {}", error);
      }
    }
  }
  // The generated frame (or the real one again) into the acquired image.
  // Diagnostics (native_frame_generation_debug): 1 the real frame in its place
  // (the same pacing without generated content), 2 the generated frame in both
  // (only generated content shown).
  const int32_t debug = REXCVAR_GET(native_frame_generation_debug);
  Barrier(dfn, command_buffer, generated, kTransfer, 0, VK_IMAGE_LAYOUT_UNDEFINED, kTransfer,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  if (generated_frame && debug != 1) {
    Barrier(dfn, command_buffer, output_, kCompute, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            kTransfer, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Copy(dfn, command_buffer, output_, format_, generated, swapchain_format_, extent_);
    if (debug == 2) {
      Barrier(dfn, command_buffer, real, kTransfer, VK_ACCESS_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      Copy(dfn, command_buffer, output_, format_, real, swapchain_format_, extent_);
      Barrier(dfn, command_buffer, real, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTransfer, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
  } else {
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent_.width, extent_.height, 1};
    dfn.vkCmdCopyImage(command_buffer, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, generated,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  Barrier(dfn, command_buffer, generated, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  Barrier(dfn, command_buffer, real, kTransfer, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  if (generated_frame) ++generated_frames_;
  generating_ = generated_frame;
  return generated_frame;
}

void VulkanDlssFrameGeneration::ScheduleRealPresent(uint32_t image_index, VkSemaphore wait_semaphore) {
  const auto now = std::chrono::steady_clock::now();
  // The real frames' interval, smoothed (pauses ignored).
  if (last_schedule_ != std::chrono::steady_clock::time_point{}) {
    const double seconds = std::chrono::duration<double>(now - last_schedule_).count();
    if (seconds > 0 && seconds < 0.1)
      interval_seconds_ = interval_seconds_ > 0 ? interval_seconds_ * 0.9 + seconds * 0.1 : seconds;
  }
  last_schedule_ = now;
  // FIFO shows the held frame on the vblank after the generated one; other
  // modes show at once, so the real one waits half a real frame.
  const double delay = fifo_ ? 0.0 : std::clamp(interval_seconds_ * 0.5, 0.0, 0.033);
  {
    std::lock_guard lock(present_mutex_);
    present_pending_ = true;
    present_swapchain_ = swapchain_;
    present_index_ = image_index;
    present_semaphore_ = wait_semaphore;
    present_due_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(delay));
  }
  present_condition_.notify_all();
  ++held_presents_;
}

VkResult VulkanDlssFrameGeneration::WaitForHeldPresent() {
  std::unique_lock lock(present_mutex_);
  present_condition_.wait(lock, [this] { return !present_pending_; });
  const VkResult result = present_result_;
  present_result_ = VK_SUCCESS;
  return result;
}

void VulkanDlssFrameGeneration::PresentLoop() {
  std::unique_lock lock(present_mutex_);
  for (;;) {
    present_condition_.wait(lock, [this] { return present_pending_ || present_stop_; });
    if (present_stop_ && !present_pending_) return;
    const auto due = present_due_;
    const uint32_t index = present_index_;
    const VkSemaphore semaphore = present_semaphore_;
    const VkSwapchainKHR swapchain = present_swapchain_;
    lock.unlock();
    std::this_thread::sleep_until(due);
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = semaphore ? 1 : 0;
    info.pWaitSemaphores = &semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain;
    info.pImageIndices = &index;
    VkResult result;
    {
      const VulkanDevice::Queue::Acquisition queue = device_->AcquireQueue(present_queue_family_, 0);
      result = device_->functions().vkQueuePresentKHR(queue.queue(), &info);
    }
    lock.lock();
    present_result_ = result;
    present_pending_ = false;
    present_condition_.notify_all();
  }
}

}  // namespace rex::ui::vulkan
