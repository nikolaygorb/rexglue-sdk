/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/ui/flags.h>
#include <rex/ui/vulkan/frame_generation.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#include "nis_upscaler.h"

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

#if REX_PLATFORM_ANDROID
#include <rex/ui/surface_android.h>
#endif
#if REX_PLATFORM_GNU_LINUX
#include <rex/ui/surface_gnulinux.h>
#endif
#if REX_PLATFORM_WIN32
#include <rex/ui/surface_win.h>
#endif
#if REX_PLATFORM_MAC
#include <rex/ui/surface_mac.h>
#endif

REXCVAR_DEFINE_BOOL(present_render_pass_clear, true, "UI/Presenter",
                    "Clear render pass during presentation");

REXCVAR_DEFINE_BOOL(vulkan_allow_present_mode_immediate, true, "UI/Vulkan",
                    "Allow immediate present mode (no vsync)");

REXCVAR_DEFINE_BOOL(vulkan_allow_present_mode_mailbox, true, "UI/Vulkan",
                    "Allow mailbox present mode (triple buffering)");

REXCVAR_DEFINE_BOOL(vulkan_allow_present_mode_fifo_relaxed, true, "UI/Vulkan",
                    "Allow FIFO relaxed present mode");

REXCVAR_DEFINE_BOOL(vulkan_allow_present_mode_fifo_latest_ready, false, "UI/Vulkan",
                    "Use FIFO latest ready (VK_EXT_present_mode_fifo_latest_ready) instead of "
                    "plain FIFO when the device and the surface support it: vsync without "
                    "tearing, but a frame queued while an older one waits for the vblank "
                    "replaces it instead of being shown after it. Only when immediate, mailbox "
                    "and FIFO relaxed are not used");

REXCVAR_DEFINE_BOOL(vulkan_semaphore_reuse_workaround, false, "UI/Vulkan",
                    "Wait for presentation queue idle before each frame to prevent semaphore "
                    "reuse. May fix rendering issues but causes significant performance loss.");

namespace rex {
namespace ui {
namespace vulkan {

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
namespace {

std::atomic<PFN_vkGetDeviceProcAddr> g_ffx_vk_get_device_proc_addr{nullptr};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FfxVkGetDeviceProcAddrCompat(VkDevice device,
                                                                      const char* p_name) {
  if (!p_name) {
    return nullptr;
  }

  PFN_vkGetDeviceProcAddr get_device_proc_addr =
      g_ffx_vk_get_device_proc_addr.load(std::memory_order_relaxed);
  if (!get_device_proc_addr) {
    return nullptr;
  }

  PFN_vkVoidFunction proc = get_device_proc_addr(device, p_name);
  if (proc) {
    return proc;
  }

  // Some drivers expose Vulkan 1.1+ core entry points, but not the legacy KHR
  // aliases expected by parts of FidelityFX VK backend initialization.
  if (!std::strcmp(p_name, "vkGetBufferMemoryRequirements2KHR")) {
    return get_device_proc_addr(device, "vkGetBufferMemoryRequirements2");
  }
  if (!std::strcmp(p_name, "vkGetImageMemoryRequirements2KHR")) {
    return get_device_proc_addr(device, "vkGetImageMemoryRequirements2");
  }
  if (!std::strcmp(p_name, "vkBindBufferMemory2KHR")) {
    return get_device_proc_addr(device, "vkBindBufferMemory2");
  }
  if (!std::strcmp(p_name, "vkBindImageMemory2KHR")) {
    return get_device_proc_addr(device, "vkBindImageMemory2");
  }

  return nullptr;
}

}  // namespace
#endif  // defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME

namespace {

// vkAcquireNextImageKHR waits without a timeout, and vkQueuePresentKHR runs
// holding the queue the command processor submits to: either blocking this
// long stalls the game, so it is logged.
void LogBlockingPresentationCall(const char* call, std::chrono::steady_clock::time_point start,
                                 VkResult result) {
  const int64_t ms = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
  if (ms >= 1000) {
    REXLOG_WARN("VulkanPresenter: {} blocked for {} ms (VkResult {})", call, ms, int(result));
  }
}

}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/vulkan_spirv/guest_output_bilinear_dither_ps.h"
#include "../shaders/vulkan_spirv/guest_output_bilinear_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_cas_resample_dither_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_cas_resample_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_cas_sharpen_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_fsr_easu_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "../shaders/vulkan_spirv/guest_output_ffx_fsr_rcas_ps.h"
#include "../shaders/vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

VulkanPresenter::PaintContext::Submission::~Submission() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (draw_command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, draw_command_pool_, nullptr);
  }

  if (acquire_semaphore_ != VK_NULL_HANDLE) {
    dfn.vkDestroySemaphore(device, acquire_semaphore_, nullptr);
  }
}

bool VulkanPresenter::PaintContext::Submission::Initialize() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkSemaphoreCreateInfo semaphore_create_info;
  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_create_info.pNext = nullptr;
  semaphore_create_info.flags = 0;
  if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &acquire_semaphore_) !=
      VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create a swapchain image acquisition "
        "semaphore");
    return false;
  }

  VkCommandPoolCreateInfo command_pool_create_info;
  command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_create_info.pNext = nullptr;
  command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  command_pool_create_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr, &draw_command_pool_) !=
      VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create a command pool for drawing to a "
        "swapchain");
    return false;
  }
  VkCommandBufferAllocateInfo command_buffer_allocate_info;
  command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_allocate_info.pNext = nullptr;
  command_buffer_allocate_info.commandPool = draw_command_pool_;
  command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &draw_command_buffer_) !=
      VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to allocate a command buffer for drawing to a "
        "swapchain");
    return false;
  }

  return true;
}

VulkanPresenter::VulkanPresenter(HostGpuLossCallback host_gpu_loss_callback,
                                 VulkanDevice* vulkan_device, const UISamplers* ui_samplers)
    : Presenter(host_gpu_loss_callback),
      vulkan_device_(vulkan_device),
      ui_samplers_(ui_samplers),
      guest_output_image_refresher_completion_timeline_(vulkan_device),
      ui_completion_timeline_(vulkan_device),
      paint_context_(vulkan_device) {
  assert_not_null(vulkan_device);
  assert_not_null(ui_samplers);
}

VulkanPresenter::~VulkanPresenter() {
  // Destroy the swapchain after its images are not used for drawing anymore.
  // This is a confusing part in Vulkan, as vkQueuePresentKHR doesn't signal a
  // fence clearly indicating when it's safe to destroy a swapchain, so we
  // assume that its lifetime is tracked internally in the WSI. This is also
  // done before destroying the semaphore awaited by vkQueuePresentKHR, hoping
  // that it will prevent the destruction during the semaphore wait in
  // vkQueuePresentKHR execution (or between the vkQueueSubmit semaphore signal
  // and the vkQueuePresentKHR semaphore wait).
  // This will await completion of all paint submissions also.
  paint_context_.DestroySwapchainAndVulkanSurface();
  util::DestroyAndNullHandle(vulkan_device_->functions().vkDestroyRenderPass,
                             vulkan_device_->device(), paint_context_.fg_hudless_render_pass);
  SetFrameGenerationActive(false);

  // Await completion of the usage of everything before destroying anything
  // (paint submission completion already awaited).
  // From most likely the latest to most likely the earliest to be signaled, so
  // just one sleep will likely be needed.
  ui_completion_timeline_.AwaitAllSubmissions();
  guest_output_image_refresher_completion_timeline_.AwaitAllSubmissions();
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  DestroyTemporalUpscalerContext();
#endif
  nis_upscaler_.reset();
  paint_context_.frame_generation = nullptr;
  frame_generation_.reset();
  paint_context_.dlss_frame_generation = nullptr;
  dlss_frame_generation_.reset();

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (paint_context_.swapchain_render_pass != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, paint_context_.swapchain_render_pass, nullptr);
  }

  for (const PaintContext::UISetupCommandBuffer& ui_setup_command_buffer :
       paint_context_.ui_setup_command_buffers) {
    dfn.vkDestroyCommandPool(device, ui_setup_command_buffer.command_pool, nullptr);
  }

  for (VkFramebuffer& framebuffer : paint_context_.guest_output_intermediate_framebuffers) {
    util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, framebuffer);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                             paint_context_.guest_output_descriptor_pool);
  for (PaintContext::GuestOutputPaintPipeline& guest_output_paint_pipeline :
       paint_context_.guest_output_paint_pipelines) {
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                               guest_output_paint_pipeline.swapchain_pipeline);
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                               guest_output_paint_pipeline.intermediate_pipeline);
  }

  util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                             guest_output_intermediate_render_pass_);
  for (VkShaderModule& shader_module : guest_output_paint_fs_) {
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, shader_module);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, guest_output_paint_vs_);
  for (VkPipelineLayout& pipeline_layout : guest_output_paint_pipeline_layouts_) {
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                             guest_output_paint_image_descriptor_set_layout_);
}

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
void VulkanPresenter::DestroyTemporalUpscalerContext() {
  if (!temporal_upscaler_context_) {
    return;
  }
  ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
  ffxDestroyContext(context, nullptr);
  temporal_upscaler_context_ = nullptr;
  temporal_upscaler_max_render_width_ = 0;
  temporal_upscaler_max_render_height_ = 0;
  temporal_upscaler_max_output_width_ = 0;
  temporal_upscaler_max_output_height_ = 0;
}

bool VulkanPresenter::EnsureTemporalUpscalerContext(uint32_t render_width, uint32_t render_height,
                                                    uint32_t output_width, uint32_t output_height) {
  if (!temporal_upscaler_context_ || temporal_upscaler_max_render_width_ != render_width ||
      temporal_upscaler_max_render_height_ != render_height ||
      temporal_upscaler_max_output_width_ != output_width ||
      temporal_upscaler_max_output_height_ != output_height) {
    DestroyTemporalUpscalerContext();

    ffxCreateContextDescUpscale create_desc = {};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create_desc.header.pNext = nullptr;
    create_desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    create_desc.maxRenderSize.width = render_width;
    create_desc.maxRenderSize.height = render_height;
    create_desc.maxUpscaleSize.width = output_width;
    create_desc.maxUpscaleSize.height = output_height;
    create_desc.fpMessage = nullptr;

    ffxCreateBackendVKDesc backend_desc = {};
    backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend_desc.header.pNext = nullptr;
    backend_desc.vkDevice = vulkan_device_->device();
    backend_desc.vkPhysicalDevice = vulkan_device_->physical_device();
    PFN_vkGetDeviceProcAddr vk_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vulkan_device_->vulkan_instance()->functions().vkGetInstanceProcAddr(
            vulkan_device_->vulkan_instance()->instance(), "vkGetDeviceProcAddr"));
    if (!vk_device_proc_addr) {
      REXLOG_WARN("VulkanPresenter: vkGetDeviceProcAddr is unavailable for FidelityFX");
      return false;
    }
    g_ffx_vk_get_device_proc_addr.store(vk_device_proc_addr, std::memory_order_relaxed);
    backend_desc.vkDeviceProcAddr = FfxVkGetDeviceProcAddrCompat;

    create_desc.header.pNext = &backend_desc.header;

    ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
    if (ffxCreateContext(context, &create_desc.header, nullptr) != FFX_API_RETURN_OK) {
      REXLOG_WARN(
          "VulkanPresenter: Failed to create FidelityFX temporal upscaler "
          "context");
      temporal_upscaler_context_ = nullptr;
      return false;
    }

    temporal_upscaler_max_render_width_ = render_width;
    temporal_upscaler_max_render_height_ = render_height;
    temporal_upscaler_max_output_width_ = output_width;
    temporal_upscaler_max_output_height_ = output_height;
    temporal_upscaler_provider_logged_ = false;
  }

  if (!temporal_upscaler_provider_logged_) {
    ffxQueryGetProviderVersion provider_version = {};
    provider_version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    provider_version.header.pNext = nullptr;
    ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
    if (ffxQuery(context, &provider_version.header) == FFX_API_RETURN_OK &&
        provider_version.versionName) {
      REXLOG_INFO("VulkanPresenter: FidelityFX upscaler provider {}", provider_version.versionName);
    }
    temporal_upscaler_provider_logged_ = true;
  }

  return temporal_upscaler_context_ != nullptr;
}

