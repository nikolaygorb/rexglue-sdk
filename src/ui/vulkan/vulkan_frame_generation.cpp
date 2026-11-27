#include <rex/ui/vulkan/frame_generation.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/fsr_versions.h>
#include <rex/ui/vulkan/instance.h>

REXCVAR_DEFINE_STRING(native_frame_generation, "off", "GPU",
    "Frame generation (Native, Vulkan, Windows): off, fsr3 (AMD FSR 3.1), fsr4 (AMD FSR 4 where the GPU "
    "offers it: Radeon RX 9000 and newer; elsewhere FSR 3.1, noted in logs/frame_generation.log), dlss "
    "(NVIDIA DLSS Frame Generation: GeForce RTX 40 and newer; paced by the presenter); fsr = fsr3. Shows a "
    "generated frame between two real ones; the game's real frame rate is held near half the "
    "display's refresh rate. Needs the title's scene data (depth, motion, the frame without the HUD); "
    "without it frames are shown as they are. Unavailable -> the regular swapchain.")
    .allowed({"off", "fsr3", "fsr4", "dlss", "fsr"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_frame_generation_debug, 0, "GPU",
    "Frame generation diagnostics: 0 off; 1 AMD: tear, reset and pacing markers, DLSS: the real frame in "
    "the generated one's place (the same pacing, no generated content); 2 AMD: its debug view (FSR 3.1 "
    "only), DLSS: only generated frames shown; 3 the frame without the HUD instead of the frame, 4 the "
    "inputs instead of the frame (motion as color, gray = still; depth in blue). 3 and 4 show real frames "
    "only.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_frame_generation_async, true, "GPU",
    "Frame generation on a compute queue of its own, overlapping the next frame's rendering (AMD's async "
    "workloads; needs a fourth queue, else on the graphics queue). Off: on the graphics queue.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_frame_generation_debug_checking, false, "GPU",
    "AMD frame generation input validation; messages go to the log.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#if defined(REX_HAS_FSR_SDK) && REX_HAS_FSR_SDK && defined(_WIN32)
#define REX_FSR_FG_RUNTIME 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <api/include/ffx_api.h>
#include <api/include/vk/ffx_api_vk.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <framegeneration/include/vk/ffx_api_framegeneration_vk.h>
#include <upscalers/include/ffx_upscale.h>
#else
#define REX_FSR_FG_RUNTIME 0
#endif

namespace rex::ui::vulkan {

bool VulkanFrameGeneration::Requested() {
  const std::string& value = REXCVAR_GET(native_frame_generation);
  return value != "off" && value != "dlss";  // dlss: dlss_frame_generation.h.
}

void VulkanFrameGeneration::WriteStatus(const VulkanDevice& device, const std::string& text) {
  static std::mutex mutex;
  static std::string written;
  std::lock_guard lock(mutex);
  if (text == written) return;
  written = text;
  std::error_code error;
  const auto directory = rex::filesystem::GetExecutableFolder() / "logs";
  std::filesystem::create_directories(directory, error);
  std::ofstream file(directory / "frame_generation.log", std::ios::trunc);
  file << text;
  if (device.properties().deviceName[0]) file << ", " << device.properties().deviceName;
  file << '\n';
}

#if REX_FSR_FG_RUNTIME
namespace {
std::atomic<PFN_vkGetDeviceProcAddr> g_get_device_proc_addr{nullptr};
// The game queue (graphics and compute queue 0) is shared with the renderer
// and the presenter: the proxy submits to it under the device's queue lock.
std::atomic<VulkanDevice*> g_game_device{nullptr};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
  const auto get = g_get_device_proc_addr.load(std::memory_order_relaxed);
  if (!get || !name) return nullptr;
  if (auto proc = get(device, name)) return proc;
  // Vulkan 1.1+ core entry points some drivers don't expose under the KHR names.
  static constexpr const char* kAliases[][2] = {
      {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
      {"vkGetImageMemoryRequirements2KHR", "vkGetImageMemoryRequirements2"},
      {"vkBindBufferMemory2KHR", "vkBindBufferMemory2"},
      {"vkBindImageMemory2KHR", "vkBindImageMemory2"},
      {"vkGetSemaphoreCounterValueKHR", "vkGetSemaphoreCounterValue"},
      {"vkWaitSemaphoresKHR", "vkWaitSemaphores"},
      {"vkSignalSemaphoreKHR", "vkSignalSemaphore"}};
  for (const auto& alias : kAliases)
    if (!std::strcmp(name, alias[0])) return get(device, alias[1]);
  return nullptr;
}

VkResult SubmitToGameQueue(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
  VulkanDevice* device = g_game_device.load(std::memory_order_acquire);
  if (!device) return VK_ERROR_DEVICE_LOST;
  const VulkanDevice::Queue::Acquisition queue =
      device->AcquireQueue(device->queue_family_graphics_compute(), 0);
  return device->functions().vkQueueSubmit(queue.queue(), count, submits, fence);
}

void Message(uint32_t type, const wchar_t* message) {
  std::string text;
  for (const wchar_t* c = message; c && *c; ++c) text.push_back(*c < 128 ? char(*c) : '?');
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXLOG_ERROR("FSR frame generation: {}", text);
  } else {
    REXLOG_WARN("FSR frame generation: {}", text);
  }
}

FfxApiResource Resource(VkImage image, VkFormat format, uint32_t width, uint32_t height) {
  if (!image) return {};
  FfxApiResourceDescription description{};
  description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  description.format = ffxApiGetSurfaceFormatVK(format);
  description.width = width;
  description.height = height;
  description.depth = 1;
  description.mipCount = 1;
  description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image), description,
                             FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
}

// The providers of an effect on this device, newest first (names copied: they
// live in the DLLs).
void ListVersions(PfnFfxQuery query, VkDeviceContext& context, uint64_t create_type,
                  std::vector<uint64_t>& ids, std::vector<std::string>& names) {
  ids.clear();
  names.clear();
  uint64_t count = 0;
  ffxQueryDescGetVersions versions{};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = create_type;
  versions.device = &context;
  versions.outputCount = &count;
  if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count) return;
  ids.resize(size_t(count));
  std::vector<const char*> text(size_t(count), nullptr);
  versions.versionIds = ids.data();
  versions.versionNames = text.data();
  if (query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
    ids.clear();
    return;
  }
  ids.resize(std::min<size_t>(size_t(count), ids.size()));
  for (size_t i = 0; i < ids.size(); ++i) names.emplace_back(text[i] ? text[i] : "");
}

std::string JoinVersions(const std::vector<std::string>& names) {
  std::string list;
  for (size_t i = 0; i < names.size(); ++i) list += fmt::format("{}{}", i ? ", " : "", names[i]);
  return list.empty() ? std::string("none") : list;
}
}  // namespace