bool VulkanPresenter::DispatchTemporalUpscaler(VkCommandBuffer command_buffer, VkImage input_image,
                                               uint32_t input_width, uint32_t input_height,
                                               VkImage output_image, uint32_t output_width,
                                               uint32_t output_height,
                                               const GuestOutputPaintConfig& config) {
  if (command_buffer == VK_NULL_HANDLE || input_image == VK_NULL_HANDLE ||
      output_image == VK_NULL_HANDLE || !input_width || !input_height || !output_width ||
      !output_height) {
    return false;
  }
  if (!EnsureTemporalUpscalerContext(input_width, input_height, output_width, output_height)) {
    return false;
  }

  VkImageCreateInfo source_image_create_info = {};
  source_image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  source_image_create_info.imageType = VK_IMAGE_TYPE_2D;
  source_image_create_info.format = kGuestOutputFormat;
  source_image_create_info.extent.width = input_width;
  source_image_create_info.extent.height = input_height;
  source_image_create_info.extent.depth = 1;
  source_image_create_info.mipLevels = 1;
  source_image_create_info.arrayLayers = 1;
  source_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  source_image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  source_image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  source_image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  source_image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  FfxApiResourceDescription source_desc = ffxApiGetImageResourceDescriptionVK(
      input_image, source_image_create_info, FFX_API_RESOURCE_USAGE_READ_ONLY);
  FfxApiResource source = ffxApiGetResourceVK(reinterpret_cast<void*>(input_image), source_desc,
                                              FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  FfxApiResource depth =
      ffxApiGetResourceVK(reinterpret_cast<void*>(input_image),
                          ffxApiGetImageResourceDescriptionVK(input_image, source_image_create_info,
                                                              FFX_API_RESOURCE_USAGE_DEPTHTARGET),
                          FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  FfxApiResource motion_vectors = source;

  VkImageCreateInfo output_image_create_info = source_image_create_info;
  output_image_create_info.extent.width = output_width;
  output_image_create_info.extent.height = output_height;
  FfxApiResourceDescription output_desc = ffxApiGetImageResourceDescriptionVK(
      output_image, output_image_create_info,
      FFX_API_RESOURCE_USAGE_UAV | FFX_API_RESOURCE_USAGE_READ_ONLY);
  FfxApiResource output = ffxApiGetResourceVK(reinterpret_cast<void*>(output_image), output_desc,
                                              FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

  ffxDispatchDescUpscale dispatch_desc = {};
  dispatch_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch_desc.header.pNext = nullptr;
  dispatch_desc.commandList = command_buffer;
  dispatch_desc.color = source;
  dispatch_desc.depth = depth;
  dispatch_desc.motionVectors = motion_vectors;
  dispatch_desc.exposure = {};
  dispatch_desc.reactive = {};
  dispatch_desc.transparencyAndComposition = {};
  dispatch_desc.output = output;
  dispatch_desc.jitterOffset.x = 0.0f;
  dispatch_desc.jitterOffset.y = 0.0f;
  dispatch_desc.motionVectorScale.x = float(input_width);
  dispatch_desc.motionVectorScale.y = float(input_height);
  dispatch_desc.renderSize.width = input_width;
  dispatch_desc.renderSize.height = input_height;
  dispatch_desc.upscaleSize.width = output_width;
  dispatch_desc.upscaleSize.height = output_height;
  dispatch_desc.enableSharpening = true;
  dispatch_desc.sharpness = std::clamp(1.0f - config.GetFsrSharpnessReduction() * 0.5f, 0.0f, 1.0f);
  dispatch_desc.reset = true;
  dispatch_desc.frameTimeDelta = 16.666f;
  dispatch_desc.preExposure = 1.0f;
  dispatch_desc.cameraNear = 0.1f;
  dispatch_desc.cameraFar = 1000.0f;
  dispatch_desc.cameraFovAngleVertical = 1.0472f;
  dispatch_desc.viewSpaceToMetersFactor = 1.0f;
  dispatch_desc.flags = 0;

  ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
  if (ffxDispatch(context, &dispatch_desc.header) != FFX_API_RETURN_OK) {
    return false;
  }
  return true;
}
#endif

bool VulkanPresenter::DispatchNisUpscaler(VkCommandBuffer command_buffer, uint64_t submission,
                                          const GuestOutputImage& input, uint32_t input_width,
                                          uint32_t input_height, const GuestOutputImage& output,
                                          uint32_t output_width, uint32_t output_height,
                                          const GuestOutputPaintConfig& config) {
  if (!nis_upscaler_attempted_) {
    nis_upscaler_attempted_ = true;
    nis_upscaler_ = VulkanNisUpscaler::Create(
        vulkan_device_, ui_samplers_->samplers()[UISamplers::kSamplerIndexLinearClampToEdge]);
    if (!nis_upscaler_) {
      REXLOG_WARN("VulkanPresenter: NVIDIA Image Scaling is unavailable, using FSR EASU instead");
    }
  }
  if (!nis_upscaler_) {
    return false;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  std::array<VkImageMemoryBarrier, 2> barriers = {};
  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = util::InitializeSubresourceRange();
  }
  // The input was written by an earlier pass (or the guest output refresher)
  // and is read in the compute shader; the output is written as a storage
  // image.
  barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barriers[0].image = input.image();
  barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barriers[1].image = output.image();
  dfn.vkCmdPipelineBarrier(command_buffer,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                           uint32_t(barriers.size()), barriers.data());

  bool dispatched = nis_upscaler_->Dispatch(
      command_buffer, paint_context_.completion_timeline, submission, input.view(), input_width,
      input_height, output.view(), output_width, output_height, config.GetNisSharpness());

  // Back to what the next pass samples, written or not (EASU then draws over
  // it if the dispatch failed).
  VkImageMemoryBarrier& output_barrier = barriers[1];
  output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  output_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  output_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  output_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &output_barrier);
  return dispatched;
}

Surface::TypeFlags VulkanPresenter::GetSurfaceTypesSupportedByInstance(
    const VulkanInstance::Extensions& instance_extensions) {
  if (!instance_extensions.ext_KHR_surface) {
    return 0;
  }
  Surface::TypeFlags type_flags = 0;
#if REX_PLATFORM_ANDROID
  if (instance_extensions.ext_KHR_android_surface) {
    type_flags |= Surface::kTypeFlag_AndroidNativeWindow;
  }
#endif
#if REX_PLATFORM_GNU_LINUX
  if (instance_extensions.ext_KHR_wayland_surface) {
    type_flags |= Surface::kTypeFlag_WaylandSurface;
  }
  if (instance_extensions.ext_KHR_xcb_surface) {
    type_flags |= Surface::kTypeFlag_XcbWindow;
  }
#endif
#if REX_PLATFORM_WIN32
  if (instance_extensions.ext_KHR_win32_surface) {
    type_flags |= Surface::kTypeFlag_Win32Hwnd;
  }
#endif
#if REX_PLATFORM_MAC
  if (instance_extensions.ext_EXT_metal_surface) {
    type_flags |= Surface::kTypeFlag_CAMetalLayer;
  }
#endif
  return type_flags;
}

Surface::TypeFlags VulkanPresenter::GetSupportedSurfaceTypes() const {
  if (!vulkan_device_->extensions().ext_KHR_swapchain) {
    return 0;
  }
  return GetSurfaceTypesSupportedByInstance(vulkan_device_->vulkan_instance()->extensions());
}

bool VulkanPresenter::CaptureGuestOutput(RawImage& image_out) {
  std::shared_ptr<GuestOutputImage> guest_output_image;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      assert_true(guest_output_images_[guest_output_mailbox_index].ever_successfully_refreshed);
      guest_output_image = guest_output_images_[guest_output_mailbox_index].image;
    }
    // Incremented the reference count of the guest output image - safe to leave
    // the consumer critical section now.
  }
  if (!guest_output_image) {
    return false;
  }

  VkExtent2D image_extent = guest_output_image->extent();
  size_t pixel_count = size_t(image_extent.width) * image_extent.height;
  VkDeviceSize buffer_size = VkDeviceSize(sizeof(uint32_t) * pixel_count);
  VkBuffer buffer;
  VkDeviceMemory buffer_memory;
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device_, buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          util::MemoryPurpose::kReadback, buffer, buffer_memory)) {
    REXLOG_ERROR("VulkanPresenter: Failed to create the guest output capture buffer");
    return false;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  {
    VkCommandPoolCreateInfo command_pool_create_info;
    command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_create_info.pNext = nullptr;
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool_create_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
    VkCommandPool command_pool;
    if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr, &command_pool) !=
        VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to create the guest output capturing "
          "command pool");
      dfn.vkDestroyBuffer(device, buffer, nullptr);
      dfn.vkFreeMemory(device, buffer_memory, nullptr);
      return false;
    }

    VkCommandBufferAllocateInfo command_buffer_allocate_info;
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.pNext = nullptr;
    command_buffer_allocate_info.commandPool = command_pool;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer;
    if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer) !=
        VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to allocate the guest output capturing "
          "command buffer");
      dfn.vkDestroyCommandPool(device, command_pool, nullptr);
      dfn.vkDestroyBuffer(device, buffer, nullptr);
      dfn.vkFreeMemory(device, buffer_memory, nullptr);
      return false;
    }

    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to begin recording the guest output "
          "capturing command buffer");
      dfn.vkDestroyCommandPool(device, command_pool, nullptr);
      dfn.vkDestroyBuffer(device, buffer, nullptr);
      dfn.vkFreeMemory(device, buffer_memory, nullptr);
      return false;
    }

    VkImageMemoryBarrier image_memory_barrier;
    image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_memory_barrier.pNext = nullptr;
    image_memory_barrier.srcAccessMask = kGuestOutputInternalAccessMask;
    image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_memory_barrier.oldLayout = kGuestOutputInternalLayout;
    image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_memory_barrier.image = guest_output_image->image();
    image_memory_barrier.subresourceRange = util::InitializeSubresourceRange();
    dfn.vkCmdPipelineBarrier(command_buffer, kGuestOutputInternalStageMask,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &image_memory_barrier);

    VkBufferImageCopy buffer_image_copy = {};
    buffer_image_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    buffer_image_copy.imageSubresource.layerCount = 1;
    buffer_image_copy.imageExtent.width = image_extent.width;
    buffer_image_copy.imageExtent.height = image_extent.height;
    buffer_image_copy.imageExtent.depth = 1;
    dfn.vkCmdCopyImageToBuffer(command_buffer, guest_output_image->image(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &buffer_image_copy);

    // A fence doesn't guarantee host visibility and availability.
    VkBufferMemoryBarrier buffer_memory_barrier;
    buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_memory_barrier.pNext = nullptr;
    buffer_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_memory_barrier.buffer = buffer;
    buffer_memory_barrier.offset = 0;
    buffer_memory_barrier.size = VK_WHOLE_SIZE;
    std::swap(image_memory_barrier.srcAccessMask, image_memory_barrier.dstAccessMask);
    std::swap(image_memory_barrier.oldLayout, image_memory_barrier.newLayout);
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT | kGuestOutputInternalStageMask, 0, 0,
                             nullptr, 1, &buffer_memory_barrier, 1, &image_memory_barrier);

    if (dfn.vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to end recording the guest output capturing "
          "command buffer");
      dfn.vkDestroyCommandPool(device, command_pool, nullptr);
      dfn.vkDestroyBuffer(device, buffer, nullptr);
      dfn.vkFreeMemory(device, buffer_memory, nullptr);
      return false;
    }

    {
      VulkanGPUCompletionTimeline completion_timeline(vulkan_device_);
      VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &command_buffer;
      const VkResult submit_result = completion_timeline.AcquireFenceAndSubmit(
          vulkan_device_->queue_family_graphics_compute(), 0, 1, &submit_info);
      if (submit_result != VK_SUCCESS) {
        REXLOG_ERROR(
            "VulkanPresenter: Failed to submit the guest output capturing "
            "command buffer: {}",
            vk::to_string(vk::Result(submit_result)));
        dfn.vkDestroyCommandPool(device, command_pool, nullptr);
        dfn.vkDestroyBuffer(device, buffer, nullptr);
        dfn.vkFreeMemory(device, buffer_memory, nullptr);
        return false;
      }
      // Destroying the completion timeline causes the submission to be awaited.
    }

    dfn.vkDestroyCommandPool(device, command_pool, nullptr);
  }

  // Don't need the buffer anymore, just its memory.
  dfn.vkDestroyBuffer(device, buffer, nullptr);

  void* mapping;
  if (dfn.vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to map the guest output capture memory");
    dfn.vkFreeMemory(device, buffer_memory, nullptr);
    return false;
  }

  image_out.width = image_extent.width;
  image_out.height = image_extent.height;
  image_out.stride = sizeof(uint32_t) * image_extent.width;
  image_out.data.resize(size_t(buffer_size));
  uint32_t* image_out_pixels = reinterpret_cast<uint32_t*>(image_out.data.data());
  for (size_t i = 0; i < pixel_count; ++i) {
    image_out_pixels[i] = Packed10bpcRGBTo8bpcBytes(reinterpret_cast<const uint32_t*>(mapping)[i]);
  }

  // Unmapping will be done by freeing.
  dfn.vkFreeMemory(device, buffer_memory, nullptr);

  return true;
}

VkCommandBuffer VulkanPresenter::AcquireUISetupCommandBufferFromUIThread() {
  if (paint_context_.ui_setup_command_buffer_current_index != SIZE_MAX) {
    return paint_context_
        .ui_setup_command_buffers[paint_context_.ui_setup_command_buffer_current_index]
        .command_buffer;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkCommandBufferBeginInfo command_buffer_begin_info;
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.pNext = nullptr;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  command_buffer_begin_info.pInheritanceInfo = nullptr;

  // Try to reuse an existing command buffer.
  if (!paint_context_.ui_setup_command_buffers.empty()) {
    const uint64_t submission_index_completed =
        ui_completion_timeline_.UpdateAndGetCompletedSubmission();
    for (size_t i = 0; i < paint_context_.ui_setup_command_buffers.size(); ++i) {
      PaintContext::UISetupCommandBuffer& ui_setup_command_buffer =
          paint_context_.ui_setup_command_buffers[i];
      if (ui_setup_command_buffer.last_usage_submission_index > submission_index_completed) {
        continue;
      }
      if (dfn.vkResetCommandPool(device, ui_setup_command_buffer.command_pool, 0) != VK_SUCCESS) {
        REXLOG_ERROR("VulkanPresenter: Failed to reset a UI setup command pool");
        return VK_NULL_HANDLE;
      }
      if (dfn.vkBeginCommandBuffer(ui_setup_command_buffer.command_buffer,
                                   &command_buffer_begin_info) != VK_SUCCESS) {
        REXLOG_ERROR(
            "VulkanPresenter: Failed to begin UI setup command buffer "
            "recording");
        return VK_NULL_HANDLE;
      }
      paint_context_.ui_setup_command_buffer_current_index = i;
      ui_setup_command_buffer.last_usage_submission_index =
          ui_completion_timeline_.GetUpcomingSubmission();
      return ui_setup_command_buffer.command_buffer;
    }
  }

  // Create a new command buffer.
  VkCommandPoolCreateInfo command_pool_create_info;
  command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_create_info.pNext = nullptr;
  command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  command_pool_create_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  VkCommandPool new_command_pool;
  if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr, &new_command_pool) !=
      VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to create a UI setup command pool");
    return VK_NULL_HANDLE;
  }
  VkCommandBufferAllocateInfo command_buffer_allocate_info;
  command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_allocate_info.pNext = nullptr;
  command_buffer_allocate_info.commandPool = new_command_pool;
  command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_allocate_info.commandBufferCount = 1;
  VkCommandBuffer new_command_buffer;
  if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &new_command_buffer) !=
      VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to allocate a UI setup command buffer");
    dfn.vkDestroyCommandPool(device, new_command_pool, nullptr);
    return VK_NULL_HANDLE;
  }
  if (dfn.vkBeginCommandBuffer(new_command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to begin UI setup command buffer recording");
    dfn.vkDestroyCommandPool(device, new_command_pool, nullptr);
    return VK_NULL_HANDLE;
  }
  paint_context_.ui_setup_command_buffer_current_index =
      paint_context_.ui_setup_command_buffers.size();
  paint_context_.ui_setup_command_buffers.emplace_back(
      new_command_pool, new_command_buffer, ui_completion_timeline_.GetUpcomingSubmission());
  return new_command_buffer;
}

Presenter::SurfacePaintConnectResult
VulkanPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                 uint32_t new_surface_width,
                                                                 uint32_t new_surface_height,
                                                                 bool was_paintable,
                                                                 bool& is_vsync_implicit_out) {
  const VulkanInstance* const vulkan_instance = vulkan_device_->vulkan_instance();
  const VulkanInstance::Functions& ifn = vulkan_instance->functions();
  const VkInstance instance = vulkan_instance->instance();
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkFormat new_swapchain_format;

  // ConnectOrReconnectToSurfaceFromUIThread may be called only for the
  // ui::Surface of the current swapchain or when the old swapchain and
  // VkSurface have, if the ui::Surface is the same, try using the existing
  // VkSurface and creating the swapchain smoothly from the existing one - if
  // this doesn't succeed, start from scratch.
  // The retirement or destruction of the swapchain here will also cause
  // awaiting completion of the usage of the swapchain and the surface on the
  // GPU.
  if (paint_context_.vulkan_surface != VK_NULL_HANDLE) {
    const bool old_frame_generation = paint_context_.swapchain_frame_generation;
    VkSwapchainKHR old_swapchain = paint_context_.PrepareForSwapchainRetirement();
    // A frame generation swapchain is replaced, not retired into the next.
    if (old_frame_generation) {
      paint_context_.DestroyRetiredSwapchain(old_swapchain, true);
      old_swapchain = VK_NULL_HANDLE;
    }
    bool surface_unusable;
    bool frame_generation_created = false;
    paint_context_.swapchain = PaintContext::CreateSwapchainForVulkanSurface(
        vulkan_device_, paint_context_.vulkan_surface, new_surface_width, new_surface_height,
        old_swapchain, paint_context_.present_queue_family, new_swapchain_format,
        paint_context_.swapchain_extent, paint_context_.swapchain_is_fifo, surface_unusable,
        paint_context_.frame_generation, &frame_generation_created,
        paint_context_.dlss_frame_generation != nullptr, &paint_context_.swapchain_surface_min_image_count,
        &paint_context_.swapchain_image_usage);
    paint_context_.swapchain_frame_generation = frame_generation_created;
    // Destroy the old swapchain that may be retired now.
    if (old_swapchain != VK_NULL_HANDLE) {
      dfn.vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    }
    if (paint_context_.swapchain == VK_NULL_HANDLE) {
      // Couldn't create the swapchain for the existing surface - start over.
      paint_context_.DestroySwapchainAndVulkanSurface();
    }
  }

  // If failed to create the swapchain for the previous surface, recreate the
  // surface and create the new swapchain.
  if (paint_context_.swapchain == VK_NULL_HANDLE) {
    // DestroySwapchainAndVulkanSurface should have been called previously.
    assert_true(paint_context_.vulkan_surface == VK_NULL_HANDLE);
    Surface::TypeIndex surface_type = new_surface.GetType();
    // Check if the surface type is supported according to the Vulkan
    // extensions.
    if (!(GetSupportedSurfaceTypes() & (Surface::TypeFlags(1) << surface_type))) {
      REXLOG_ERROR(
          "VulkanPresenter: Tried to create a Vulkan surface for an "
          "unsupported Xenia surface type");
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    VkResult vulkan_surface_create_result = VK_ERROR_UNKNOWN;
    switch (surface_type) {
#if REX_PLATFORM_ANDROID
      case Surface::kTypeIndex_AndroidNativeWindow: {
        auto& android_native_window_surface =
            static_cast<const AndroidNativeWindowSurface&>(new_surface);
        VkAndroidSurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.window = android_native_window_surface.window();
        vulkan_surface_create_result = ifn.vkCreateAndroidSurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
#if REX_PLATFORM_GNU_LINUX
      case Surface::kTypeIndex_XcbWindow: {
        auto& xcb_window_surface = static_cast<const XcbWindowSurface&>(new_surface);
        VkXcbSurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.connection = xcb_window_surface.connection();
        surface_create_info.window = xcb_window_surface.window();
        vulkan_surface_create_result = ifn.vkCreateXcbSurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
      case Surface::kTypeIndex_WaylandSurface: {
        auto& wayland_surface = static_cast<const WaylandSurface&>(new_surface);
        VkWaylandSurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.display = wayland_surface.display();
        surface_create_info.surface = wayland_surface.surface();
        vulkan_surface_create_result = ifn.vkCreateWaylandSurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
#if REX_PLATFORM_WIN32
      case Surface::kTypeIndex_Win32Hwnd: {
        auto& win32_hwnd_surface = static_cast<const Win32HwndSurface&>(new_surface);
        VkWin32SurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.hinstance = win32_hwnd_surface.hinstance();
        surface_create_info.hwnd = win32_hwnd_surface.hwnd();
        vulkan_surface_create_result = ifn.vkCreateWin32SurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
#if REX_PLATFORM_MAC
      case Surface::kTypeIndex_CAMetalLayer: {
        auto& metal_surface = static_cast<const CAMetalLayerSurface&>(new_surface);
        VkMetalSurfaceCreateInfoEXT surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.pLayer = metal_surface.layer();
        vulkan_surface_create_result = ifn.vkCreateMetalSurfaceEXT(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
      default:
        assert_unhandled_case(surface_type);
        REXLOG_ERROR(
            "VulkanPresenter: Tried to create a Vulkan surface for an "
            "unknown Xenia surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    if (vulkan_surface_create_result != VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a Vulkan surface");
      return SurfacePaintConnectResult::kFailure;
    }
    bool surface_unusable;
    bool frame_generation_created = false;
    paint_context_.swapchain = PaintContext::CreateSwapchainForVulkanSurface(
        vulkan_device_, paint_context_.vulkan_surface, new_surface_width, new_surface_height,
        VK_NULL_HANDLE, paint_context_.present_queue_family, new_swapchain_format,
        paint_context_.swapchain_extent, paint_context_.swapchain_is_fifo, surface_unusable,
        paint_context_.frame_generation, &frame_generation_created,
        paint_context_.dlss_frame_generation != nullptr, &paint_context_.swapchain_surface_min_image_count,
        &paint_context_.swapchain_image_usage);
    paint_context_.swapchain_frame_generation = frame_generation_created;
    if (paint_context_.swapchain == VK_NULL_HANDLE) {
      // Failed to create the swapchain for the new Vulkan surface - destroy the
      // Vulkan surface.
      ifn.vkDestroySurfaceKHR(instance, paint_context_.vulkan_surface, nullptr);
      paint_context_.vulkan_surface = VK_NULL_HANDLE;
      return surface_unusable ? SurfacePaintConnectResult::kFailureSurfaceUnusable
                              : SurfacePaintConnectResult::kFailure;
    }
    // Successfully attached (at least for now).
  }

  // From now on, in case of failure,
  // paint_context_.DestroySwapchainAndVulkanSurface must be called before
  // returning.

  // Update the render pass to the new format.
  if (paint_context_.swapchain_render_pass_format != new_swapchain_format) {
    util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                               paint_context_.swapchain_render_pass);
    paint_context_.swapchain_render_pass_format = new_swapchain_format;
  }
  if (paint_context_.swapchain_render_pass == VK_NULL_HANDLE) {
    VkAttachmentDescription render_pass_attachment;
    render_pass_attachment.flags = 0;
    render_pass_attachment.format = new_swapchain_format;
    render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    render_pass_attachment.loadOp = REXCVAR_GET(present_render_pass_clear)
                                        ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference render_pass_color_attachment;
    render_pass_color_attachment.attachment = 0;
    render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription render_pass_subpass = {};
    render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    render_pass_subpass.colorAttachmentCount = 1;
    render_pass_subpass.pColorAttachments = &render_pass_color_attachment;
    VkSubpassDependency render_pass_dependencies[2];
    render_pass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    render_pass_dependencies[0].dstSubpass = 0;
    // srcStageMask is the semaphore wait stage.
    render_pass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    render_pass_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    render_pass_dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    // The main target can be used for UI drawing at any moment, which requires
    // blending, so VK_ACCESS_COLOR_ATTACHMENT_READ_BIT is also included.
    render_pass_dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    render_pass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    render_pass_dependencies[1].srcSubpass = 0;
    render_pass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    render_pass_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // Semaphores are signaled at VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT.
    render_pass_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    render_pass_dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    render_pass_dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    render_pass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    VkRenderPassCreateInfo render_pass_create_info;
    render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_create_info.pNext = nullptr;
    render_pass_create_info.flags = 0;
    render_pass_create_info.attachmentCount = 1;
    render_pass_create_info.pAttachments = &render_pass_attachment;
    render_pass_create_info.subpassCount = 1;
    render_pass_create_info.pSubpasses = &render_pass_subpass;
    render_pass_create_info.dependencyCount = uint32_t(rex::countof(render_pass_dependencies));
    render_pass_create_info.pDependencies = render_pass_dependencies;
    VkRenderPass new_render_pass;
    if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &new_render_pass) !=
        VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to create the render pass for drawing to "
          "swapchain images");
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    paint_context_.swapchain_render_pass = new_render_pass;
    paint_context_.swapchain_render_pass_format = new_swapchain_format;
    paint_context_.swapchain_render_pass_clear_load_op =
        render_pass_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
  }

  // Get the swapchain images.
  paint_context_.swapchain_images.clear();
  VkResult swapchain_images_get_result;
  for (;;) {
    uint32_t swapchain_image_count = uint32_t(paint_context_.swapchain_images.size());
    bool swapchain_images_were_empty = !swapchain_image_count;
    VkImage* const swapchain_images_out =
        swapchain_images_were_empty ? nullptr : paint_context_.swapchain_images.data();
    swapchain_images_get_result =
        paint_context_.swapchain_frame_generation
            ? frame_generation_->GetSwapchainImages(paint_context_.swapchain, &swapchain_image_count,
                                                    swapchain_images_out)
            : dfn.vkGetSwapchainImagesKHR(device, paint_context_.swapchain, &swapchain_image_count,
                                          swapchain_images_out);
    // If the original swapchain image count was 0 (first call), SUCCESS is
    // returned, not INCOMPLETE.
    if (swapchain_images_get_result == VK_SUCCESS || swapchain_images_get_result == VK_INCOMPLETE) {
      paint_context_.swapchain_images.resize(swapchain_image_count);
      if (swapchain_images_get_result == VK_SUCCESS &&
          (!swapchain_images_were_empty || !swapchain_image_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (swapchain_images_get_result != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to get swapchain images");
    paint_context_.DestroySwapchainAndVulkanSurface();
    return SurfacePaintConnectResult::kFailure;
  }

  // Create the image views and the framebuffers.
  assert_true(paint_context_.swapchain_framebuffers.empty());
  paint_context_.swapchain_framebuffers.reserve(paint_context_.swapchain_images.size());
  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = new_swapchain_format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_create_info.subresourceRange.baseMipLevel = 0;
  image_view_create_info.subresourceRange.levelCount = 1;
  image_view_create_info.subresourceRange.baseArrayLayer = 0;
  image_view_create_info.subresourceRange.layerCount = 1;
  VkImageView image_view;
  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = paint_context_.swapchain_render_pass;
  framebuffer_create_info.attachmentCount = 1;
  framebuffer_create_info.pAttachments = &image_view;
  framebuffer_create_info.width = paint_context_.swapchain_extent.width;
  framebuffer_create_info.height = paint_context_.swapchain_extent.height;
  framebuffer_create_info.layers = 1;
  for (VkImage image : paint_context_.swapchain_images) {
    image_view_create_info.image = image;
    if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr, &image_view) !=
        VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a swapchain image view");
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    VkFramebuffer framebuffer;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
        VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a swapchain framebuffer");
      dfn.vkDestroyImageView(device, image_view, nullptr);
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    paint_context_.swapchain_framebuffers.emplace_back(image_view, framebuffer);
  }

  // Create per-swapchain-image present semaphores to avoid
  // VUID-vkQueueSubmit-pSignalSemaphores-00067 (semaphore reuse before the
  // previous present completes).
  paint_context_.swapchain_image_present_semaphores.reserve(
      paint_context_.swapchain_images.size());
  VkSemaphoreCreateInfo present_semaphore_create_info;
  present_semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  present_semaphore_create_info.pNext = nullptr;
  present_semaphore_create_info.flags = 0;
  for (size_t i = 0; i < paint_context_.swapchain_images.size(); ++i) {
    VkSemaphore present_semaphore;
    if (dfn.vkCreateSemaphore(device, &present_semaphore_create_info, nullptr,
                              &present_semaphore) != VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to create a per-swapchain-image present "
          "semaphore");
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    paint_context_.swapchain_image_present_semaphores.push_back(present_semaphore);
  }

  // DLSS frame generation on this (regular) swapchain, where it can generate.
  paint_context_.swapchain_dlss_frame_generation = false;
  if (paint_context_.dlss_frame_generation && !paint_context_.swapchain_frame_generation) {
    std::string reason;
    paint_context_.swapchain_dlss_frame_generation = paint_context_.dlss_frame_generation->AttachSwapchain(
        paint_context_.swapchain, new_swapchain_format, paint_context_.swapchain_extent,
        uint32_t(paint_context_.swapchain_images.size()), paint_context_.swapchain_surface_min_image_count,
        paint_context_.swapchain_image_usage, paint_context_.swapchain_is_fifo,
        paint_context_.present_queue_family, paint_context_.submissions.size(), reason);
    if (!paint_context_.swapchain_dlss_frame_generation) {
      REXLOG_WARN("VulkanPresenter: DLSS frame generation off for this swapchain, frames are shown as they are: {}",
                  reason);
      VulkanFrameGeneration::WriteStatus(*vulkan_device_, "NVIDIA DLSS Frame Generation unavailable: " + reason);
    }
  }
  // Frame generation paints the frame without the HUD too: a swapchain-sized image.
  const bool generating_swapchain =
      paint_context_.swapchain_frame_generation || paint_context_.swapchain_dlss_frame_generation;
  if (generating_swapchain && !CreateFrameGenerationHudless()) {
    REXLOG_ERROR("VulkanPresenter: frame generation hudless image unavailable, frames are shown as they are");
  }
  SetFrameGenerationActive(generating_swapchain && paint_context_.fg_hudless_framebuffer[1] != VK_NULL_HANDLE);
  paint_context_.fg_last_frame = 0;

  is_vsync_implicit_out = paint_context_.swapchain_is_fifo;
  return SurfacePaintConnectResult::kSuccess;
}

void VulkanPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapchainAndVulkanSurface();
}

bool VulkanPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  VkExtent2D max_framebuffer_extent = util::GetMax2DFramebufferExtent(vulkan_device_->properties());
  if (frontbuffer_width > max_framebuffer_extent.width ||
      frontbuffer_height > max_framebuffer_extent.height) {
    // Writing the guest output isn't supposed to rescale, and a guest texture
    // exceeding the maximum size won't be loadable anyway.
    return false;
  }

  GuestOutputImageInstance& image_instance = guest_output_images_[mailbox_index];
  if (image_instance.image && (image_instance.image->extent().width != frontbuffer_width ||
                               image_instance.image->extent().height != frontbuffer_height)) {
    guest_output_image_refresher_completion_timeline_.AwaitSubmissionAndUpdateCompleted(
        image_instance.last_refresher_submission);
    image_instance.image.reset();
  }
  if (!image_instance.image) {
    std::unique_ptr<GuestOutputImage> new_image =
        GuestOutputImage::Create(vulkan_device_, frontbuffer_width, frontbuffer_height);
    if (!new_image) {
      return false;
    }
    // Frame generation inputs live with the image (painting holds it).
    const bool frame_generation_images =
        (frame_generation_ || dlss_frame_generation_) && new_image->CreateFrameGenerationImages();
    image_instance.SetToNewImage(std::move(new_image), guest_output_image_next_version_++);
    if (frame_generation_images) {
      image_instance.hudless_version = guest_output_image_next_version_++;
    }
  }

  // Frame generation shows two frames per real one: the guest's frame rate is
  // held a little below half the display's refresh rate (AMD's recommendation)
  // here, on the thread producing the frames (painting may be the UI thread's).
  // Every frame then reaches the presenter, without gaps that reset generation.
  // Only while frames are generated: without the title's inputs (menus, a
  // scene the producer doesn't describe) the game keeps its own frame rate.
  if ((frame_generation_ || dlss_frame_generation_) && IsFrameGenerationActive() &&
      frame_generation_generating_.load(std::memory_order_relaxed)) {
    const double refresh = display_refresh_rate();
    const auto now = std::chrono::steady_clock::now();
    if (refresh >= 48.0) {
      const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(2.0 * 1.03 / refresh));
      const auto next = frame_generation_last_refresh_ + period;
      if (now < next && next - now < std::chrono::milliseconds(100)) {
        if (next - now > std::chrono::milliseconds(2)) {
          std::this_thread::sleep_until(next - std::chrono::milliseconds(1));
        }
        while (std::chrono::steady_clock::now() < next) {
          std::this_thread::yield();
        }
      }
    }
    frame_generation_last_refresh_ = std::chrono::steady_clock::now();
  }

  // Inputs for frame generation while it runs (the refresher fills them).
  FrameGenerationTargets frame_generation_targets;
  const GuestOutputImage& refreshed_image = *image_instance.image;
  const bool frame_generation_wanted = IsFrameGenerationActive() &&
                                       refreshed_image.frame_generation_hudless() &&
                                       refreshed_image.frame_generation_depth() &&
                                       refreshed_image.frame_generation_motion();
  if (frame_generation_wanted) {
    frame_generation_targets.hudless = refreshed_image.frame_generation_hudless()->image();
    frame_generation_targets.hudless_view = refreshed_image.frame_generation_hudless()->view();
    frame_generation_targets.hudless_version = image_instance.hudless_version;
    frame_generation_targets.depth = refreshed_image.frame_generation_depth()->image();
    frame_generation_targets.depth_view = refreshed_image.frame_generation_depth()->view();
    frame_generation_targets.motion = refreshed_image.frame_generation_motion()->image();
    frame_generation_targets.motion_view = refreshed_image.frame_generation_motion()->view();
    frame_generation_targets.ever_written_previously = image_instance.ever_successfully_refreshed;
    frame_generation_targets.debug = REXCVAR_GET(native_frame_generation_debug);
    frame_generation_targets.inputs = &GuestOutputFrameGenerationInputs(mailbox_index);
  }

  VulkanGuestOutputRefreshContext context(is_8bpc_out_ref, image_instance.image->image(),
                                          image_instance.image->view(), image_instance.version,
                                          image_instance.ever_successfully_refreshed,
                                          frame_generation_wanted ? &frame_generation_targets
                                                                  : nullptr);
  bool refresher_succeeded = refresher(context);
  if (refresher_succeeded) {
    image_instance.ever_successfully_refreshed = true;
  }
  // With a publish deferrer the refresher's work reaches the queue later, and
  // the step that follows it runs this after that.
  if (!IsGuestOutputPublishDeferred()) {
    GuestOutputRefreshSubmitted(mailbox_index);
  }
  return refresher_succeeded;
}

void VulkanPresenter::GuestOutputRefreshSubmitted(uint32_t mailbox_index) {
  GuestOutputImageInstance& image_instance = guest_output_images_[mailbox_index];
  // Even if the refresher has returned false, it still might have submitted
  // some commands referencing the image. It's better to put an excessive
  // signal and wait slightly longer, for nothing important, while shutting down
  // than to destroy the image while it's still in use.
  image_instance.last_refresher_submission =
      guest_output_image_refresher_completion_timeline_.GetUpcomingSubmission();
  // No need to make the refresher signal the fence by itself - signal it here
  // instead to have more control:
  // "Fence signal operations that are defined by vkQueueSubmit additionally
  //  include in the first synchronization scope all commands that occur earlier
  //  in submission order."
  const VkResult submit_result =
      guest_output_image_refresher_completion_timeline_.AcquireFenceAndSubmit(
          vulkan_device_->queue_family_graphics_compute(), 0, 0, nullptr);
  if (submit_result != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to submit the guest output image refresh "
        "fence signal: {}",
        vk::to_string(vk::Result(submit_result)));
  }
}

VkSwapchainKHR VulkanPresenter::PaintContext::CreateSwapchainForVulkanSurface(
    const VulkanDevice* vulkan_device, VkSurfaceKHR surface, uint32_t width, uint32_t height,
    VkSwapchainKHR old_swapchain, uint32_t& present_queue_family_out, VkFormat& image_format_out,
    VkExtent2D& image_extent_out, bool& is_fifo_out, bool& ui_surface_unusable_out,
    VulkanFrameGeneration* frame_generation, bool* frame_generation_out, bool dlss_frame_generation,
    uint32_t* surface_min_image_count_out, VkImageUsageFlags* image_usage_out) {
  ui_surface_unusable_out = false;
  if (frame_generation_out) {
    *frame_generation_out = false;
  }

  const VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Get surface capabilities.
  VkSurfaceCapabilitiesKHR surface_capabilities;
  if (ifn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface,
                                                    &surface_capabilities) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to get Vulkan surface capabilities");
    // Some strange error, try again later.
    return VK_NULL_HANDLE;
  }

  // First, check if the surface is not zero-area because in this case, the rest
  // of the fields in theory may not be informative as the surface doesn't need
  // to go to presentation anyway, thus there's no need to return real
  // information, and ui_surface_unusable_out (from which it may not be possible
  // to recover on certain platforms at all) may be set to true spuriously if
  // any checks set it to true. VkSurfaceKHR may have zero-area bounds in some
  // window state cases on Windows, for example. Also clamp the requested size
  // to the maximum supported by the physical device as long as minImageExtent
  // in the instance's WSI allows that (if not, there's no way to satisfy both
  // requirements - the maximum 2D framebuffer size on the specific physical
  // device, and the minimum swap chain size on the whole instance - fail to
  // create until the surface becomes smaller).
  VkExtent2D max_framebuffer_extent = util::GetMax2DFramebufferExtent(vulkan_device->properties());
  VkExtent2D image_extent;
  image_extent.width = std::min(std::max(std::min(width, max_framebuffer_extent.width),
                                         surface_capabilities.minImageExtent.width),
                                surface_capabilities.maxImageExtent.width);
  image_extent.height = std::min(std::max(std::min(height, max_framebuffer_extent.height),
                                          surface_capabilities.minImageExtent.height),
                                 surface_capabilities.maxImageExtent.height);
  if (!image_extent.width || !image_extent.height ||
      image_extent.width > max_framebuffer_extent.width ||
      image_extent.height > max_framebuffer_extent.height) {
    return VK_NULL_HANDLE;
  }

  // Get the queue family for presentation.
  uint32_t queue_family_index_present = UINT32_MAX;
  const std::vector<VulkanDevice::QueueFamily>& queue_families = vulkan_device->queue_families();
  VkBool32 queue_family_present_supported;
  // First try the graphics and compute queue, prefer it to avoid the concurrent
  // image sharing mode.
  uint32_t queue_family_index_graphics_compute = vulkan_device->queue_family_graphics_compute();
  const VulkanDevice::QueueFamily& queue_family_graphics_compute =
      queue_families[queue_family_index_graphics_compute];
  if (queue_family_graphics_compute.may_support_presentation &&
      !queue_family_graphics_compute.queues.empty() &&
      ifn.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family_index_graphics_compute,
                                               surface,
                                               &queue_family_present_supported) == VK_SUCCESS &&
      queue_family_present_supported) {
    queue_family_index_present = queue_family_index_graphics_compute;
  } else {
    for (uint32_t i = 0; i < uint32_t(queue_families.size()); ++i) {
      const VulkanDevice::QueueFamily& queue_family = queue_families[i];
      if (!queue_family.queues.empty() && queue_family.may_support_presentation &&
          ifn.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface,
                                                   &queue_family_present_supported) == VK_SUCCESS &&
          queue_family_present_supported) {
        queue_family_index_present = i;
        break;
      }
    }
  }
  if (queue_family_index_present == UINT32_MAX) {
    // Not unusable though - may become presentable if the window (with the same
    // surface) moved to a different display, for instance.
    return VK_NULL_HANDLE;
  }

  // TODO(Triang3l): Support transforms.
  if (!(surface_capabilities.supportedTransforms &
        (VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR))) {
    REXLOG_ERROR(
        "VulkanPresenter: The surface doesn't support identity or "
        "window-system-controlled transform");
    return VK_NULL_HANDLE;
  }

  // Get the surface format.
  std::vector<VkSurfaceFormatKHR> surface_formats;
  VkResult surface_formats_get_result;
  for (;;) {
    uint32_t surface_format_count = uint32_t(surface_formats.size());
    bool surface_formats_were_empty = !surface_format_count;
    surface_formats_get_result = ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, surface, &surface_format_count,
        surface_formats_were_empty ? nullptr : surface_formats.data());
    // If the original presentation mode count was 0 (first call), SUCCESS is
    // returned, not INCOMPLETE.
    if (surface_formats_get_result == VK_SUCCESS || surface_formats_get_result == VK_INCOMPLETE) {
      surface_formats.resize(surface_format_count);
      if (surface_formats_get_result == VK_SUCCESS &&
          (!surface_formats_were_empty || !surface_format_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (surface_formats_get_result != VK_SUCCESS) {
    // Assuming any format in case of an error (or as some fallback in case of
    // specification violation).
    surface_formats.clear();
  }
#if REX_PLATFORM_ANDROID
  // Android uses R8G8B8A8.
  static const VkFormat kFormat8888Primary = VK_FORMAT_R8G8B8A8_UNORM;
  static const VkFormat kFormat8888Secondary = VK_FORMAT_B8G8R8A8_UNORM;
#else
  // GNU/Linux X11 and Windows DWM use B8G8R8A8.
  static const VkFormat kFormat8888Primary = VK_FORMAT_B8G8R8A8_UNORM;
  static const VkFormat kFormat8888Secondary = VK_FORMAT_R8G8B8A8_UNORM;
#endif
  VkSurfaceFormatKHR image_format;
  if (surface_formats.empty() ||
      (surface_formats.size() == 1 || surface_formats[0].format == VK_FORMAT_UNDEFINED)) {
    // Can choose any format if the implementation specifies only UNDEFINED.
    image_format.format = kFormat8888Primary;
    image_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  } else {
    // Pick the sRGB 8888 format preferred by the OS, fall back to any sRGB
    // 8888, then to 8888 with an unknown color space, and then to the first
    // sRGB available, and then to the first.
    auto format_8888_primary_it = surface_formats.cend();
    auto format_8888_secondary_it = surface_formats.cend();
    auto any_non_8888_srgb_it = surface_formats.cend();
    for (auto it = surface_formats.cbegin(); it != surface_formats.cend(); ++it) {
      if (it->format != kFormat8888Primary && it->format != kFormat8888Secondary) {
        if (it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            any_non_8888_srgb_it == surface_formats.cend()) {
          any_non_8888_srgb_it = it;
        }
        continue;
      }
      auto& preferred_8888_it =
          it->format == kFormat8888Primary ? format_8888_primary_it : format_8888_secondary_it;
      if (preferred_8888_it == surface_formats.cend()) {
        // First primary or secondary 8888 encounter.
        preferred_8888_it = it;
        continue;
      }
      // Is this a better primary or secondary 8888, that is, this is sRGB,
      // while the previous encounter was not?
      if (it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
          preferred_8888_it->colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        preferred_8888_it = it;
      }
    }
    if (format_8888_primary_it != surface_formats.cend() &&
        format_8888_secondary_it != surface_formats.cend()) {
      // Both the primary and the secondary 8888 formats are available - prefer
      // sRGB, if both are sRGB or not, prefer the primary format.
      if (format_8888_primary_it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
          format_8888_secondary_it->colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        image_format = *format_8888_primary_it;
      } else {
        image_format = *format_8888_secondary_it;
      }
    } else if (format_8888_primary_it != surface_formats.cend()) {
      // Only primary 8888.
      image_format = *format_8888_primary_it;
    } else if (format_8888_secondary_it != surface_formats.cend()) {
      // Only secondary 8888.
      image_format = *format_8888_secondary_it;
    } else if (any_non_8888_srgb_it != surface_formats.cend()) {
      // No 8888, but some sRGB format is available.
      image_format = *any_non_8888_srgb_it;
    } else {
      // Just pick any format.
      image_format = surface_formats.front();
    }
  }

  // Get presentation modes.
  std::vector<VkPresentModeKHR> present_modes;
  VkResult present_modes_get_result;
  for (;;) {
    uint32_t present_mode_count = uint32_t(present_modes.size());
    bool present_modes_were_empty = !present_mode_count;
    present_modes_get_result = ifn.vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, surface, &present_mode_count,
        present_modes_were_empty ? nullptr : present_modes.data());
    // If the original presentation mode count was 0 (first call), SUCCESS is
    // returned, not INCOMPLETE.
    if (present_modes_get_result == VK_SUCCESS || present_modes_get_result == VK_INCOMPLETE) {
      present_modes.resize(present_mode_count);
      if (present_modes_get_result == VK_SUCCESS &&
          (!present_modes_were_empty || !present_mode_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (present_modes_get_result != VK_SUCCESS) {
    // Assuming FIFO only (required everywhere) in case of an error.
    present_modes.clear();
  }

  // Create the swapchain.
  VkSwapchainCreateInfoKHR swapchain_create_info;
  swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchain_create_info.pNext = nullptr;

#if REX_PLATFORM_WIN32
  // On Windows, use VK_EXT_full_screen_exclusive to explicitly disallow
  // fullscreen exclusive mode. This prevents HDR state corruption when
  // entering/exiting fullscreen, as the Windows compositor remains in control
  // of the display state throughout the transition.
  VkSurfaceFullScreenExclusiveInfoEXT full_screen_exclusive_info;
  if (vulkan_device->extensions().ext_EXT_full_screen_exclusive) {
    full_screen_exclusive_info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    full_screen_exclusive_info.pNext = nullptr;
    full_screen_exclusive_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
    swapchain_create_info.pNext = &full_screen_exclusive_info;
  }
#endif
  swapchain_create_info.flags = 0;
  swapchain_create_info.surface = surface;
  swapchain_create_info.minImageCount =
      std::max(kSubmissionCount, surface_capabilities.minImageCount);
  // DLSS frame generation holds the real image while it acquires the
  // generated one (dlss_frame_generation.h).
  if (dlss_frame_generation) {
    swapchain_create_info.minImageCount =
        std::max(swapchain_create_info.minImageCount,
                 surface_capabilities.minImageCount + VulkanDlssFrameGeneration::kExtraSwapchainImages);
  }
  if (surface_capabilities.maxImageCount) {
    swapchain_create_info.minImageCount =
        std::min(swapchain_create_info.minImageCount, surface_capabilities.maxImageCount);
  }
  swapchain_create_info.imageFormat = image_format.format;
  swapchain_create_info.imageColorSpace = image_format.colorSpace;
  swapchain_create_info.imageExtent = image_extent;
  swapchain_create_info.imageArrayLayers = 1;
  swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  // DLSS frame generation copies the real frame out and the generated one in.
  if (dlss_frame_generation &&
      (surface_capabilities.supportedUsageFlags & VulkanDlssFrameGeneration::kSwapchainUsage) ==
          VulkanDlssFrameGeneration::kSwapchainUsage) {
    swapchain_create_info.imageUsage |= VulkanDlssFrameGeneration::kSwapchainUsage;
  }
  uint32_t swapchain_queue_family_indices[2];
  if (queue_family_index_graphics_compute != queue_family_index_present) {
    // Using concurrent sharing mode to avoid an explicit ownership transfer
    // before presenting, which would require an additional command buffer
    // submission with the acquire barrier and a semaphore between the graphics
    // queue and the present queue. Different queues are an extremely rare case,
    // and Xenia only uses the swapchain for the final guest output and the
    // internal UI, so keeping framebuffer compression is not worth the
    // additional submission complexity and possibly latency.
    swapchain_queue_family_indices[0] = queue_family_index_graphics_compute;
    swapchain_queue_family_indices[1] = queue_family_index_present;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swapchain_create_info.queueFamilyIndexCount = 2;
    swapchain_create_info.pQueueFamilyIndices = swapchain_queue_family_indices;
  } else {
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.queueFamilyIndexCount = 0;
    swapchain_create_info.pQueueFamilyIndices = nullptr;
  }
  swapchain_create_info.preTransform =
      (surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
          : VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
  // Prefer opaque to avoid blending in the window system, or let that be
  // specified via the window system if it can't be forced. As a last resort,
  // just pick any - guest output will write alpha of 1 anyway.
  if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  } else {
    uint32_t composite_alpha_shift;
    if (!rex::bit_scan_forward(uint32_t(surface_capabilities.supportedCompositeAlpha),
                               &composite_alpha_shift)) {
      // Against the Vulkan specification, but breaks the logic here.
      REXLOG_ERROR(
          "VulkanPresenter: The surface doesn't support any composite alpha "
          "mode");
      return VK_NULL_HANDLE;
    }
    swapchain_create_info.compositeAlpha =
        VkCompositeAlphaFlagBitsKHR(uint32_t(1) << composite_alpha_shift);
  }
  // As presentation is usually controlled by the GPU command processor, it's
  // better to use modes that allow as quick acquisition as possible to avoid
  // interfering with GPU command processing, and also to allow tearing so
  // variable refresh rate may be used where it's available.
  // Note: If the priorities here are changes, update the cvar descriptions.
  // present_vsync rules out the modes that tear: immediate, and FIFO relaxed
  // for a late frame.
  const bool present_vsync = REXCVAR_GET(present_vsync);
  if (!present_vsync && REXCVAR_GET(vulkan_allow_present_mode_immediate) &&
      std::find(present_modes.cbegin(), present_modes.cend(), VK_PRESENT_MODE_IMMEDIATE_KHR) !=
          present_modes.cend()) {
    // Allowing tearing to reduce latency, and possibly variable refresh rate
    // (though on Windows with borderless fullscreen, GDI copying is used
    // instead of independent flip, so it's not supported there).
    swapchain_create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  } else if (REXCVAR_GET(vulkan_allow_present_mode_mailbox) &&
             std::find(present_modes.cbegin(), present_modes.cend(), VK_PRESENT_MODE_MAILBOX_KHR) !=
                 present_modes.cend()) {
    // Allowing dropping frames to reduce latency, but no tearing.
    swapchain_create_info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  } else if (!present_vsync && REXCVAR_GET(vulkan_allow_present_mode_fifo_relaxed) &&
             std::find(present_modes.cbegin(), present_modes.cend(),
                       VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.cend()) {
    // Limiting the frame rate, but lets too long frames cause tearing not to
    // make the latency even worse.
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
  } else if (REXCVAR_GET(vulkan_allow_present_mode_fifo_latest_ready) &&
             vulkan_device->properties().presentModeFifoLatestReady &&
             std::find(present_modes.cbegin(), present_modes.cend(),
                       VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) != present_modes.cend()) {
    // Still limited to the refresh rate and without tearing, but when frames
    // come faster than the display shows them, the vblank takes the newest
    // finished one instead of the oldest queued: plain FIFO shows every frame,
    // so a game drawing at a rate that is not a divisor of the refresh rate
    // gets a queue that delays and drops frames irregularly.
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  } else {
    // Highest latency (but always guaranteed to be available).
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  }
  swapchain_create_info.clipped = VK_TRUE;
  swapchain_create_info.oldSwapchain = old_swapchain;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  // AMD FSR frame generation: its proxy swapchain, which presents on a queue of
  // its own; the presenter keeps submitting to the graphics queue. Else (or if
  // it fails for this surface) the regular swapchain.
  if (frame_generation && old_swapchain == VK_NULL_HANDLE) {
    std::string reason;
    if (frame_generation->CreateSwapchain(surface, swapchain_create_info, swapchain, reason)) {
      queue_family_index_present = queue_family_index_graphics_compute;
      if (frame_generation_out) {
        *frame_generation_out = true;
      }
    } else {
      swapchain = VK_NULL_HANDLE;
      REXLOG_WARN("VulkanPresenter: frame generation swapchain unavailable, frames are shown as they are: {}",
                  reason);
      VulkanFrameGeneration::WriteStatus(*vulkan_device,
                                         "AMD FSR frame generation unavailable: " + reason);
    }
  }
  if (swapchain == VK_NULL_HANDLE &&
      dfn.vkCreateSwapchainKHR(device, &swapchain_create_info, nullptr, &swapchain) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to create a swapchain");
    return VK_NULL_HANDLE;
  }
  REXLOG_INFO(
      "VulkanPresenter: Created {}x{} swapchain with format {}, color space "
      "{}, presentation mode {}",
      swapchain_create_info.imageExtent.width, swapchain_create_info.imageExtent.height,
      uint32_t(swapchain_create_info.imageFormat), uint32_t(swapchain_create_info.imageColorSpace),
      uint32_t(swapchain_create_info.presentMode));

  present_queue_family_out = queue_family_index_present;
  if (surface_min_image_count_out) {
    *surface_min_image_count_out = surface_capabilities.minImageCount;
  }
  if (image_usage_out) {
    *image_usage_out = swapchain_create_info.imageUsage;
  }
  image_format_out = swapchain_create_info.imageFormat;
  image_extent_out = swapchain_create_info.imageExtent;
  is_fifo_out = swapchain_create_info.presentMode == VK_PRESENT_MODE_FIFO_KHR ||
                swapchain_create_info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
                swapchain_create_info.presentMode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  return swapchain;
}

VkSwapchainKHR VulkanPresenter::PaintContext::PrepareForSwapchainRetirement() {
  // Frame generation presents (and generates) on threads of its own: done
  // before the images it reads go.
  if (swapchain != VK_NULL_HANDLE && swapchain_frame_generation && frame_generation) {
    frame_generation->WaitForPresents();
  }
  // DLSS frame generation's held present first; its feature and images after
  // the GPU work that used them.
  if (swapchain != VK_NULL_HANDLE && swapchain_dlss_frame_generation && dlss_frame_generation) {
    dlss_frame_generation->WaitForHeldPresent();
  }
  if (swapchain != VK_NULL_HANDLE) {
    completion_timeline.AwaitAllSubmissions();
    if (swapchain_dlss_frame_generation && dlss_frame_generation) {
      dlss_frame_generation->DetachSwapchain();
    }
    swapchain_dlss_frame_generation = false;
    // Also wait for the presentation queue since vkQueuePresentKHR doesn't
    // signal a fence, and the present semaphores may still be in use.
    if (present_queue_family != UINT32_MAX) {
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(present_queue_family, 0);
      vulkan_device->functions().vkQueueWaitIdle(queue_acquisition.queue());
    }
  }
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (const SwapchainFramebuffer& framebuffer : swapchain_framebuffers) {
    dfn.vkDestroyFramebuffer(device, framebuffer.framebuffer, nullptr);
    dfn.vkDestroyImageView(device, framebuffer.image_view, nullptr);
  }
  swapchain_framebuffers.clear();
  for (VkSemaphore present_semaphore : swapchain_image_present_semaphores) {
    dfn.vkDestroySemaphore(device, present_semaphore, nullptr);
  }
  swapchain_image_present_semaphores.clear();
  DestroyFrameGenerationHudless();
  swapchain_images.clear();
  swapchain_extent.width = 0;
  swapchain_extent.height = 0;
  // The old swapchain must be destroyed externally.
  VkSwapchainKHR old_swapchain = swapchain;
  swapchain = nullptr;
  return old_swapchain;
}

void VulkanPresenter::PaintContext::DestroyRetiredSwapchain(VkSwapchainKHR old_swapchain,
                                                            bool was_frame_generation) {
  if (old_swapchain == VK_NULL_HANDLE) {
    return;
  }
  if (was_frame_generation && frame_generation) {
    frame_generation->DestroySwapchain(old_swapchain);
  } else {
    vulkan_device->functions().vkDestroySwapchainKHR(vulkan_device->device(), old_swapchain,
                                                     nullptr);
  }
  if (was_frame_generation) {
    swapchain_frame_generation = false;
  }
}

void VulkanPresenter::PaintContext::DestroyFrameGenerationHudless() {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (size_t i = 0; i < 2; ++i) {
    util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, fg_hudless_framebuffer[i]);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, fg_hudless_view[i]);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, fg_hudless_image[i]);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, fg_hudless_memory[i]);
  }
  fg_neutral_depth[0].reset();
  fg_neutral_depth[1].reset();
  fg_neutral_motion.reset();
  fg_neutral_ready = false;
  fg_has_last_generated = false;
  fg_gap_frames = 0;
}

void VulkanPresenter::PaintContext::DestroySwapchainAndVulkanSurface() {
  const bool was_frame_generation = swapchain_frame_generation;
  VkSwapchainKHR old_swapchain = PrepareForSwapchainRetirement();
  DestroyRetiredSwapchain(old_swapchain, was_frame_generation);
  swapchain_frame_generation = false;
  present_queue_family = UINT32_MAX;
  if (vulkan_surface != VK_NULL_HANDLE) {
    const VulkanInstance* vulkan_instance = vulkan_device->vulkan_instance();
    vulkan_instance->functions().vkDestroySurfaceKHR(vulkan_instance->instance(), vulkan_surface,
                                                     nullptr);
    vulkan_surface = VK_NULL_HANDLE;
  }
}

VulkanPresenter::GuestOutputImage::~GuestOutputImage() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (view_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_, nullptr);
  }
  if (image_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, image_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, memory_, nullptr);
  }
}