struct VulkanFrameGeneration::Functions {
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  ffxQueryDescSwapchainReplacementFunctionsVK swapchain{};
};

namespace {
// The proxy calls this from its present to record the interpolation.
ffxReturnCode_t DispatchGeneration(ffxDispatchDescFrameGeneration* desc, void* user) {
  auto* pair = static_cast<std::pair<PfnFfxDispatch, void**>*>(user);
  return pair->first(reinterpret_cast<ffxContext*>(pair->second), &desc->header);
}
// Stable storage for the callback's context (one presenter per process).
std::pair<PfnFfxDispatch, void**> g_dispatch_target{nullptr, nullptr};
}  // namespace
#else
struct VulkanFrameGeneration::Functions {};
#endif

bool QueryFsrVersions(const VulkanDevice& device, FsrVersions& out, std::string& reason) {
  out = {};
#if REX_FSR_FG_RUNTIME
  HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (!module) {
    reason = "amd_fidelityfx_loader_vk.dll not found next to the executable";
    return false;
  }
  auto query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
  const VulkanInstance* instance = device.vulkan_instance();
  auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      instance->functions().vkGetInstanceProcAddr(instance->instance(), "vkGetDeviceProcAddr"));
  if (!query || !get) {
    FreeLibrary(module);
    reason = !query ? "ffx-api entry points missing in the loader DLL" : "vkGetDeviceProcAddr unavailable";
    return false;
  }
  g_get_device_proc_addr.store(get, std::memory_order_relaxed);
  VkDeviceContext context{device.device(), device.physical_device(), GetDeviceProcAddr};
  std::vector<uint64_t> ids;
  ListVersions(query, context, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ids, out.upscaler);
  ListVersions(query, context, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, ids, out.frame_generation);
  ListVersions(query, context, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK, ids, out.swapchain);
  FreeLibrary(module);
  return true;