bool VulkanPresenter::GuestOutputImage::Initialize() {
  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = format_;
  image_create_info.extent.width = extent_.width;
  image_create_info.extent.height = extent_.height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_STORAGE_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image_,
          memory_)) {
    REXLOG_ERROR("VulkanPresenter: Failed to create a guest output image");
    return false;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = image_;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = format_;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_create_info.subresourceRange.baseMipLevel = 0;
  image_view_create_info.subresourceRange.levelCount = 1;
  image_view_create_info.subresourceRange.baseArrayLayer = 0;
  image_view_create_info.subresourceRange.layerCount = 1;
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr, &view_) != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to create a guest output image view");
    return false;
  }

  return true;
}

bool VulkanPresenter::GuestOutputImage::CreateFrameGenerationImages() {
  fg_hudless_ = Create(vulkan_device_, extent_.width, extent_.height, kGuestOutputFormat);
  fg_depth_ = Create(vulkan_device_, extent_.width, extent_.height, kFrameGenerationDepthFormat);
  fg_motion_ = Create(vulkan_device_, extent_.width, extent_.height, kFrameGenerationMotionFormat);
  if (!fg_hudless_ || !fg_depth_ || !fg_motion_) {
    fg_hudless_.reset();
    fg_depth_.reset();
    fg_motion_.reset();
    REXLOG_ERROR("VulkanPresenter: Failed to create the frame generation input images");
    return false;
  }
  return true;
}

bool VulkanPresenter::CreateFrameGenerationHudless() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  paint_context_.DestroyFrameGenerationHudless();
  const VkFormat format = paint_context_.swapchain_render_pass_format;
  if (paint_context_.fg_hudless_render_pass_format != format) {
    util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                               paint_context_.fg_hudless_render_pass);
  }
  if (paint_context_.fg_hudless_render_pass == VK_NULL_HANDLE) {
    // Compatible with the swapchain pass (same format): its pipelines draw here.
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    // Frame generation reads it in compute work queued after this paint.
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 2;
    render_pass_info.pDependencies = dependencies;
    if (dfn.vkCreateRenderPass(device, &render_pass_info, nullptr,
                               &paint_context_.fg_hudless_render_pass) != VK_SUCCESS) {
      paint_context_.fg_hudless_render_pass = VK_NULL_HANDLE;
      return false;
    }
    paint_context_.fg_hudless_render_pass_format = format;
  }
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {paint_context_.swapchain_extent.width, paint_context_.swapchain_extent.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  for (size_t i = 0; i < 2; ++i) {
    if (!util::CreateDedicatedAllocationImage(vulkan_device_, image_info,
                                              util::MemoryPurpose::kDeviceLocal,
                                              paint_context_.fg_hudless_image[i],
                                              paint_context_.fg_hudless_memory[i])) {
      paint_context_.DestroyFrameGenerationHudless();
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = paint_context_.fg_hudless_image[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &paint_context_.fg_hudless_view[i]) !=
        VK_SUCCESS) {
      paint_context_.DestroyFrameGenerationHudless();
      return false;
    }
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = paint_context_.fg_hudless_render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &paint_context_.fg_hudless_view[i];
    framebuffer_info.width = paint_context_.swapchain_extent.width;
    framebuffer_info.height = paint_context_.swapchain_extent.height;
    framebuffer_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                &paint_context_.fg_hudless_framebuffer[i]) != VK_SUCCESS) {
      paint_context_.DestroyFrameGenerationHudless();
      return false;
    }
  }
  for (auto& depth : paint_context_.fg_neutral_depth) {
    depth = GuestOutputImage::Create(vulkan_device_, 8, 8, kFrameGenerationDepthFormat);
  }
  paint_context_.fg_neutral_motion =
      GuestOutputImage::Create(vulkan_device_, 8, 8, kFrameGenerationMotionFormat);
  return true;
}

Presenter::PaintResult VulkanPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  // Begin the submission in place of the one not currently potentially used on
  // the GPU.
  const uint64_t current_paint_submission_index =
      paint_context_.completion_timeline.GetUpcomingSubmission();
  uint64_t paint_submission_count = uint64_t(paint_context_.submissions.size());
  paint_context_.completion_timeline.AwaitMaxSubmissionsPendingAndUpdateCompleted(
      paint_submission_count);
  const PaintContext::Submission& paint_submission =
      *paint_context_.submissions[current_paint_submission_index % paint_submission_count];

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkCommandPool draw_command_pool = paint_submission.draw_command_pool();
  if (dfn.vkResetCommandPool(device, draw_command_pool, 0) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to reset a command buffer for drawing to the "
        "swapchain");
    return PaintResult::kNotPresented;
  }

  VkCommandBuffer draw_command_buffer = paint_submission.draw_command_buffer();
  VkCommandBufferBeginInfo command_buffer_begin_info;
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.pNext = nullptr;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  command_buffer_begin_info.pInheritanceInfo = nullptr;
  if (dfn.vkBeginCommandBuffer(draw_command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to being recording the command buffer for "
        "drawing to the swapchain");
    return PaintResult::kNotPresented;
  }
  // vkResetCommandPool resets from both initial and recording states, still
  // safe to return early from this function in case of an error.

  VkSemaphore acquire_semaphore = paint_submission.acquire_semaphore();

  // WORKAROUND: Wait for presentation queue to be idle to ensure semaphore
  // from previous present is not in use. This prevents
  // VUID-vkQueueSubmit-pSignalSemaphores-00067.
  // The semaphore is unsignaled by vkQueuePresentKHR, not by submission fences,
  // so we must wait for the present queue specifically.
  // See https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
  if (REXCVAR_GET(vulkan_semaphore_reuse_workaround)) {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(paint_context_.present_queue_family, 0);
    dfn.vkQueueWaitIdle(queue_acquisition.queue());
  }

  // AMD FSR frame generation's proxy swapchain (frame_generation.h).
  VulkanFrameGeneration* const frame_generation =
      paint_context_.swapchain_frame_generation ? frame_generation_.get() : nullptr;
  // DLSS frame generation on the regular swapchain (dlss_frame_generation.h):
  // the previous real frame's held present goes first (present order, images).
  VulkanDlssFrameGeneration* const dlss_generation =
      paint_context_.swapchain_dlss_frame_generation ? paint_context_.dlss_frame_generation : nullptr;
  if (dlss_generation) {
    switch (dlss_generation->WaitForHeldPresent()) {
      case VK_SUCCESS:
      case VK_SUBOPTIMAL_KHR:
        break;
      case VK_ERROR_DEVICE_LOST:
        vulkan_device_->SetLost();
        REXLOG_ERROR("VulkanPresenter: Failed to present the held real frame as the device has been lost");
        return PaintResult::kGpuLostResponsible;
      default:
        REXLOG_INFO("VulkanPresenter: The held real frame's present was dropped (outdated swapchain or surface)");
        return PaintResult::kNotPresentedConnectionOutdated;
    }
  }
  const bool any_generation = frame_generation || dlss_generation;
  if (frame_generation && !paint_context_.fg_neutral_ready && paint_context_.fg_neutral_motion &&
      paint_context_.fg_neutral_depth[0] && paint_context_.fg_neutral_depth[1]) {
    const GuestOutputImage* neutral[3] = {paint_context_.fg_neutral_depth[0].get(),
                                          paint_context_.fg_neutral_depth[1].get(),
                                          paint_context_.fg_neutral_motion.get()};
    VkImageMemoryBarrier barriers[3] = {};
    for (size_t i = 0; i < 3; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].image = neutral[i]->image();
      barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    dfn.vkCmdPipelineBarrier(draw_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
    // [0]: far of a forward depth (1), [1]: of an inverted one (0); still motion.
    for (size_t i = 0; i < 3; ++i) {
      VkClearColorValue value = {};
      value.float32[0] = i == 0 ? 1.0f : 0.0f;
      dfn.vkCmdClearColorImage(draw_command_buffer, neutral[i]->image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                               &barriers[i].subresourceRange);
    }
    for (size_t i = 0; i < 3; ++i) {
      barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    dfn.vkCmdPipelineBarrier(draw_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 3,
                             barriers);
    paint_context_.fg_neutral_ready = true;
  }

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  std::shared_ptr<GuestOutputImage> guest_output_image;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      assert_true(guest_output_images_[guest_output_mailbox_index].ever_successfully_refreshed);
      guest_output_image = guest_output_images_[guest_output_mailbox_index].image;
    }
    // Incremented the reference count of the guest output image - safe to leave
    // the consumer critical section now as everything here either will be using
    // the new reference or is exclusively owned by main target painting (and
    // multiple threads can't paint the main target at the same time).
  }

  // Frame generation: this frame's inputs (the refresher's), and what to do
  // with them. The UI (ImGui, drawn by the UI thread) is in the frame but not
  // in the frame without the HUD: generation treats it as UI.
  const FrameGenerationInputs& fg_inputs = guest_output_properties.frame_generation;
  const int32_t fg_debug = any_generation ? REXCVAR_GET(native_frame_generation_debug) : 0;
  const GuestOutputImage* fg_hudless_source =
      guest_output_image ? guest_output_image->frame_generation_hudless() : nullptr;
  const bool fg_inputs_ready =
      any_generation && fg_inputs.valid && fg_hudless_source &&
      guest_output_image->frame_generation_depth() && guest_output_image->frame_generation_motion() &&
      paint_context_.fg_hudless_framebuffer[1] != VK_NULL_HANDLE;
  // Diagnostics 3 and 4: the frame without the HUD (or the inputs drawn into it) instead.
  const bool fg_show_hudless = fg_inputs_ready && fg_debug >= 3;
  bool fg_generate = fg_inputs_ready && !fg_show_hudless && fg_inputs.generate &&
                     fg_inputs.frame != paint_context_.fg_last_frame;
  bool fg_generating = false, fg_began = false;
  VulkanFrameGeneration::Frame fg_frame;
  // DLSS: this frame's inputs, generated after the swapchain pass (the UI drawn).
  bool dlss_pending = false;
  VulkanDlssFrameGeneration::Frame dlss_frame;
  // A frame presented without inputs (outside render passes). While
  // generation is on, for up to a second of such frames (a frame the title
  // didn't describe, a UI repaint), with neutral inputs and a reset rather
  // than switching generation off and on: each switch makes the proxy wait
  // for its presents and restart its present thread.
  auto present_without_inputs = [&]() {
    fg_began = true;
    constexpr uint32_t kGapFrames = 60;
    if (frame_generation->enabled() && !fg_show_hudless && paint_context_.fg_neutral_ready &&
        paint_context_.fg_has_last_generated && paint_context_.fg_gap_frames < kGapFrames) {
      VulkanFrameGeneration::Frame gap = paint_context_.fg_last_generated;
      gap.reset = true;
      gap.command_buffer = draw_command_buffer;
      const GuestOutputImage& depth =
          *paint_context_.fg_neutral_depth[gap.depth_inverted ? 1 : 0];
      gap.depth = depth.image();
      gap.depth_format = depth.format();
      gap.motion = paint_context_.fg_neutral_motion->image();
      gap.motion_format = paint_context_.fg_neutral_motion->format();
      gap.render_width = depth.extent().width;
      gap.render_height = depth.extent().height;
      gap.hudless = VK_NULL_HANDLE;
      if (frame_generation->BeginPresent(paint_context_.swapchain, gap)) {
        ++paint_context_.fg_gap_frames;
        return;
      }
    }
    frame_generation->BeginPresent(paint_context_.swapchain, VulkanFrameGeneration::Frame());
  };
  // Every present is a real frame to generation: while it runs, a repaint
  // without a new guest frame (UI ticks at the display's rate) is dropped,
  // unless nothing was presented for a while (the UI keeps updating).
  if (fg_inputs_ready && !fg_show_hudless && fg_inputs.frame == paint_context_.fg_last_frame &&
      std::chrono::steady_clock::now() - paint_context_.fg_last_real_present <
          std::chrono::milliseconds(50)) {
    return PaintResult::kNotPresented;
  }

  uint32_t swapchain_image_index;
  const auto acquire_start = std::chrono::steady_clock::now();
  VkResult acquire_result =
      frame_generation
          ? frame_generation->AcquireNextImage(paint_context_.swapchain, UINT64_MAX,
                                               acquire_semaphore, &swapchain_image_index)
          : dfn.vkAcquireNextImageKHR(device, paint_context_.swapchain, UINT64_MAX,
                                      acquire_semaphore, VK_NULL_HANDLE, &swapchain_image_index);
  LogBlockingPresentationCall("vkAcquireNextImageKHR", acquire_start, acquire_result);
  switch (acquire_result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
      break;
    case VK_ERROR_DEVICE_LOST:
      vulkan_device_->SetLost();
      REXLOG_ERROR(
          "VulkanPresenter: Failed to acquire the swapchain image as the "
          "device has been lost");
      return PaintResult::kGpuLostResponsible;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      // Not an error, reporting just as info (may normally occur while resizing
      // on some platforms).
      REXLOG_INFO(
          "VulkanPresenter: Presentation to the swapchain image has been "
          "dropped as the swapchain or the surface has become outdated");
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      REXLOG_ERROR("VulkanPresenter: Failed to acquire the swapchain image");
      return PaintResult::kNotPresented;
  }

  // Non-zero extents needed for both the viewport (width must not be zero) and
  // the guest output rectangle.
  assert_not_zero(paint_context_.swapchain_extent.width);
  assert_not_zero(paint_context_.swapchain_extent.height);

  bool swapchain_image_clear_needed = true;
  VkClearAttachment swapchain_image_clear_attachment;
  swapchain_image_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  swapchain_image_clear_attachment.colorAttachment = 0;
  swapchain_image_clear_attachment.clearValue.color.float32[0] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[1] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[2] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[3] = 1.0f;

  VkRenderPassBeginInfo swapchain_render_pass_begin_info;
  swapchain_render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  swapchain_render_pass_begin_info.pNext = nullptr;
  swapchain_render_pass_begin_info.renderPass = paint_context_.swapchain_render_pass;
  swapchain_render_pass_begin_info.framebuffer =
      paint_context_.swapchain_framebuffers[swapchain_image_index].framebuffer;
  swapchain_render_pass_begin_info.renderArea.offset.x = 0;
  swapchain_render_pass_begin_info.renderArea.offset.y = 0;
  swapchain_render_pass_begin_info.renderArea.extent = paint_context_.swapchain_extent;
  swapchain_render_pass_begin_info.clearValueCount = 0;
  swapchain_render_pass_begin_info.pClearValues = nullptr;
  if (paint_context_.swapchain_render_pass_clear_load_op) {
    swapchain_render_pass_begin_info.clearValueCount = 1;
    swapchain_render_pass_begin_info.pClearValues = &swapchain_image_clear_attachment.clearValue;
    swapchain_image_clear_needed = false;
  }
  // The frame without the HUD, painted the same way (cleared, letterbox black).
  VkRenderPassBeginInfo hudless_render_pass_begin_info = swapchain_render_pass_begin_info;
  hudless_render_pass_begin_info.renderPass = paint_context_.fg_hudless_render_pass;
  // The image the last generated frame didn't use.
  const uint32_t fg_hudless_index = paint_context_.fg_hudless_index ^ 1;
  hudless_render_pass_begin_info.framebuffer = paint_context_.fg_hudless_framebuffer[fg_hudless_index];
  hudless_render_pass_begin_info.clearValueCount = 1;
  hudless_render_pass_begin_info.pClearValues = &swapchain_image_clear_attachment.clearValue;

  bool swapchain_image_pass_begun = false;


  if (guest_output_image) {
    VkExtent2D max_framebuffer_extent =
        util::GetMax2DFramebufferExtent(vulkan_device_->properties());
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swapchain_extent.width,
        paint_context_.swapchain_extent.height, max_framebuffer_extent.width,
        max_framebuffer_extent.height, guest_output_paint_config);
    if (guest_output_flow.effect_count) {
      // Store the main target reference to the guest output image so it's not
      // destroyed while it's still potentially in use by main target painting
      // queued on the GPU.
      size_t guest_output_image_paint_ref_index = SIZE_MAX;
      size_t guest_output_image_paint_ref_new_index = SIZE_MAX;
      // Try to find the existing reference to the same image, or an already
      // released (or a taken, but never actually used) slot.
      for (size_t i = 0; i < paint_context_.guest_output_image_paint_refs.size(); ++i) {
        const std::pair<uint64_t, std::shared_ptr<GuestOutputImage>>& guest_output_image_paint_ref =
            paint_context_.guest_output_image_paint_refs[i];
        if (guest_output_image_paint_ref.second == guest_output_image) {
          guest_output_image_paint_ref_index = i;
          break;
        }
        if (guest_output_image_paint_ref_new_index == SIZE_MAX &&
            (!guest_output_image_paint_ref.second || !guest_output_image_paint_ref.first)) {
          guest_output_image_paint_ref_new_index = i;
        }
      }
      if (guest_output_image_paint_ref_index == SIZE_MAX) {
        // New image - store the reference and create the descriptors.
        if (guest_output_image_paint_ref_new_index == SIZE_MAX) {
          // Replace the earliest used reference.
          guest_output_image_paint_ref_new_index = 0;
          for (size_t i = 1; i < paint_context_.guest_output_image_paint_refs.size(); ++i) {
            if (paint_context_.guest_output_image_paint_refs[i].first <
                paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_new_index]
                    .first) {
              guest_output_image_paint_ref_new_index = i;
            }
          }
          // Await the completion of the usage of the old guest output image and
          // its descriptors.
          paint_context_.completion_timeline.AwaitSubmissionAndUpdateCompleted(
              paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_new_index]
                  .first);
        }
        guest_output_image_paint_ref_index = guest_output_image_paint_ref_new_index;
        // The actual submission index will be set if the image is actually
        // used, not dropped due to some error.
        paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_index] =
            std::make_pair(uint64_t(0), guest_output_image);
        // Create the descriptors of the new image (and of its frame
        // generation hudless image, created with it).
        VkDescriptorImageInfo guest_output_image_descriptor_image_infos[2] = {};
        VkWriteDescriptorSet guest_output_image_descriptor_writes[2] = {};
        uint32_t guest_output_image_descriptor_write_count = 0;
        for (uint32_t j = 0; j < 2; ++j) {
          const GuestOutputImage* descriptor_image =
              j ? guest_output_image->frame_generation_hudless() : guest_output_image.get();
          if (!descriptor_image) {
            continue;
          }
          VkDescriptorImageInfo& image_info =
              guest_output_image_descriptor_image_infos[guest_output_image_descriptor_write_count];
          image_info.sampler = VK_NULL_HANDLE;
          image_info.imageView = descriptor_image->view();
          image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          VkWriteDescriptorSet& write =
              guest_output_image_descriptor_writes[guest_output_image_descriptor_write_count++];
          write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write.pNext = nullptr;
          write.dstSet = paint_context_.guest_output_descriptor_sets
              [(j ? PaintContext::kGuestOutputDescriptorSetHudless0Sampled
                  : PaintContext::kGuestOutputDescriptorSetGuestOutput0Sampled) +
               guest_output_image_paint_ref_index];
          write.dstBinding = 0;
          write.dstArrayElement = 0;
          write.descriptorCount = 1;
          write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          write.pImageInfo = &image_info;
          write.pBufferInfo = nullptr;
          write.pTexelBufferView = nullptr;
        }
        dfn.vkUpdateDescriptorSets(device, guest_output_image_descriptor_write_count,
                                   guest_output_image_descriptor_writes, 0, nullptr);
      }

      // Make sure intermediate textures of the needed size are available, and
      // unneeded intermediate textures are destroyed.
      for (size_t i = 0; i < kMaxGuestOutputPaintEffects - 1; ++i) {
        std::pair<uint32_t, uint32_t> intermediate_needed_size(0, 0);
        if (i + 1 < guest_output_flow.effect_count) {
          intermediate_needed_size = guest_output_flow.effect_output_sizes[i];
        }
        std::unique_ptr<GuestOutputImage>& intermediate_image_ptr_ref =
            paint_context_.guest_output_intermediate_images[i];
        VkExtent2D intermediate_current_extent(
            intermediate_image_ptr_ref ? intermediate_image_ptr_ref->extent() : VkExtent2D{});
        if (intermediate_current_extent.width != intermediate_needed_size.first ||
            intermediate_current_extent.height != intermediate_needed_size.second) {
          if (intermediate_needed_size.first && intermediate_needed_size.second) {
            // Need to replace immediately as a new image with the requested
            // size is needed.
            if (intermediate_image_ptr_ref) {
              paint_context_.completion_timeline.AwaitSubmissionAndUpdateCompleted(
                  paint_context_.guest_output_intermediate_image_last_submission);
              intermediate_image_ptr_ref.reset();
              util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         paint_context_.guest_output_intermediate_framebuffers[i]);
            }
            // Image.
            intermediate_image_ptr_ref = GuestOutputImage::Create(
                vulkan_device_, intermediate_needed_size.first, intermediate_needed_size.second);
            if (!intermediate_image_ptr_ref) {
              // Don't display the guest output, and don't try to create more
              // intermediate textures (only destroy them).
              guest_output_flow.effect_count = 0;
              continue;
            }
            // Framebuffer.
            VkImageView intermediate_framebuffer_attachment = intermediate_image_ptr_ref->view();
            VkFramebufferCreateInfo intermediate_framebuffer_create_info;
            intermediate_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            intermediate_framebuffer_create_info.pNext = nullptr;
            intermediate_framebuffer_create_info.flags = 0;
            intermediate_framebuffer_create_info.renderPass =
                guest_output_intermediate_render_pass_;
            intermediate_framebuffer_create_info.attachmentCount = 1;
            intermediate_framebuffer_create_info.pAttachments =
                &intermediate_framebuffer_attachment;
            intermediate_framebuffer_create_info.width = intermediate_needed_size.first;
            intermediate_framebuffer_create_info.height = intermediate_needed_size.second;
            intermediate_framebuffer_create_info.layers = 1;
            if (dfn.vkCreateFramebuffer(
                    device, &intermediate_framebuffer_create_info, nullptr,
                    &paint_context_.guest_output_intermediate_framebuffers[i]) != VK_SUCCESS) {
              REXLOG_ERROR(
                  "VulkanPresenter: Failed to create a guest output "
                  "intermediate framebuffer");
              // Don't display the guest output, and don't try to create more
              // intermediate textures (only destroy them).
              guest_output_flow.effect_count = 0;
              continue;
            }
            // Descriptors.
            VkDescriptorImageInfo intermediate_descriptor_image_info;
            intermediate_descriptor_image_info.sampler = VK_NULL_HANDLE;
            intermediate_descriptor_image_info.imageView = intermediate_image_ptr_ref->view();
            intermediate_descriptor_image_info.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet intermediate_descriptor_write;
            intermediate_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            intermediate_descriptor_write.pNext = nullptr;
            intermediate_descriptor_write.dstSet =
                paint_context_.guest_output_descriptor_sets
                    [PaintContext ::kGuestOutputDescriptorSetIntermediate0Sampled + i];
            intermediate_descriptor_write.dstBinding = 0;
            intermediate_descriptor_write.dstArrayElement = 0;
            intermediate_descriptor_write.descriptorCount = 1;
            intermediate_descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            intermediate_descriptor_write.pImageInfo = &intermediate_descriptor_image_info;
            intermediate_descriptor_write.pBufferInfo = nullptr;
            intermediate_descriptor_write.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &intermediate_descriptor_write, 0, nullptr);
          } else {
            // Was previously needed, but not anymore - destroy when possible.
            if (intermediate_image_ptr_ref &&
                paint_context_.completion_timeline.UpdateAndGetCompletedSubmission() >=
                    paint_context_.guest_output_intermediate_image_last_submission) {
              intermediate_image_ptr_ref.reset();
              util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         paint_context_.guest_output_intermediate_framebuffers[i]);
            }
          }
        }
      }

      if (guest_output_flow.effect_count) {
        // Check if all the intermediate effects are supported by the
        // implementation.
        for (size_t i = 0; i + 1 < guest_output_flow.effect_count; ++i) {
          if (paint_context_.guest_output_paint_pipelines[size_t(guest_output_flow.effects[i])]
                  .intermediate_pipeline == VK_NULL_HANDLE) {
            guest_output_flow.effect_count = 0;
            break;
          }
        }
        if (guest_output_flow.effect_count) {
          // Ensure the pipeline exists for the final effect drawing to the
          // swapchain, for the render pass with the up-to-date image format.
          GuestOutputPaintEffect swapchain_effect =
              guest_output_flow.effects[guest_output_flow.effect_count - 1];
          PaintContext::GuestOutputPaintPipeline& swapchain_effect_pipeline =
              paint_context_.guest_output_paint_pipelines[size_t(swapchain_effect)];
          if (swapchain_effect_pipeline.swapchain_pipeline != VK_NULL_HANDLE &&
              swapchain_effect_pipeline.swapchain_format !=
                  paint_context_.swapchain_render_pass_format) {
            paint_context_.completion_timeline.AwaitSubmissionAndUpdateCompleted(
                paint_context_.guest_output_image_paint_last_submission);
            util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                       swapchain_effect_pipeline.swapchain_pipeline);
          }
          if (swapchain_effect_pipeline.swapchain_pipeline == VK_NULL_HANDLE) {
            assert_true(CanGuestOutputPaintEffectBeFinal(swapchain_effect));
            assert_true(guest_output_paint_fs_[size_t(swapchain_effect)] != VK_NULL_HANDLE);
            swapchain_effect_pipeline.swapchain_pipeline = CreateGuestOutputPaintPipeline(
                swapchain_effect, paint_context_.swapchain_render_pass);
            if (swapchain_effect_pipeline.swapchain_pipeline == VK_NULL_HANDLE) {
              guest_output_flow.effect_count = 0;
            } else {
              // Remember the format the pipeline was created for, otherwise it's
              // considered outdated, awaited and recreated on every frame.
              swapchain_effect_pipeline.swapchain_format =
                  paint_context_.swapchain_render_pass_format;
            }
          }
        }
      }

      if (guest_output_flow.effect_count) {
        // Actually draw the guest output.
        paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_index].first =
            current_paint_submission_index;
        paint_context_.guest_output_image_paint_last_submission = current_paint_submission_index;
        if (guest_output_flow.effect_count > 1) {
          paint_context_.guest_output_intermediate_image_last_submission =
              current_paint_submission_index;
        }
        [[maybe_unused]] bool temporal_effect_selected = false;
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
        temporal_effect_selected =
            guest_output_paint_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
            guest_output_paint_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3;