#else
  (void)device;
  reason = "AMD FSR SDK not available in this build/platform (Windows with REXGLUE_ENABLE_FSR_SDK)";
  return false;
#endif
}

std::unique_ptr<VulkanFrameGeneration> VulkanFrameGeneration::Create(VulkanDevice& device,
                                                                     std::string& reason) {
#if REX_FSR_FG_RUNTIME
  // Next to the executable (staged with the native GPU plugin).
  HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (!module) {
    reason = "amd_fidelityfx_loader_vk.dll not found next to the executable";
    return nullptr;
  }
  std::unique_ptr<VulkanFrameGeneration> generation(new VulkanFrameGeneration());
  generation->device_ = &device;
  generation->module_ = module;
  generation->functions_ = std::make_unique<Functions>();
  auto& f = *generation->functions_;
  f.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
  f.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
  f.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
  f.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
  f.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
  if (!f.create || !f.destroy || !f.configure || !f.query || !f.dispatch) {
    reason = "ffx-api entry points missing in the loader DLL";
    return nullptr;
  }
  const VulkanInstance* instance = device.vulkan_instance();
  auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      instance->functions().vkGetInstanceProcAddr(instance->instance(), "vkGetDeviceProcAddr"));
  if (!get) {
    reason = "vkGetDeviceProcAddr unavailable";
    return nullptr;
  }
  g_get_device_proc_addr.store(get, std::memory_order_relaxed);
  // The frame generation and swapchain versions on this device, newest first.
  VkDeviceContext context{device.device(), device.physical_device(), GetDeviceProcAddr};
  ListVersions(f.query, context, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, generation->version_ids_,
               generation->version_names_);
  if (generation->version_ids_.empty()) {
    reason = "no AMD FSR frame generation provider for this GPU (amd_fidelityfx_framegeneration_vk.dll)";
    return nullptr;
  }
  ListVersions(f.query, context, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK, generation->swapchain_ids_,
               generation->swapchain_names_);
  REXLOG_INFO("FSR frame generation: versions on this device: {}; swapchain: {}",
              JoinVersions(generation->version_names_), JoinVersions(generation->swapchain_names_));
  // FSR 4 generation goes with the FSR 4 swapchain: both, else FSR 3 for both.
  const int wanted = REXCVAR_GET(native_frame_generation) == "fsr4" ? 4 : 3;
  const bool fsr4 = FsrHasMajor(generation->version_names_, 4) &&
                    (generation->swapchain_names_.empty() || FsrHasMajor(generation->swapchain_names_, 4));
  generation->major_ = wanted >= 4 && fsr4 ? 4 : 3;
  if (wanted >= 4 && !fsr4) {
    generation->fallback_note_ = " (FSR 4 unavailable on this GPU)";
    REXLOG_WARN("FSR frame generation: FSR 4 is not offered for this GPU, FSR 3 instead");
  }
  g_game_device.store(&device, std::memory_order_release);
  return generation;
#else
  (void)device;
  reason = "AMD FSR SDK not available in this build/platform (Windows with REXGLUE_ENABLE_FSR_SDK)";
  return nullptr;
#endif
}

VulkanFrameGeneration::~VulkanFrameGeneration() {
#if REX_FSR_FG_RUNTIME
  // The presenter destroys the swapchain (DestroySwapchain) first.
  if (module_) FreeLibrary(static_cast<HMODULE>(module_));
#endif
}