#endif
        // The effect chain from `source_image` (`source_set` its descriptors)
        // into the swapchain image (its pass stays open for the UI), or into
        // the frame generation hudless image (the frame without the HUD,
        // painted identically).
        auto paint_guest_output = [&](const GuestOutputImage& source_image,
                                      size_t source_set, bool to_hudless) {
          VkViewport guest_output_viewport;
          guest_output_viewport.x = 0.0f;
          guest_output_viewport.y = 0.0f;
          guest_output_viewport.minDepth = 0.0f;
          guest_output_viewport.maxDepth = 1.0f;
          VkRect2D guest_output_scissor;
          guest_output_scissor.offset.x = 0;
          guest_output_scissor.offset.y = 0;
          for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
            bool is_final_effect = i + 1 >= guest_output_flow.effect_count;
            GuestOutputPaintEffect effect = guest_output_flow.effects[i];

            std::pair<uint32_t, uint32_t> effect_rect_size =
                guest_output_flow.effect_output_sizes[i];
            if (is_final_effect && guest_output_flow.output_x == 0 &&
                guest_output_flow.output_y == 0 &&
                effect_rect_size.first < paint_context_.swapchain_extent.width &&
                effect_rect_size.second < paint_context_.swapchain_extent.height) {
              static std::atomic<bool> logged_swapchain_expand = false;
              if (!logged_swapchain_expand.exchange(true)) {
                REXLOG_WARN(
                    "VulkanPresenter: expanding final guest output rect from "
                    "{}x{} to swapchain {}x{} to avoid top-left-only output",
                    effect_rect_size.first, effect_rect_size.second,
                    paint_context_.swapchain_extent.width, paint_context_.swapchain_extent.height);
              }
              effect_rect_size = std::make_pair(paint_context_.swapchain_extent.width,
                                                paint_context_.swapchain_extent.height);
              // Keep all final-pass data coherent - shader constants and
              // letterbox clears are derived from guest_output_flow.
              guest_output_flow.effect_output_sizes[i] = effect_rect_size;
              guest_output_flow.output_x = 0;
              guest_output_flow.output_y = 0;
              guest_output_flow.letterbox_clear_rectangle_count = 0;
            }
            if (guest_output_paint_config.GetEffect() == GuestOutputPaintConfig::Effect::kNis &&
                effect == GuestOutputPaintEffect::kFsrEasu && !is_final_effect) {
              const GuestOutputImage& source =
                  i ? *paint_context_.guest_output_intermediate_images[i - 1] : source_image;
              uint32_t effect_input_width = 0, effect_input_height = 0;
              guest_output_flow.GetEffectInputSize(i, effect_input_width, effect_input_height);
              if (DispatchNisUpscaler(draw_command_buffer, current_paint_submission_index, source,
                                      effect_input_width, effect_input_height,
                                      *paint_context_.guest_output_intermediate_images[i],
                                      effect_rect_size.first, effect_rect_size.second,
                                      guest_output_paint_config)) {
                continue;
              }
            }
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
            bool use_temporal_upscaler =
                temporal_effect_selected && effect == GuestOutputPaintEffect::kFsrEasu;
            if (use_temporal_upscaler && !is_final_effect) {
              VkImage temporal_source_image =
                  i ? paint_context_.guest_output_intermediate_images[i - 1]->image()
                    : source_image.image();
              VkImage output_image = paint_context_.guest_output_intermediate_images[i]->image();

              VkImageMemoryBarrier barrier_to_uav = {};
              barrier_to_uav.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              barrier_to_uav.srcAccessMask = 0;
              barrier_to_uav.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
              barrier_to_uav.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
              barrier_to_uav.newLayout = VK_IMAGE_LAYOUT_GENERAL;
              barrier_to_uav.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              barrier_to_uav.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              barrier_to_uav.image = output_image;
              barrier_to_uav.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
              barrier_to_uav.subresourceRange.baseMipLevel = 0;
              barrier_to_uav.subresourceRange.levelCount = 1;
              barrier_to_uav.subresourceRange.baseArrayLayer = 0;
              barrier_to_uav.subresourceRange.layerCount = 1;
              dfn.vkCmdPipelineBarrier(draw_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &barrier_to_uav);

              uint32_t effect_input_width = 0, effect_input_height = 0;
              guest_output_flow.GetEffectInputSize(i, effect_input_width, effect_input_height);
              if (DispatchTemporalUpscaler(draw_command_buffer, temporal_source_image,
                                           effect_input_width, effect_input_height, output_image,
                                           effect_rect_size.first, effect_rect_size.second,
                                           guest_output_paint_config)) {
                VkImageMemoryBarrier barrier_to_shader_read = {};
                barrier_to_shader_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier_to_shader_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier_to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier_to_shader_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier_to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier_to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier_to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier_to_shader_read.image = output_image;
                barrier_to_shader_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier_to_shader_read.subresourceRange.baseMipLevel = 0;
                barrier_to_shader_read.subresourceRange.levelCount = 1;
                barrier_to_shader_read.subresourceRange.baseArrayLayer = 0;
                barrier_to_shader_read.subresourceRange.layerCount = 1;
                dfn.vkCmdPipelineBarrier(
                    draw_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier_to_shader_read);
                continue;
              }
            }
#endif

            int32_t effect_rect_x, effect_rect_y;
            if (is_final_effect) {
              effect_rect_x = guest_output_flow.output_x;
              effect_rect_y = guest_output_flow.output_y;
              if (to_hudless) {
                dfn.vkCmdBeginRenderPass(draw_command_buffer, &hudless_render_pass_begin_info,
                                         VK_SUBPASS_CONTENTS_INLINE);
              } else {
                dfn.vkCmdBeginRenderPass(draw_command_buffer, &swapchain_render_pass_begin_info,
                                         VK_SUBPASS_CONTENTS_INLINE);
                swapchain_image_pass_begun = true;
              }
              guest_output_viewport.width = float(paint_context_.swapchain_extent.width);
              guest_output_viewport.height = float(paint_context_.swapchain_extent.height);
              guest_output_scissor.extent.width = paint_context_.swapchain_extent.width;
              guest_output_scissor.extent.height = paint_context_.swapchain_extent.height;
            } else {
              effect_rect_x = 0;
              effect_rect_y = 0;
              VkRenderPassBeginInfo intermediate_render_pass_begin_info;
              intermediate_render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
              intermediate_render_pass_begin_info.pNext = nullptr;
              intermediate_render_pass_begin_info.renderPass =
                  guest_output_intermediate_render_pass_;
              intermediate_render_pass_begin_info.framebuffer =
                  paint_context_.guest_output_intermediate_framebuffers[i];
              intermediate_render_pass_begin_info.renderArea.offset.x = 0;
              intermediate_render_pass_begin_info.renderArea.offset.y = 0;
              intermediate_render_pass_begin_info.renderArea.extent.width = effect_rect_size.first;
              intermediate_render_pass_begin_info.renderArea.extent.height =
                  effect_rect_size.second;
              intermediate_render_pass_begin_info.clearValueCount = 0;
              intermediate_render_pass_begin_info.pClearValues = nullptr;
              dfn.vkCmdBeginRenderPass(draw_command_buffer, &intermediate_render_pass_begin_info,
                                       VK_SUBPASS_CONTENTS_INLINE);
              guest_output_viewport.width = float(effect_rect_size.first);
              guest_output_viewport.height = float(effect_rect_size.second);
              guest_output_scissor.extent.width = effect_rect_size.first;
              guest_output_scissor.extent.height = effect_rect_size.second;
            }
            dfn.vkCmdSetViewport(draw_command_buffer, 0, 1, &guest_output_viewport);
            dfn.vkCmdSetScissor(draw_command_buffer, 0, 1, &guest_output_scissor);

            const PaintContext::GuestOutputPaintPipeline& effect_pipeline =
                paint_context_.guest_output_paint_pipelines[size_t(effect)];
            // The hudless pass is compatible with the swapchain's (same format).
            VkPipeline effect_vulkan_pipeline = is_final_effect
                                                    ? effect_pipeline.swapchain_pipeline
                                                    : effect_pipeline.intermediate_pipeline;
            assert_true(effect_vulkan_pipeline != VK_NULL_HANDLE);
            dfn.vkCmdBindPipeline(draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  effect_vulkan_pipeline);

            GuestOutputPaintPipelineLayoutIndex guest_output_paint_pipeline_layout_index =
                GetGuestOutputPaintPipelineLayoutIndex(effect);
            VkPipelineLayout guest_output_paint_pipeline_layout =
                guest_output_paint_pipeline_layouts_[guest_output_paint_pipeline_layout_index];

            size_t effect_src_descriptor_set =
                i ? PaintContext::kGuestOutputDescriptorSetIntermediate0Sampled + (i - 1)
                  : source_set;
            dfn.vkCmdBindDescriptorSets(
                draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                guest_output_paint_pipeline_layout, 0, 1,
                &paint_context_.guest_output_descriptor_sets[effect_src_descriptor_set], 0,
                nullptr);

            GuestOutputPaintRectangleConstants effect_rect_constants;
            float effect_x_to_ndc = 2.0f / guest_output_viewport.width;
            float effect_y_to_ndc = 2.0f / guest_output_viewport.height;
            effect_rect_constants.x = -1.0f + float(effect_rect_x) * effect_x_to_ndc;
            effect_rect_constants.y = -1.0f + float(effect_rect_y) * effect_y_to_ndc;
            effect_rect_constants.width = float(effect_rect_size.first) * effect_x_to_ndc;
            effect_rect_constants.height = float(effect_rect_size.second) * effect_y_to_ndc;
            dfn.vkCmdPushConstants(draw_command_buffer, guest_output_paint_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(effect_rect_constants),
                                   &effect_rect_constants);

            uint32_t effect_constants_size = 0;
            union {
              BilinearConstants bilinear;
              CasSharpenConstants cas_sharpen;
              CasResampleConstants cas_resample;
              FsrEasuConstants fsr_easu;
              FsrRcasConstants fsr_rcas;
            } effect_constants;
            switch (guest_output_paint_pipeline_layout_index) {
              case kGuestOutputPaintPipelineLayoutIndexBilinear: {
                effect_constants_size = sizeof(effect_constants.bilinear);
                effect_constants.bilinear.Initialize(guest_output_flow, i);
              } break;
              case kGuestOutputPaintPipelineLayoutIndexCasSharpen: {
                effect_constants_size = sizeof(effect_constants.cas_sharpen);
                effect_constants.cas_sharpen.Initialize(guest_output_flow, i,
                                                        guest_output_paint_config);
              } break;
              case kGuestOutputPaintPipelineLayoutIndexCasResample: {
                effect_constants_size = sizeof(effect_constants.cas_resample);
                effect_constants.cas_resample.Initialize(guest_output_flow, i,
                                                         guest_output_paint_config);
              } break;
              case kGuestOutputPaintPipelineLayoutIndexFsrEasu: {
                effect_constants_size = sizeof(effect_constants.fsr_easu);
                effect_constants.fsr_easu.Initialize(guest_output_flow, i);
              } break;
              case kGuestOutputPaintPipelineLayoutIndexFsrRcas: {
                effect_constants_size = sizeof(effect_constants.fsr_rcas);
                effect_constants.fsr_rcas.Initialize(guest_output_flow, i,
                                                     guest_output_paint_config);
              } break;
              default:
                break;
            }
            if (effect_constants_size) {
              dfn.vkCmdPushConstants(draw_command_buffer, guest_output_paint_pipeline_layout,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(effect_rect_constants),
                                     effect_constants_size, &effect_constants);
            }

            dfn.vkCmdDraw(draw_command_buffer, 4, 1, 0, 0);

            if (is_final_effect && to_hudless) {
              // Cleared at the pass's load; left for frame generation to read.
              dfn.vkCmdEndRenderPass(draw_command_buffer);
            } else if (is_final_effect) {
              // Clear the letterbox around the guest output if the guest output
              // doesn't cover the entire swapchain image.
              if (swapchain_image_clear_needed &&
                  guest_output_flow.letterbox_clear_rectangle_count) {
                VkClearRect
                    letterbox_clear_vulkan_rectangles[GuestOutputPaintFlow::kMaxClearRectangles];
                for (size_t j = 0; j < guest_output_flow.letterbox_clear_rectangle_count; ++j) {
                  VkClearRect& letterbox_clear_vulkan_rectangle =
                      letterbox_clear_vulkan_rectangles[j];
                  const GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle =
                      guest_output_flow.letterbox_clear_rectangles[j];
                  letterbox_clear_vulkan_rectangle.rect.offset.x =
                      int32_t(letterbox_clear_rectangle.x);
                  letterbox_clear_vulkan_rectangle.rect.offset.y =
                      int32_t(letterbox_clear_rectangle.y);
                  letterbox_clear_vulkan_rectangle.rect.extent.width =
                      letterbox_clear_rectangle.width;
                  letterbox_clear_vulkan_rectangle.rect.extent.height =
                      letterbox_clear_rectangle.height;
                  letterbox_clear_vulkan_rectangle.baseArrayLayer = 0;
                  letterbox_clear_vulkan_rectangle.layerCount = 1;
                }
                dfn.vkCmdClearAttachments(
                    draw_command_buffer, 1, &swapchain_image_clear_attachment,
                    uint32_t(guest_output_flow.letterbox_clear_rectangle_count),
                    letterbox_clear_vulkan_rectangles);
              }
              swapchain_image_clear_needed = false;
            } else {
              // Still need the swapchain pass to be open for UI drawing.
              dfn.vkCmdEndRenderPass(draw_command_buffer);
            }
          }
        };

        const size_t guest_output_set =
            PaintContext::kGuestOutputDescriptorSetGuestOutput0Sampled +
            guest_output_image_paint_ref_index;
        const size_t hudless_set = PaintContext::kGuestOutputDescriptorSetHudless0Sampled +
                                   guest_output_image_paint_ref_index;
        // Scalers that keep per-paint state (NIS, the FidelityFX source build's
        // temporal pass) run once per paint: no second, hudless chain.
        if (fg_generate && (temporal_effect_selected ||
                            guest_output_paint_config.GetEffect() ==
                                GuestOutputPaintConfig::Effect::kNis)) {
          fg_generate = false;
        }
        if (fg_generate) {
          // The frame without the HUD first, then Prepare (outside passes).
          paint_guest_output(*fg_hudless_source, hudless_set, true);
          const auto& final_size =
              guest_output_flow.effect_output_sizes[guest_output_flow.effect_count - 1];
          const GuestOutputImage& fg_depth = *guest_output_image->frame_generation_depth();
          fg_frame.generate = true;
          fg_frame.reset = fg_inputs.reset || paint_context_.fg_last_frame + 1 != fg_inputs.frame;
          fg_frame.command_buffer = draw_command_buffer;
          fg_frame.depth = fg_depth.image();
          fg_frame.depth_format = fg_depth.format();
          fg_frame.motion = guest_output_image->frame_generation_motion()->image();
          fg_frame.motion_format = guest_output_image->frame_generation_motion()->format();
          fg_frame.render_width = fg_depth.extent().width;
          fg_frame.render_height = fg_depth.extent().height;
          fg_frame.hudless = paint_context_.fg_hudless_image[fg_hudless_index];
          fg_frame.backbuffer_format = paint_context_.swapchain_render_pass_format;
          fg_frame.display_width = paint_context_.swapchain_extent.width;
          fg_frame.display_height = paint_context_.swapchain_extent.height;
          fg_frame.rect_x = guest_output_flow.output_x;
          fg_frame.rect_y = guest_output_flow.output_y;
          fg_frame.rect_width = final_size.first;
          fg_frame.rect_height = final_size.second;
          fg_frame.jitter_x = fg_inputs.jitter_x;
          fg_frame.jitter_y = fg_inputs.jitter_y;
          fg_frame.delta_ms = fg_inputs.delta_ms;
          fg_frame.camera_near = fg_inputs.camera_near;
          fg_frame.camera_far = fg_inputs.camera_far;
          fg_frame.fov_y = fg_inputs.fov_y;
          fg_frame.meters_per_unit = fg_inputs.meters_per_unit;
          fg_frame.depth_inverted = fg_inputs.depth_inverted;
          fg_frame.depth_infinite = fg_inputs.depth_infinite;
          fg_frame.position = fg_inputs.position;
          fg_frame.up = fg_inputs.up;
          fg_frame.right = fg_inputs.right;
          fg_frame.forward = fg_inputs.forward;
          if (frame_generation) {
            fg_generating = frame_generation->BeginPresent(paint_context_.swapchain, fg_frame);
          } else {
            // DLSS: generated after the swapchain pass (below), from the final
            // frame: the guest output's rectangle in the backbuffer, with the
            // depth and motion images covering it (a fixed size: letterbox
            // bars moving in the scene don't re-create the feature).
            dlss_frame.depth = fg_frame.depth;
            dlss_frame.depth_view = fg_depth.view();
            dlss_frame.depth_format = fg_frame.depth_format;
            dlss_frame.motion = fg_frame.motion;
            dlss_frame.motion_view = guest_output_image->frame_generation_motion()->view();
            dlss_frame.motion_format = fg_frame.motion_format;
            dlss_frame.render_width = fg_frame.render_width;
            dlss_frame.render_height = fg_frame.render_height;
            dlss_frame.hudless = fg_frame.hudless;
            dlss_frame.hudless_view = paint_context_.fg_hudless_view[fg_hudless_index];
            dlss_frame.rect_x = fg_frame.rect_x;
            dlss_frame.rect_y = fg_frame.rect_y;
            dlss_frame.rect_width = fg_frame.rect_width;
            dlss_frame.rect_height = fg_frame.rect_height;
            dlss_frame.jitter_x = fg_inputs.jitter_x;
            dlss_frame.jitter_y = fg_inputs.jitter_y;
            // History across frames without inputs: by the frame the motion
            // refers to (dlss_frame_generation.h), not by consecutive frames.
            dlss_frame.reset = fg_inputs.reset;
            dlss_frame.frame = fg_inputs.frame;
            dlss_frame.previous_frame = fg_inputs.previous_frame;
            dlss_frame.depth_inverted = fg_inputs.depth_inverted;
            dlss_frame.view_to_clip = fg_inputs.view_to_clip;
            dlss_frame.clip_to_view = fg_inputs.clip_to_view;
            dlss_frame.clip_to_previous_clip = fg_inputs.clip_to_previous_clip;
            dlss_frame.previous_clip_to_clip = fg_inputs.previous_clip_to_clip;
            dlss_frame.position = fg_inputs.position;
            dlss_frame.up = fg_inputs.up;
            dlss_frame.right = fg_inputs.right;
            dlss_frame.forward = fg_inputs.forward;
            dlss_frame.camera_near = fg_inputs.camera_near;
            dlss_frame.camera_far = fg_inputs.camera_far;
            // The frame's field of view, as its matrices (a letterboxed scene's is narrower).
            const float projection_y = std::abs(fg_inputs.view_to_clip[5]);
            dlss_frame.fov_y = projection_y > 1e-6f ? 2.f * std::atan(1.f / projection_y) : fg_inputs.fov_y;
            dlss_frame.aspect = fg_inputs.aspect;
            dlss_frame.meters_per_unit = fg_inputs.meters_per_unit;
            dlss_pending = dlss_frame.rect_width && dlss_frame.rect_height;
            paint_context_.fg_hudless_index = fg_hudless_index;
          }
          fg_began = true;
          paint_context_.fg_last_frame = fg_inputs.frame;
          if (fg_generating) {
            paint_context_.fg_hudless_index = fg_hudless_index;
            paint_context_.fg_last_generated = fg_frame;
            paint_context_.fg_has_last_generated = true;
            paint_context_.fg_gap_frames = 0;
          }
        } else if (frame_generation) {
          present_without_inputs();
        }
        if (fg_show_hudless) {
          paint_guest_output(*fg_hudless_source, hudless_set, false);
        } else {
          paint_guest_output(*guest_output_image, guest_output_set, false);
        }
      }
    }
  }
  if (frame_generation && !fg_began) {
    // No guest output painted: Prepare can still be recorded (no pass is open).
    if (!swapchain_image_pass_begun) {
      present_without_inputs();
    } else {
      frame_generation->BeginPresent(paint_context_.swapchain, VulkanFrameGeneration::Frame());
    }
  }

  // Release main target guest output image references that aren't needed
  // anymore (this is done after various potential guest-output-related main
  // target completion timeline waits so the completed submission index is the
  // most actual).
  uint64_t completed_paint_submission =
      paint_context_.completion_timeline.UpdateAndGetCompletedSubmission();
  for (std::pair<uint64_t, std::shared_ptr<GuestOutputImage>>& guest_output_image_paint_ref :
       paint_context_.guest_output_image_paint_refs) {
    if (!guest_output_image_paint_ref.second ||
        guest_output_image_paint_ref.second == guest_output_image) {
      continue;
    }
    if (completed_paint_submission >= guest_output_image_paint_ref.first) {
      guest_output_image_paint_ref.second.reset();
    }
  }

  // If hasn't presented the guest output, begin the pass to clear and, if
  // needed, to draw the UI.
  if (!swapchain_image_pass_begun) {
    dfn.vkCmdBeginRenderPass(draw_command_buffer, &swapchain_render_pass_begin_info,
                             VK_SUBPASS_CONTENTS_INLINE);
  }
  if (swapchain_image_clear_needed) {
    VkClearRect swapchain_image_clear_rectangle;
    swapchain_image_clear_rectangle.rect.offset.x = 0;
    swapchain_image_clear_rectangle.rect.offset.y = 0;
    swapchain_image_clear_rectangle.rect.extent = paint_context_.swapchain_extent;
    swapchain_image_clear_rectangle.baseArrayLayer = 0;
    swapchain_image_clear_rectangle.layerCount = 1;
    dfn.vkCmdClearAttachments(draw_command_buffer, 1, &swapchain_image_clear_attachment, 1,
                              &swapchain_image_clear_rectangle);
    swapchain_image_clear_needed = false;
  }

  if (execute_ui_drawers) {
    // Draw the UI.
    VulkanUIDrawContext ui_draw_context(
        *this, paint_context_.swapchain_extent.width, paint_context_.swapchain_extent.height,
        draw_command_buffer, ui_completion_timeline_.GetUpcomingSubmission(),
        ui_completion_timeline_.UpdateAndGetCompletedSubmission(),
        paint_context_.swapchain_render_pass, paint_context_.swapchain_render_pass_format);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  dfn.vkCmdEndRenderPass(draw_command_buffer);

  // DLSS frame generation: the final frame (the UI drawn) is the real one; a
  // second image gets the generated frame, presented first.
  uint32_t dlss_generated_index = UINT32_MAX;
  VkSemaphore dlss_acquire_semaphore = VK_NULL_HANDLE;
  if (dlss_generation) {
    if (dlss_pending) {
      // Other sizes: a new feature, after the GPU work using the old one.
      if (!dlss_generation->FeatureMatches(dlss_frame)) {
        paint_context_.completion_timeline.AwaitAllSubmissions();
        dlss_generation->ReleaseFeature();
      }
      dlss_acquire_semaphore = dlss_generation->acquire_semaphore(size_t(current_paint_submission_index));
      // The swapchain has images for both (kExtraSwapchainImages); a short
      // wait at most, else this frame goes without.
      const VkResult second_acquire = dfn.vkAcquireNextImageKHR(
          device, paint_context_.swapchain, 50'000'000, dlss_acquire_semaphore, VK_NULL_HANDLE,
          &dlss_generated_index);
      if ((second_acquire == VK_SUCCESS || second_acquire == VK_SUBOPTIMAL_KHR) &&
          dlss_generated_index != swapchain_image_index &&
          dlss_generated_index < paint_context_.swapchain_images.size()) {
        fg_generating = dlss_generation->Record(draw_command_buffer, dlss_frame,
                                                paint_context_.swapchain_images[swapchain_image_index],
                                                paint_context_.swapchain_images[dlss_generated_index]);
      } else {
        dlss_generated_index = UINT32_MAX;
        dlss_acquire_semaphore = VK_NULL_HANDLE;
        dlss_generation->NoteFrameWithoutGeneration();
      }
    } else {
      dlss_generation->NoteFrameWithoutGeneration();
    }
  }

  dfn.vkEndCommandBuffer(draw_command_buffer);

  // Generation on in the proxy: the refresher holds the real frame rate.
  if (frame_generation) {
    frame_generation_generating_.store(frame_generation->enabled(), std::memory_order_relaxed);
  }
  // DLSS: while frames were generated in the last second (a frame without
  // inputs doesn't release the cap at once).
  if (dlss_generation) {
    const auto now = std::chrono::steady_clock::now();
    frame_generation_generating_.store(
        fg_generating || now - paint_context_.fg_last_real_present < std::chrono::seconds(1),
        std::memory_order_relaxed);
    if (fg_generating) {
      paint_context_.fg_last_real_present = now;
      ++paint_context_.fg_stats_generating;
    }
    ++paint_context_.fg_stats_real;
    if (paint_context_.fg_stats_since == std::chrono::steady_clock::time_point{}) {
      paint_context_.fg_stats_since = now;
      paint_context_.fg_stats_presents_base = dlss_generation->generated_frames();
      paint_context_.fg_stats_held_base = dlss_generation->held_presents();
      paint_context_.fg_stats_resets_base = dlss_generation->resets();
    } else if (now - paint_context_.fg_stats_since >= std::chrono::seconds(10)) {
      const double seconds = std::chrono::duration<double>(now - paint_context_.fg_stats_since).count();
      const uint64_t generated = dlss_generation->generated_frames();
      const uint64_t held = dlss_generation->held_presents();
      const uint64_t resets = dlss_generation->resets();
      // Shown: every real frame, plus a second image per held real present.
      const uint64_t shown = paint_context_.fg_stats_real + (held - paint_context_.fg_stats_held_base);
      REXLOG_INFO(
          "Frame generation (DLSS): {:.1f} real FPS, {:.1f} shown, {} of {} real frames generated ({} new "
          "histories), display {:.0f} Hz, {}; histories in total: {} producer reset, {} after an error, {} "
          "without a previous frame, {} after a frame without generation{}{}",
          double(paint_context_.fg_stats_real) / seconds, double(shown) / seconds,
          generated - paint_context_.fg_stats_presents_base, paint_context_.fg_stats_real,
          resets - paint_context_.fg_stats_resets_base,
          display_refresh_rate(), paint_context_.swapchain_is_fifo ? "FIFO" : "timed pacing",
          dlss_generation->reset_reasons()[0], dlss_generation->reset_reasons()[1],
          dlss_generation->reset_reasons()[2], dlss_generation->reset_reasons()[3],
          dlss_generation->last_error().empty() ? "" : ", last issue: ", dlss_generation->last_error());
      paint_context_.fg_stats_since = now;
      paint_context_.fg_stats_presents_base = generated;
      paint_context_.fg_stats_held_base = held;
      paint_context_.fg_stats_resets_base = resets;
      paint_context_.fg_stats_real = paint_context_.fg_stats_generating = 0;
    }
  }
  // Frame generation statistics (the real frame rate is held by the refresher).
  if (frame_generation) {
    const auto now = std::chrono::steady_clock::now();
    if (fg_generating) {
      paint_context_.fg_last_real_present = now;
      ++paint_context_.fg_stats_generating;
    }
    ++paint_context_.fg_stats_real;
    if (paint_context_.fg_stats_since == std::chrono::steady_clock::time_point{}) {
      paint_context_.fg_stats_since = now;
      paint_context_.fg_stats_presents_base = frame_generation->PresentCount(paint_context_.swapchain);
    } else if (now - paint_context_.fg_stats_since >= std::chrono::seconds(10)) {
      const double seconds = std::chrono::duration<double>(now - paint_context_.fg_stats_since).count();
      const uint64_t presents = frame_generation->PresentCount(paint_context_.swapchain);
      REXLOG_INFO(
          "Frame generation: {:.1f} real FPS, {:.1f} shown, {} of {} real frames with generation, "
          "{} on/off switches in total, display {:.0f} Hz{}{}",
          double(paint_context_.fg_stats_real) / seconds,
          double(presents - paint_context_.fg_stats_presents_base) / seconds,
          paint_context_.fg_stats_generating, paint_context_.fg_stats_real,
          frame_generation->enable_switches(), display_refresh_rate(),
          frame_generation->last_error().empty() ? "" : ", last issue: ",
          frame_generation->last_error());
      paint_context_.fg_stats_since = now;
      paint_context_.fg_stats_presents_base = presents;
      paint_context_.fg_stats_real = paint_context_.fg_stats_generating = 0;
    }
  }

  VkPipelineStageFlags acquire_semaphore_wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkCommandBuffer command_buffers[2];
  uint32_t command_buffer_count = 0;
  // UI setup command buffers must be accessed only if execute_ui_drawers is not
  // null, to identify the UI thread.
  size_t ui_setup_command_buffer_index =
      execute_ui_drawers ? paint_context_.ui_setup_command_buffer_current_index : SIZE_MAX;
  if (ui_setup_command_buffer_index != SIZE_MAX) {
    PaintContext::UISetupCommandBuffer& ui_setup_command_buffer =
        paint_context_.ui_setup_command_buffers[ui_setup_command_buffer_index];
    dfn.vkEndCommandBuffer(ui_setup_command_buffer.command_buffer);
    command_buffers[command_buffer_count++] = ui_setup_command_buffer.command_buffer;
    // Release the current UI setup command buffer regardless of submission
    // result. Failed submissions (if the UI submission index wasn't incremented
    // since the previous draw) should be handled by UI drawers themselves by
    // retrying all the failed work if needed.
    paint_context_.ui_setup_command_buffer_current_index = SIZE_MAX;
  }
  command_buffers[command_buffer_count++] = draw_command_buffer;
  VkSemaphore present_semaphore =
      paint_context_.swapchain_image_present_semaphores[swapchain_image_index];
  // DLSS: the generated image's acquisition (awaited by its copy) and present too.
  const bool dlss_two_images = dlss_generated_index != UINT32_MAX;
  const VkSemaphore wait_semaphores[2] = {acquire_semaphore, dlss_acquire_semaphore};
  const VkPipelineStageFlags wait_stages[2] = {acquire_semaphore_wait_stage, VK_PIPELINE_STAGE_TRANSFER_BIT};
  const VkSemaphore signal_semaphores[2] = {
      present_semaphore, dlss_two_images
                             ? paint_context_.swapchain_image_present_semaphores[dlss_generated_index]
                             : VK_NULL_HANDLE};
  VkSubmitInfo submit_info;
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = nullptr;
  submit_info.waitSemaphoreCount = dlss_two_images ? 2 : 1;
  submit_info.pWaitSemaphores = wait_semaphores;
  submit_info.pWaitDstStageMask = wait_stages;
  submit_info.commandBufferCount = command_buffer_count;
  submit_info.pCommandBuffers = command_buffers;
  submit_info.signalSemaphoreCount = dlss_two_images ? 2 : 1;
  submit_info.pSignalSemaphores = signal_semaphores;
  const VkResult submit_result = paint_context_.completion_timeline.AcquireFenceAndSubmit(
      vulkan_device_->queue_family_graphics_compute(), 0, 1, &submit_info);
  if (submit_result != VK_SUCCESS) {
    REXLOG_ERROR("VulkanPresenter: Failed to submit the presentation command buffer: {}",
                 vk::to_string(vk::Result(submit_result)));
    if (ui_setup_command_buffer_index != SIZE_MAX) {
      // If failed to submit, make the UI setup command buffer available for
      // immediate reuse, as the completed submission index won't be updated to
      // the current index, and failing submissions with setup command buffer
      // over and over will result in never reusing the setup command buffers.
      paint_context_.ui_setup_command_buffers[ui_setup_command_buffer_index]
          .last_usage_submission_index = 0;
    }
    // The image is in an acquired state - but now, it will be in it forever.
    // To avoid that, recreate the swapchain - don't return just kNotPresented.
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  if (execute_ui_drawers) {
    // Also update the completion timeline providing submission indices to UI
    // draw callbacks if submission is successful.
    const VkResult ui_signal_submit_result = ui_completion_timeline_.AcquireFenceAndSubmit(
        vulkan_device_->queue_family_graphics_compute(), 0, 0, nullptr);
    if (ui_signal_submit_result != VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to submit the UI drawing fence signal: {}",
                   vk::to_string(vk::Result(ui_signal_submit_result)));
    }
  }

  VkPresentInfoKHR present_info;
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.pNext = nullptr;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &present_semaphore;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &paint_context_.swapchain;
  present_info.pImageIndices = &swapchain_image_index;
  present_info.pResults = nullptr;
  VkResult present_result;
  if (frame_generation) {
    // The proxy submits to the game queue through its lock itself, and presents
    // on its own queue (frame_generation.h).
    const auto present_start = std::chrono::steady_clock::now();
    present_result = frame_generation->Present(present_info);
    LogBlockingPresentationCall("vkQueuePresentKHR", present_start, present_result);
  } else if (dlss_two_images) {
    // The generated frame now; the real one held (dlss_frame_generation.h),
    // also when this present fails (the image must leave the acquired state).
    present_info.pWaitSemaphores = &signal_semaphores[1];
    present_info.pImageIndices = &dlss_generated_index;
    {
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device_->AcquireQueue(paint_context_.present_queue_family, 0);
      const auto present_start = std::chrono::steady_clock::now();
      present_result = dfn.vkQueuePresentKHR(queue_acquisition.queue(), &present_info);
      LogBlockingPresentationCall("vkQueuePresentKHR", present_start, present_result);
    }
    dlss_generation->ScheduleRealPresent(swapchain_image_index, present_semaphore);
  } else {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(paint_context_.present_queue_family, 0);
    const auto present_start = std::chrono::steady_clock::now();
    present_result = dfn.vkQueuePresentKHR(queue_acquisition.queue(), &present_info);
    LogBlockingPresentationCall("vkQueuePresentKHR", present_start, present_result);
  }
  switch (present_result) {
    case VK_SUCCESS:
      return PaintResult::kPresented;
    case VK_SUBOPTIMAL_KHR:
      return PaintResult::kPresentedSuboptimal;
    case VK_ERROR_DEVICE_LOST:
      vulkan_device_->SetLost();
      REXLOG_ERROR(
          "VulkanPresenter: Failed to present the swapchain image as the "
          "device has been lost");
      return PaintResult::kGpuLostResponsible;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      // Not an error, reporting just as info (may normally occur while resizing
      // on some platforms).
      REXLOG_INFO(
          "VulkanPresenter: Presentation to the swapchain image has been "
          "dropped as the swapchain or the surface has become outdated");
      // Note that the semaphore wait (followed by reset) has been enqueued,
      // however, this should have no effect on anything here likely.
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      REXLOG_ERROR("VulkanPresenter: Failed to present the swapchain image");
      // The image is in an acquired state - but now, it will be in it forever.
      // To avoid that, recreate the swapchain - don't return just
      // kNotPresented.
      return PaintResult::kNotPresentedConnectionOutdated;
  }
}