bool VulkanFrameGeneration::PickQueues(VkSurfaceKHR surface, std::string& reason) {
  // Two queues besides the game queue (graphics and compute 0, used by the
  // renderer and the presenter): one that can present to the surface, and
  // one for image acquisition. The proxy needs them distinct; VulkanDevice
  // creates the extra queues while frame generation is requested.
  const VulkanDevice& device = *device_;
  const auto& ifn = device.vulkan_instance()->functions();
  const auto& families = device.queue_families();
  const uint32_t game_family = device.queue_family_graphics_compute();
  const uint32_t sparse_family = device.queue_family_sparse_binding();
  present_family_ = acquire_family_ = UINT32_MAX;
  auto usable = [&](uint32_t family, uint32_t index) {
    if (family == game_family && index == 0) return false;
    if (family == sparse_family && index == 0) return false;  // The emulation's sparse binding.
    return true;
  };
  // Present first: the graphics family (no ownership transfer), then any.
  for (int pass = 0; pass < 2 && present_family_ == UINT32_MAX; ++pass) {
    for (uint32_t family = 0; family < uint32_t(families.size()); ++family) {
      if ((pass == 0) != (family == game_family) || !families[family].may_support_presentation) continue;
      VkBool32 supported = VK_FALSE;
      if (ifn.vkGetPhysicalDeviceSurfaceSupportKHR(device.physical_device(), family, surface,
                                                   &supported) != VK_SUCCESS || !supported) continue;
      for (uint32_t index = 0; index < uint32_t(families[family].queues.size()); ++index) {
        if (!usable(family, index)) continue;
        present_family_ = family;
        present_index_ = index;
        break;
      }
      if (present_family_ != UINT32_MAX) break;
    }
  }
  if (present_family_ == UINT32_MAX) {
    reason = "no second queue that can present (the GPU exposes one)";
    return false;
  }
  for (uint32_t family = 0; family < uint32_t(families.size()) && acquire_family_ == UINT32_MAX; ++family) {
    for (uint32_t index = 0; index < uint32_t(families[family].queues.size()); ++index) {
      if (!usable(family, index) || (family == present_family_ && index == present_index_)) continue;
      acquire_family_ = family;
      acquire_index_ = index;
      break;
    }
  }
  if (acquire_family_ == UINT32_MAX) {
    reason = "no third queue for image acquisition";
    return false;
  }
  // Async generation: a compute queue, preferably of a compute-only family
  // (overlaps the graphics work the most), distinct from the others.
  async_family_ = UINT32_MAX;
  if (REXCVAR_GET(native_frame_generation_async)) {
    for (int pass = 0; pass < 2 && async_family_ == UINT32_MAX; ++pass) {
      for (uint32_t family = 0; family < uint32_t(families.size()) && async_family_ == UINT32_MAX; ++family) {
        const VkQueueFlags flags = families[family].queue_flags;
        if (!(flags & VK_QUEUE_COMPUTE_BIT) || (pass == 0) == bool(flags & VK_QUEUE_GRAPHICS_BIT)) continue;
        for (uint32_t index = 0; index < uint32_t(families[family].queues.size()); ++index) {
          if (!usable(family, index) || (family == present_family_ && index == present_index_) ||
              (family == acquire_family_ && index == acquire_index_)) continue;
          async_family_ = family;
          async_index_ = index;
          break;
        }
      }
    }
  }
  REXLOG_INFO("FSR frame generation: queues game {}:0, present {}:{}, acquire {}:{}, async compute {}",
              game_family, present_family_, present_index_, acquire_family_, acquire_index_,
              async_family_ == UINT32_MAX ? std::string("off (on the game queue)")
                                          : fmt::format("{}:{}", async_family_, async_index_));
  return true;
}

bool VulkanFrameGeneration::CreateSwapchain(VkSurfaceKHR surface, const VkSwapchainCreateInfoKHR& info,
                                            VkSwapchainKHR& swapchain_out, std::string& reason) {
#if REX_FSR_FG_RUNTIME
  swapchain_out = VK_NULL_HANDLE;
  if (swapchain_context_) {
    reason = "a frame generation swapchain already exists";
    return false;
  }
  if (!PickQueues(surface, reason)) return false;
  auto& f = *functions_;
  const VulkanDevice& device = *device_;
  const uint32_t game_family = device.queue_family_graphics_compute();
  ffxCreateContextDescFrameGenerationSwapChainVK desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
  desc.physicalDevice = device.physical_device();
  desc.device = device.device();
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  desc.swapchain = &swapchain;
  desc.allocator = nullptr;
  desc.createInfo = info;
  desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
  desc.gameQueue = {device.queue_families()[game_family].queues[0]->queue, game_family, SubmitToGameQueue};
  // Without one, generation runs on the game queue.
  desc.asyncComputeQueue = {VK_NULL_HANDLE, 0, nullptr};
  if (async_family_ != UINT32_MAX)
    desc.asyncComputeQueue = {device.queue_families()[async_family_].queues[async_index_]->queue,
                              async_family_, nullptr};
  desc.presentQueue = {device.queue_families()[present_family_].queues[present_index_]->queue,
                       present_family_, nullptr};
  desc.imageAcquireQueue = {device.queue_families()[acquire_family_].queues[acquire_index_]->queue,
                            acquire_family_, nullptr};
  ffxCreateContextDescFrameGenerationSwapChainVersionVK version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
  version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
  desc.header.pNext = &version.header;
  // The chosen major version's providers, newest first, then FSR 3; none
  // listed: the loader's default.
  ffxOverrideVersion override_version{};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  const std::vector<size_t> order = FsrVersionOrder(swapchain_names_, major_);
  // The proxy may wait on the game queue (idle) while creating: not under its lock here.
  ffxReturnCode_t result = FFX_API_RETURN_ERROR;
  for (size_t i = 0; i < std::max<size_t>(order.size(), 1); ++i) {
    swapchain_context_ = nullptr;
    swapchain = VK_NULL_HANDLE;
    if (!order.empty()) {
      override_version.versionId = swapchain_ids_[order[i]];
      version.header.pNext = &override_version.header;
    }
    result = f.create(reinterpret_cast<ffxContext*>(&swapchain_context_), &desc.header, nullptr);
    if (result == FFX_API_RETURN_OK && swapchain_context_ && swapchain) break;
  }
  if (result != FFX_API_RETURN_OK || !swapchain_context_ || !swapchain) {
    swapchain_context_ = nullptr;
    reason = fmt::format("swapchain context creation failed ({})", result);
    return false;
  }
  f.swapchain = {};
  f.swapchain.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
  if (f.query(reinterpret_cast<ffxContext*>(&swapchain_context_), &f.swapchain.header) !=
          FFX_API_RETURN_OK ||
      !f.swapchain.pOutAcquireNextImageKHR || !f.swapchain.pOutQueuePresentKHR ||
      !f.swapchain.pOutGetSwapchainImagesKHR || !f.swapchain.pOutDestroySwapchainFFXAPI) {
    f.destroy(reinterpret_cast<ffxContext*>(&swapchain_context_), nullptr);
    swapchain_context_ = nullptr;
    reason = "swapchain replacement functions unavailable";
    return false;
  }
  frame_id_ += 2;  // A new swapchain starts without history.
  swapchain_out = swapchain;
  REXLOG_INFO("FSR frame generation: swapchain {}x{} format {}", info.imageExtent.width,
              info.imageExtent.height, uint32_t(info.imageFormat));
  return true;
#else
  (void)surface; (void)info; swapchain_out = VK_NULL_HANDLE;
  reason = "unavailable";
  return false;
#endif
}

void VulkanFrameGeneration::WaitForPresents() {
#if REX_FSR_FG_RUNTIME
  if (!swapchain_context_) return;
  ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
  wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
  functions_->dispatch(reinterpret_cast<ffxContext*>(&swapchain_context_), &wait.header);
#endif
}

void VulkanFrameGeneration::DestroySwapchain(VkSwapchainKHR swapchain) {
#if REX_FSR_FG_RUNTIME
  if (!swapchain_context_) return;
  auto& f = *functions_;
  DestroyContext(swapchain);
  ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
  wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
  f.dispatch(reinterpret_cast<ffxContext*>(&swapchain_context_), &wait.header);
  f.destroy(reinterpret_cast<ffxContext*>(&swapchain_context_), nullptr);
  swapchain_context_ = nullptr;
  if (swapchain && f.swapchain.pOutDestroySwapchainFFXAPI)
    f.swapchain.pOutDestroySwapchainFFXAPI(device_->device(), swapchain, nullptr, nullptr);
  f.swapchain = {};
#else
  (void)swapchain;
#endif
}

VkResult VulkanFrameGeneration::GetSwapchainImages(VkSwapchainKHR swapchain, uint32_t* count,
                                                   VkImage* images) {
#if REX_FSR_FG_RUNTIME
  return functions_->swapchain.pOutGetSwapchainImagesKHR(device_->device(), swapchain, count, images);
#else
  (void)swapchain; (void)count; (void)images;
  return VK_ERROR_INITIALIZATION_FAILED;
#endif
}