bool VulkanPresenter::InitializeSurfaceIndependent() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkDescriptorSetLayoutBinding guest_output_image_sampler_bindings[2];
  guest_output_image_sampler_bindings[0].binding = 0;
  guest_output_image_sampler_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  guest_output_image_sampler_bindings[0].descriptorCount = 1;
  guest_output_image_sampler_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_image_sampler_bindings[0].pImmutableSamplers = nullptr;
  const VkSampler sampler_linear_clamp =
      ui_samplers_->samplers()[UISamplers::kSamplerIndexLinearClampToEdge];
  guest_output_image_sampler_bindings[1].binding = 1;
  guest_output_image_sampler_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  guest_output_image_sampler_bindings[1].descriptorCount = 1;
  guest_output_image_sampler_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_image_sampler_bindings[1].pImmutableSamplers = &sampler_linear_clamp;
  VkDescriptorSetLayoutCreateInfo guest_output_paint_image_descriptor_set_layout_create_info;
  guest_output_paint_image_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  guest_output_paint_image_descriptor_set_layout_create_info.pNext = nullptr;
  guest_output_paint_image_descriptor_set_layout_create_info.flags = 0;
  guest_output_paint_image_descriptor_set_layout_create_info.bindingCount =
      uint32_t(rex::countof(guest_output_image_sampler_bindings));
  guest_output_paint_image_descriptor_set_layout_create_info.pBindings =
      guest_output_image_sampler_bindings;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &guest_output_paint_image_descriptor_set_layout_create_info, nullptr,
          &guest_output_paint_image_descriptor_set_layout_) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create the guest output image descriptor "
        "set layout");
    return false;
  }

  VkPushConstantRange guest_output_paint_push_constant_ranges[2];
  VkPushConstantRange& guest_output_paint_push_constant_range_rect =
      guest_output_paint_push_constant_ranges[0];
  guest_output_paint_push_constant_range_rect.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  guest_output_paint_push_constant_range_rect.offset = 0;
  guest_output_paint_push_constant_range_rect.size = sizeof(GuestOutputPaintRectangleConstants);
  VkPushConstantRange& guest_output_paint_push_constant_range_ffx =
      guest_output_paint_push_constant_ranges[1];
  guest_output_paint_push_constant_range_ffx.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_paint_push_constant_range_ffx.offset =
      guest_output_paint_push_constant_ranges[0].offset +
      guest_output_paint_push_constant_ranges[0].size;
  VkPipelineLayoutCreateInfo guest_output_paint_pipeline_layout_create_info;
  guest_output_paint_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  guest_output_paint_pipeline_layout_create_info.pNext = nullptr;
  guest_output_paint_pipeline_layout_create_info.flags = 0;
  guest_output_paint_pipeline_layout_create_info.setLayoutCount = 1;
  guest_output_paint_pipeline_layout_create_info.pSetLayouts =
      &guest_output_paint_image_descriptor_set_layout_;
  guest_output_paint_pipeline_layout_create_info.pPushConstantRanges =
      guest_output_paint_push_constant_ranges;
  for (size_t i = 0; i < size_t(kGuestOutputPaintPipelineLayoutCount); ++i) {
    switch (GuestOutputPaintPipelineLayoutIndex(i)) {
      case kGuestOutputPaintPipelineLayoutIndexBilinear:
        guest_output_paint_push_constant_range_ffx.size = sizeof(BilinearConstants);
        break;
      case kGuestOutputPaintPipelineLayoutIndexCasSharpen:
        guest_output_paint_push_constant_range_ffx.size = sizeof(CasSharpenConstants);
        break;
      case kGuestOutputPaintPipelineLayoutIndexCasResample:
        guest_output_paint_push_constant_range_ffx.size = sizeof(CasResampleConstants);
        break;
      case kGuestOutputPaintPipelineLayoutIndexFsrEasu:
        guest_output_paint_push_constant_range_ffx.size = sizeof(FsrEasuConstants);
        break;
      case kGuestOutputPaintPipelineLayoutIndexFsrRcas:
        guest_output_paint_push_constant_range_ffx.size = sizeof(FsrRcasConstants);
        break;
      default:
        assert_unhandled_case(GuestOutputPaintPipelineLayoutIndex(i));
        continue;
    }
    guest_output_paint_pipeline_layout_create_info.pushConstantRangeCount =
        1 + uint32_t(guest_output_paint_push_constant_range_ffx.size != 0);
    if (dfn.vkCreatePipelineLayout(device, &guest_output_paint_pipeline_layout_create_info, nullptr,
                                   &guest_output_paint_pipeline_layouts_[i]) != VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to create a guest output presentation "
          "pipeline layout with {} bytes of push constants",
          guest_output_paint_push_constant_range_rect.size +
              guest_output_paint_push_constant_range_ffx.size);
      return false;
    }
  }

  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
  shader_module_create_info.pCode = shaders::guest_output_triangle_strip_rect_vs;
  if (dfn.vkCreateShaderModule(device, &shader_module_create_info, nullptr,
                               &guest_output_paint_vs_) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create the guest output presentation "
        "vertex shader module");
    return false;
  }
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    GuestOutputPaintEffect guest_output_paint_effect = GuestOutputPaintEffect(i);
    switch (guest_output_paint_effect) {
      case GuestOutputPaintEffect::kBilinear:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_bilinear_ps);
        shader_module_create_info.pCode = shaders::guest_output_bilinear_ps;
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_bilinear_dither_ps);
        shader_module_create_info.pCode = shaders::guest_output_bilinear_dither_ps;
        break;
      case GuestOutputPaintEffect::kCasSharpen:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_ffx_cas_sharpen_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_cas_sharpen_ps;
        break;
      case GuestOutputPaintEffect::kCasSharpenDither:
        shader_module_create_info.codeSize =
            sizeof(shaders::guest_output_ffx_cas_sharpen_dither_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_cas_sharpen_dither_ps;
        break;
      case GuestOutputPaintEffect::kCasResample:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_ffx_cas_resample_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_cas_resample_ps;
        break;
      case GuestOutputPaintEffect::kCasResampleDither:
        shader_module_create_info.codeSize =
            sizeof(shaders::guest_output_ffx_cas_resample_dither_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_cas_resample_dither_ps;
        break;
      case GuestOutputPaintEffect::kFsrEasu:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_ffx_fsr_easu_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_fsr_easu_ps;
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_ffx_fsr_rcas_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_fsr_rcas_ps;
        break;
      case GuestOutputPaintEffect::kFsrRcasDither:
        shader_module_create_info.codeSize = sizeof(shaders::guest_output_ffx_fsr_rcas_dither_ps);
        shader_module_create_info.pCode = shaders::guest_output_ffx_fsr_rcas_dither_ps;
        break;
      default:
        // Not supported by this implementation.
        continue;
    }
    if (dfn.vkCreateShaderModule(device, &shader_module_create_info, nullptr,
                                 &guest_output_paint_fs_[i]) != VK_SUCCESS) {
      REXLOG_ERROR(
          "VulkanPresenter: Failed to create the guest output painting shader "
          "module for effect {}",
          i);
      return false;
    }
  }

  VkAttachmentDescription intermediate_render_pass_attachment;
  intermediate_render_pass_attachment.flags = 0;
  intermediate_render_pass_attachment.format = kGuestOutputFormat;
  intermediate_render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  intermediate_render_pass_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  intermediate_render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  intermediate_render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  intermediate_render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  intermediate_render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  intermediate_render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference intermediate_render_pass_color_attachment;
  intermediate_render_pass_color_attachment.attachment = 0;
  intermediate_render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription intermediate_render_pass_subpass = {};
  intermediate_render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  intermediate_render_pass_subpass.colorAttachmentCount = 1;
  intermediate_render_pass_subpass.pColorAttachments = &intermediate_render_pass_color_attachment;
  VkSubpassDependency intermediate_render_pass_dependencies[2];
  intermediate_render_pass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  intermediate_render_pass_dependencies[0].dstSubpass = 0;
  intermediate_render_pass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  intermediate_render_pass_dependencies[0].dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  intermediate_render_pass_dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  intermediate_render_pass_dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  intermediate_render_pass_dependencies[0].dependencyFlags = 0;
  intermediate_render_pass_dependencies[1].srcSubpass = 0;
  intermediate_render_pass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  intermediate_render_pass_dependencies[1].srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  intermediate_render_pass_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  intermediate_render_pass_dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  intermediate_render_pass_dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  intermediate_render_pass_dependencies[1].dependencyFlags = 0;
  VkRenderPassCreateInfo intermediate_render_pass_create_info;
  intermediate_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  intermediate_render_pass_create_info.pNext = nullptr;
  intermediate_render_pass_create_info.flags = 0;
  intermediate_render_pass_create_info.attachmentCount = 1;
  intermediate_render_pass_create_info.pAttachments = &intermediate_render_pass_attachment;
  intermediate_render_pass_create_info.subpassCount = 1;
  intermediate_render_pass_create_info.pSubpasses = &intermediate_render_pass_subpass;
  intermediate_render_pass_create_info.dependencyCount =
      uint32_t(rex::countof(intermediate_render_pass_dependencies));
  intermediate_render_pass_create_info.pDependencies = intermediate_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &intermediate_render_pass_create_info, nullptr,
                             &guest_output_intermediate_render_pass_) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create the guest output intermediate image "
        "render pass");
    return false;
  }

  // Initialize connection-independent parts of the painting context.

  for (size_t i = 0; i < paint_context_.submissions.size(); ++i) {
    paint_context_.submissions[i] = PaintContext::Submission::Create(vulkan_device_);
    if (!paint_context_.submissions[i]) {
      return false;
    }
  }

  // Guest output paint pipelines drawing to intermediate images, not depending
  // on runtime state unlike ones drawing to the swapchain images as those
  // depend on the swapchain format.
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    if (!CanGuestOutputPaintEffectBeIntermediate(GuestOutputPaintEffect(i)) ||
        guest_output_paint_fs_[i] == VK_NULL_HANDLE) {
      continue;
    }
    VkPipeline guest_output_paint_intermediate_pipeline = CreateGuestOutputPaintPipeline(
        GuestOutputPaintEffect(i), guest_output_intermediate_render_pass_);
    if (guest_output_paint_intermediate_pipeline == VK_NULL_HANDLE) {
      return false;
    }
    paint_context_.guest_output_paint_pipelines[i].intermediate_pipeline =
        guest_output_paint_intermediate_pipeline;
  }

  // Guest output painting descriptor sets.
  VkDescriptorPoolSize guest_output_paint_descriptor_pool_sizes[2];
  guest_output_paint_descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  guest_output_paint_descriptor_pool_sizes[0].descriptorCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  // Required even when using immutable samplers, otherwise failing to allocate
  // descriptor sets (tested on AMD Software: Adrenalin Edition 22.3.2 on
  // Windows 10 on AMD Radeon RX Vega 10 with Vulkan validation enabled).
  guest_output_paint_descriptor_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  guest_output_paint_descriptor_pool_sizes[1].descriptorCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  VkDescriptorPoolCreateInfo guest_output_paint_descriptor_pool_create_info;
  guest_output_paint_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  guest_output_paint_descriptor_pool_create_info.pNext = nullptr;
  guest_output_paint_descriptor_pool_create_info.flags = 0;
  guest_output_paint_descriptor_pool_create_info.maxSets =
      PaintContext::kGuestOutputDescriptorSetCount;
  guest_output_paint_descriptor_pool_create_info.poolSizeCount =
      uint32_t(rex::countof(guest_output_paint_descriptor_pool_sizes));
  guest_output_paint_descriptor_pool_create_info.pPoolSizes =
      guest_output_paint_descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &guest_output_paint_descriptor_pool_create_info, nullptr,
                                 &paint_context_.guest_output_descriptor_pool) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create the guest output painting "
        "descriptor pool");
    return false;
  }
  VkDescriptorSetLayout
      guest_output_paint_descriptor_set_layouts[PaintContext::kGuestOutputDescriptorSetCount];
  std::fill(guest_output_paint_descriptor_set_layouts,
            guest_output_paint_descriptor_set_layouts +
                rex::countof(guest_output_paint_descriptor_set_layouts),
            guest_output_paint_image_descriptor_set_layout_);
  VkDescriptorSetAllocateInfo guest_output_paint_descriptor_set_allocate_info;
  guest_output_paint_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  guest_output_paint_descriptor_set_allocate_info.pNext = nullptr;
  guest_output_paint_descriptor_set_allocate_info.descriptorPool =
      paint_context_.guest_output_descriptor_pool;
  guest_output_paint_descriptor_set_allocate_info.descriptorSetCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  guest_output_paint_descriptor_set_allocate_info.pSetLayouts =
      guest_output_paint_descriptor_set_layouts;
  if (dfn.vkAllocateDescriptorSets(device, &guest_output_paint_descriptor_set_allocate_info,
                                   paint_context_.guest_output_descriptor_sets) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to allocate the guest output painting "
        "descriptor sets");
    return false;
  }

  // AMD FSR frame generation (native_frame_generation; needs the device's extra
  // queues, so a restart to switch). Unavailable: the regular swapchain.
  if (VulkanFrameGeneration::Requested()) {
    std::string reason;
    frame_generation_ = VulkanFrameGeneration::Create(*vulkan_device_, reason);
    if (frame_generation_) {
      paint_context_.frame_generation = frame_generation_.get();
    } else {
      REXLOG_WARN("VulkanPresenter: frame generation unavailable, frames are shown as they are: {}",
                  reason);
      VulkanFrameGeneration::WriteStatus(*vulkan_device_,
                                         "AMD FSR frame generation unavailable: " + reason);
    }
  }
  // NVIDIA DLSS frame generation (native_frame_generation = dlss): on the
  // regular swapchain, the presenter presenting both frames.
  if (VulkanDlssFrameGeneration::Requested()) {
    std::string reason;
    dlss_frame_generation_ = VulkanDlssFrameGeneration::Create(*vulkan_device_, reason);
    if (dlss_frame_generation_) {
      paint_context_.dlss_frame_generation = dlss_frame_generation_.get();
    } else {
      REXLOG_WARN("VulkanPresenter: DLSS frame generation unavailable, frames are shown as they are: {}", reason);
      VulkanFrameGeneration::WriteStatus(*vulkan_device_,
                                         "NVIDIA DLSS Frame Generation unavailable: " + reason);
    }
  }

  return InitializeCommonSurfaceIndependent();
}

VkPipeline VulkanPresenter::CreateGuestOutputPaintPipeline(GuestOutputPaintEffect effect,
                                                           VkRenderPass render_pass) {
  VkPipelineShaderStageCreateInfo stages[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    VkPipelineShaderStageCreateInfo& stage = stages[i];
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stage.pName = "main";
  }
  stages[0].module = guest_output_paint_vs_;
  stages[1].module = guest_output_paint_fs_[size_t(effect)];
  assert_true(stages[1].module != VK_NULL_HANDLE);

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {};
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState color_blend_attachment_state = {};
  color_blend_attachment_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount = 1;
  color_blend_state.pAttachments = &color_blend_attachment_state;

  static const VkDynamicState kPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state = {};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = uint32_t(rex::countof(kPipelineDynamicStates));
  dynamic_state.pDynamicStates = kPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = uint32_t(rex::countof(stages));
  pipeline_create_info.pStages = stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = nullptr;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      guest_output_paint_pipeline_layouts_[GetGuestOutputPaintPipelineLayoutIndex(effect)];
  pipeline_create_info.renderPass = render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkPipeline pipeline;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    REXLOG_ERROR(
        "VulkanPresenter: Failed to create the guest output painting pipeline "
        "for effect {}",
        size_t(effect));
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