VkResult VulkanFrameGeneration::AcquireNextImage(VkSwapchainKHR swapchain, uint64_t timeout,
                                                 VkSemaphore semaphore, uint32_t* image_index) {
#if REX_FSR_FG_RUNTIME
  return functions_->swapchain.pOutAcquireNextImageKHR(device_->device(), swapchain, timeout,
                                                       semaphore, VK_NULL_HANDLE, image_index);
#else
  (void)swapchain; (void)timeout; (void)semaphore; (void)image_index;
  return VK_ERROR_INITIALIZATION_FAILED;
#endif
}

VkResult VulkanFrameGeneration::Present(const VkPresentInfoKHR& info) {
#if REX_FSR_FG_RUNTIME
  const VulkanDevice& device = *device_;
  return functions_->swapchain.pOutQueuePresentKHR(
      device.queue_families()[device.queue_family_graphics_compute()].queues[0]->queue, &info);
#else
  (void)info;
  return VK_ERROR_INITIALIZATION_FAILED;
#endif
}

uint64_t VulkanFrameGeneration::PresentCount(VkSwapchainKHR swapchain) const {
#if REX_FSR_FG_RUNTIME
  if (!swapchain || !functions_->swapchain.pOutGetLastPresentCountFFXAPI) return 0;
  return functions_->swapchain.pOutGetLastPresentCountFFXAPI(swapchain);
#else
  (void)swapchain;
  return 0;
#endif
}

void VulkanFrameGeneration::DestroyContext(VkSwapchainKHR swapchain) {
#if REX_FSR_FG_RUNTIME
  if (!context_) return;
  // Disabling waits for the proxy's work that uses the context's resources.
  Configure(swapchain, false, nullptr);
  functions_->destroy(reinterpret_cast<ffxContext*>(&context_), nullptr);
#else
  (void)swapchain;
#endif
  context_ = nullptr;
  enabled_ = false;
  context_display_width_ = context_display_height_ = 0;
  context_render_width_ = context_render_height_ = 0;
  context_flags_ = 0;
  context_format_ = VK_FORMAT_UNDEFINED;
}

bool VulkanFrameGeneration::EnsureContext(VkSwapchainKHR swapchain, const Frame& frame) {
#if REX_FSR_FG_RUNTIME
  uint32_t flags = (async_family_ != UINT32_MAX ? FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT : 0) |
                   (frame.depth_inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                   (frame.depth_infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0) |
                   (REXCVAR_GET(native_frame_generation_debug_checking)
                        ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0);
  // Inputs up to the larger of the render and the display size (dynamic).
  const uint32_t render_width = std::max(frame.render_width, frame.display_width);
  const uint32_t render_height = std::max(frame.render_height, frame.display_height);
  if (context_ && frame.display_width == context_display_width_ &&
      frame.display_height == context_display_height_ && render_width <= context_render_width_ &&
      render_height <= context_render_height_ && frame.backbuffer_format == context_format_) {
    if (flags == context_flags_) {
      flag_mismatch_frames_ = 0;
      return true;
    }
    // A stray frame with other camera flags is not generated; a lasting change re-creates.
    if (++flag_mismatch_frames_ < 30) {
      last_error_ = "camera depth flags differ from the context's (transient)";
      return false;
    }
  }
  flag_mismatch_frames_ = 0;
  if (context_failed_) return false;
  DestroyContext(swapchain);
  auto& f = *functions_;
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext = {device_->device(), device_->physical_device(), GetDeviceProcAddr};
  ffxCreateContextDescFrameGenerationVersion version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  version.version = FFX_FRAMEGENERATION_VERSION;
  ffxCreateContextDescFrameGeneration create{};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  create.flags = flags;
  create.displaySize = {frame.display_width, frame.display_height};
  create.maxRenderSize = {render_width, render_height};
  create.backBufferFormat = ffxApiGetSurfaceFormatVK(frame.backbuffer_format);
  create.header.pNext = &backend.header;
  backend.header.pNext = &version.header;
  ffxOverrideVersion override_version{};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  // The chosen major version's providers, newest first, then FSR 3 (Create).
  const std::vector<size_t> order = FsrVersionOrder(version_names_, major_);
  ffxReturnCode_t result = FFX_API_RETURN_ERROR;
  for (size_t i = 0; i < std::max<size_t>(order.size(), 1); ++i) {
    context_ = nullptr;
    if (!order.empty()) {
      override_version.versionId = version_ids_[order[i]];
      version.header.pNext = &override_version.header;
    }
    result = f.create(reinterpret_cast<ffxContext*>(&context_), &create.header, nullptr);
    if (result == FFX_API_RETURN_OK && context_) break;
  }
  if (result != FFX_API_RETURN_OK || !context_) {
    context_ = nullptr;
    context_failed_ = true;  // Not retried every frame.
    last_error_ = fmt::format("context creation failed ({})", result);
    REXLOG_ERROR("FSR frame generation: {}", last_error_);
    WriteStatus(*device_, "AMD FSR frame generation failed (context)");
    return false;
  }
  context_display_width_ = frame.display_width;
  context_display_height_ = frame.display_height;
  context_render_width_ = render_width;
  context_render_height_ = render_height;
  context_flags_ = flags;
  context_format_ = frame.backbuffer_format;
  legacy_prepare_ = false;
  ffxQueryGetProviderVersion provider{};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (f.query(reinterpret_cast<ffxContext*>(&context_), &provider.header) == FFX_API_RETURN_OK &&
      provider.versionName) {
    version_ = provider.versionName;
  }
  g_dispatch_target = {f.dispatch, &context_};
  if (flags & FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING) {
    ffxConfigureDescGlobalDebug debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    debug.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
    debug.fpMessage = Message;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    f.configure(reinterpret_cast<ffxContext*>(&context_), &debug.header);
  }
  REXLOG_INFO("FSR frame generation: context display {}x{}, inputs up to {}x{}, flags {:#x}, provider {}",
              frame.display_width, frame.display_height, render_width, render_height, flags,
              version_.empty() ? "?" : version_);
  if (major_ >= 4 && FsrMajor(version_) < 4) fallback_note_ = " (FSR 4 failed on this GPU)";
  WriteStatus(*device_, "AMD FSR frame generation " +
                            (version_.empty() ? std::string("(version unknown)") : version_) + fallback_note_);
  return true;
#else
  (void)swapchain; (void)frame;
  return false;
#endif
}

bool VulkanFrameGeneration::Configure(VkSwapchainKHR swapchain, bool enabled, const Frame* frame) {
#if REX_FSR_FG_RUNTIME
  if (!context_) return false;
  ffxConfigureDescFrameGeneration config{};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.swapChain = swapchain;
  config.presentCallback = nullptr;
  config.presentCallbackUserContext = nullptr;
  config.frameGenerationEnabled = enabled && frame;
  config.allowAsyncWorkloads = async_family_ != UINT32_MAX;
  config.onlyPresentGenerated = false;
  config.frameID = frame_id_;
  // The same flags either way: a change of them makes the proxy wait for its presents.
  const int32_t debug = REXCVAR_GET(native_frame_generation_debug);
  config.flags = debug == 1 ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
                               FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS |
                               FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES)
                 : debug == 2 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
  // Each switch makes the proxy wait for its presents and start or stop its present thread.
  if (config.frameGenerationEnabled != enabled_) {
    enabled_ = config.frameGenerationEnabled;
    ++enable_switches_;
  }
  if (config.frameGenerationEnabled) {
    config.frameGenerationCallback = DispatchGeneration;
    config.frameGenerationCallbackUserContext = &g_dispatch_target;
    config.HUDLessColor =
        Resource(frame->hudless, frame->backbuffer_format, frame->display_width, frame->display_height);
    config.generationRect = {frame->rect_x, frame->rect_y, int32_t(frame->rect_width),
                             int32_t(frame->rect_height)};
  }
  return functions_->configure(reinterpret_cast<ffxContext*>(&context_), &config.header) ==
         FFX_API_RETURN_OK;
#else
  (void)swapchain; (void)enabled; (void)frame;
  return false;
#endif
}

bool VulkanFrameGeneration::BeginPresent(VkSwapchainKHR swapchain, const Frame& frame) {
#if REX_FSR_FG_RUNTIME
  ++frame_id_;
  if (frame.reset) ++frame_id_;  // A jump in frameID resets generation too.
  if (!frame.generate) {
    Configure(swapchain, false, nullptr);  // No context yet: the proxy presents as it is.
    return false;
  }
  if (!frame.command_buffer || !frame.depth || !frame.motion || (!frame.hudless && !frame.reset) ||
      !frame.render_width || !frame.render_height || !frame.display_width || !frame.display_height ||
      !frame.rect_width || !frame.rect_height) {
    last_error_ = "incomplete inputs";
    Configure(swapchain, false, nullptr);
    return false;
  }
  if (!EnsureContext(swapchain, frame)) {
    Configure(swapchain, false, nullptr);
    return false;
  }
  auto& f = *functions_;
  const int32_t debug = REXCVAR_GET(native_frame_generation_debug);
  const uint32_t debug_flags = debug == 1 ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
                                             FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS)
                               : debug == 2 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
  const FfxApiResource depth = Resource(frame.depth, frame.depth_format, frame.render_width, frame.render_height);
  const FfxApiResource motion =
      Resource(frame.motion, frame.motion_format, frame.render_width, frame.render_height);
  const float delta_ms = frame.delta_ms > 0 ? frame.delta_ms : 1000.f / 60.f;
  const float far_distance = frame.depth_infinite ? FLT_MAX : frame.camera_far;
  const float meters = frame.meters_per_unit > 0 ? frame.meters_per_unit : 1.f;
  ffxReturnCode_t result = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  if (!legacy_prepare_) {
    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = frame_id_;
    prepare.flags = debug_flags;
    prepare.commandList = frame.command_buffer;
    prepare.renderSize = {frame.render_width, frame.render_height};
    prepare.jitterOffset = {frame.jitter_x, frame.jitter_y};
    prepare.motionVectorScale = {1.f, 1.f};  // Already input pixels.
    prepare.frameTimeDelta = delta_ms;
    prepare.reset = frame.reset;
    prepare.cameraNear = frame.camera_near;
    prepare.cameraFar = far_distance;
    prepare.cameraFovAngleVertical = frame.fov_y;
    prepare.viewSpaceToMetersFactor = meters;
    prepare.depth = depth;
    prepare.motionVectors = motion;
    std::copy(frame.position.begin(), frame.position.end(), prepare.cameraPosition);
    std::copy(frame.up.begin(), frame.up.end(), prepare.cameraUp);
    std::copy(frame.right.begin(), frame.right.end(), prepare.cameraRight);
    std::copy(frame.forward.begin(), frame.forward.end(), prepare.cameraForward);
    result = f.dispatch(reinterpret_cast<ffxContext*>(&context_), &prepare.header);
    if (result == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE) {
      legacy_prepare_ = true;
      REXLOG_INFO("FSR frame generation: the provider takes the FSR 3.1 Prepare");
    }
  }
  if (legacy_prepare_) {
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    ffxDispatchDescFrameGenerationPrepare prepare{};
    ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
    camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    std::copy(frame.position.begin(), frame.position.end(), camera.cameraPosition);
    std::copy(frame.up.begin(), frame.up.end(), camera.cameraUp);
    std::copy(frame.right.begin(), frame.right.end(), camera.cameraRight);
    std::copy(frame.forward.begin(), frame.forward.end(), camera.cameraForward);
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.header.pNext = &camera.header;
    prepare.frameID = frame_id_;
    prepare.flags = debug_flags;
    prepare.commandList = frame.command_buffer;
    prepare.renderSize = {frame.render_width, frame.render_height};
    prepare.jitterOffset = {frame.jitter_x, frame.jitter_y};
    prepare.motionVectorScale = {1.f, 1.f};
    prepare.frameTimeDelta = delta_ms;
    prepare.unused_reset = frame.reset;
    prepare.cameraNear = frame.camera_near;
    prepare.cameraFar = far_distance;
    prepare.cameraFovAngleVertical = frame.fov_y;
    prepare.viewSpaceToMetersFactor = meters;
    prepare.depth = depth;
    prepare.motionVectors = motion;
    result = f.dispatch(reinterpret_cast<ffxContext*>(&context_), &prepare.header);
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif
  }
  if (result != FFX_API_RETURN_OK) {
    last_error_ = fmt::format("prepare failed ({})", result);
    Configure(swapchain, false, nullptr);
    return false;
  }
  if (!Configure(swapchain, true, &frame)) {
    last_error_ = "configure failed";
    Configure(swapchain, false, nullptr);
    return false;
  }
  ++generating_frames_;
  return true;
#else
  (void)swapchain; (void)frame;
  return false;
#endif
}

}  // namespace rex::ui::vulkan
