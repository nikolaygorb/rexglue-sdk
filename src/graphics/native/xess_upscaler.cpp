// Intel XeSS Super Resolution as the native temporal upscaler (libxess.dll,
// cmake/rexglue_xess_sdk.cmake; Windows). The device carries XeSS's extensions
// and features when the DLL is there (ui/vulkan/xess.h). XeSS is not thread
// safe: its execute runs on the submission thread (replayed CmdExternal), so
// every other XeSS call here comes after Submitter::AwaitAll (or before the
// first execute).
#include <rex/graphics/native/temporal_upscaler.h>
#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/submission.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/xess.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(native_xess_responsive_mask, false, "GPU",
    "Give XeSS the reactive mask (opaque snapshot vs final scene) as its responsive pixel mask: try where "
    "particles or glass smear. Intel advises it only where it clearly helps.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if defined(REX_HAS_XESS_SDK) && REX_HAS_XESS_SDK && defined(_WIN32)
#define REX_XESS_RUNTIME 1
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <rex/graphics/pipeline/shader/storage.h>
#include <rex/runtime.h>
#include <rex/ui/vulkan/device.h>
#include <xess/xess_vk.h>
#else
#define REX_XESS_RUNTIME 0
#endif

namespace rex::graphics::native {
#if REX_XESS_RUNTIME
namespace {
void Log(const char* message, xess_logging_level_t level) {
  // May come from XeSS's own threads.
  if (level >= XESS_LOGGING_LEVEL_ERROR) REXGPU_ERROR("XeSS: {}", message ? message : "");
  else REXGPU_WARN("XeSS: {}", message ? message : "");
}

xess_vk_image_view_info View(const ResolvedTexture* image) {
  xess_vk_image_view_info info{};
  if (!image || !image->image) return info;
  info.imageView = image->view; info.image = image->image;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  info.format = image->vk_format; info.width = image->host_width; info.height = image->host_height;
  return info;
}

const char* QualityName(xess_quality_settings_t quality) {
  switch (quality) {
    case XESS_QUALITY_SETTING_AA: return "Native AA";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS: return "Ultra Quality Plus";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "Ultra Quality";
    case XESS_QUALITY_SETTING_QUALITY: return "Quality";
    case XESS_QUALITY_SETTING_BALANCED: return "Balanced";
    case XESS_QUALITY_SETTING_PERFORMANCE: return "Performance";
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return "Ultra Performance";
    default: return "?";
  }
}

class XessUpscaler final : public TemporalUpscaler {
 public:
  XessUpscaler(Submitter& submitter, HMODULE module)
      : TemporalUpscaler(UpscalerProvider::kXess, submitter), module_(module) {}
  ~XessUpscaler() override;
  bool Initialize(std::string& reason);
  bool Dispatch(const Frame& frame) override;
  uint32_t JitterPhases(uint32_t render_width, uint32_t display_width) override;
  double MaxUpscaleRatio() const override { return 3.0; }  // Ultra Performance.

 private:
  // XeSS initialised for this output, flags and an input size in its preset's
  // range (the preset: the first, from Native AA up, whose range has it).
  bool Init(uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height,
            uint32_t flags);
  static void Replay(VkCommandBuffer command_buffer, const void* payload, void* user);
  template <class F> F Get(const char* name) { return reinterpret_cast<F>(GetProcAddress(module_, name)); }

  HMODULE module_ = nullptr;  // libxess.dll, owned by ui/vulkan/xess.h (kept for the process).
  decltype(&xessVKCreateContext) create_context_ = nullptr;
  decltype(&xessVKBuildPipelines) build_pipelines_ = nullptr;
  decltype(&xessGetPipelineBuildStatus) build_status_ = nullptr;
  decltype(&xessVKInit) init_ = nullptr;
  decltype(&xessVKExecute) execute_ = nullptr;
  decltype(&xessDestroyContext) destroy_context_ = nullptr;
  decltype(&xessGetOptimalInputResolution) input_range_ = nullptr;
  decltype(&xessGetIntelXeFXVersion) xefx_version_ = nullptr;
  decltype(&xessIsOptimalDriver) optimal_driver_ = nullptr;
  decltype(&xessSetLoggingCallback) set_logging_ = nullptr;
  xess_context_handle_t context_ = nullptr;
  // Pipelines (kernel compilation, seconds on some GPUs): built in the
  // background for the first frame's flags, cached on disk between runs.
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::filesystem::path cache_path_;
  size_t saved_cache_size_ = 0;
  void SaveCache();
  enum class Build { kNone, kBuilding, kDone } build_ = Build::kNone;
  bool initialized_ = false, init_failed_ = false;
  uint32_t init_flags_ = 0, init_output_width_ = 0, init_output_height_ = 0;
  xess_quality_settings_t init_quality_ = XESS_QUALITY_SETTING_AA;
  xess_2d_t input_min_{}, input_max_{};
  // An output size other than the initialised one waits to settle (cutscene
  // bars resize the scene each frame); flags too (a stray frame of another format).
  uint32_t pending_width_ = 0, pending_height_ = 0, pending_frames_ = 0, flag_mismatch_frames_ = 0;
  int logged_color_ = -1;
};

bool XessUpscaler::Initialize(std::string& reason) {
  create_context_ = Get<decltype(create_context_)>("xessVKCreateContext");
  build_pipelines_ = Get<decltype(build_pipelines_)>("xessVKBuildPipelines");
  build_status_ = Get<decltype(build_status_)>("xessGetPipelineBuildStatus");
  init_ = Get<decltype(init_)>("xessVKInit");
  execute_ = Get<decltype(execute_)>("xessVKExecute");
  destroy_context_ = Get<decltype(destroy_context_)>("xessDestroyContext");
  input_range_ = Get<decltype(input_range_)>("xessGetOptimalInputResolution");
  xefx_version_ = Get<decltype(xefx_version_)>("xessGetIntelXeFXVersion");
  optimal_driver_ = Get<decltype(optimal_driver_)>("xessIsOptimalDriver");
  set_logging_ = Get<decltype(set_logging_)>("xessSetLoggingCallback");
  if (!create_context_ || !build_pipelines_ || !build_status_ || !init_ || !execute_ || !destroy_context_ ||
      !input_range_ || !xefx_version_ || !optimal_driver_ || !set_logging_) {
    reason = "XeSS entry points missing in libxess.dll";
    return false;
  }
  const auto* device = submitter_->vulkan_device();
  if (!device->xess_ready()) {
    reason = "the Vulkan device lacks XeSS's requirements: " +
             (device->xess_note().empty() ? std::string("unknown") : device->xess_note());
    return false;
  }
  const xess_result_t result = create_context_(device->vulkan_instance()->instance(), device->physical_device(),
                                               device->device(), &context_);
  if (result != XESS_RESULT_SUCCESS || !context_) {
    context_ = nullptr;
    reason = "context creation failed: " + ui::vulkan::XessResultText(result);
    return false;
  }
  set_logging_(context_, XESS_LOGGING_LEVEL_WARNING, Log);
  if (optimal_driver_(context_) == XESS_RESULT_WARNING_OLD_DRIVER)
    REXGPU_WARN("XeSS: the GPU driver is older than XeSS prefers (lower speed or quality possible)");
  // The pipeline cache of earlier runs, per GPU (the driver ignores a foreign
  // one), beside the renderer's own driver cache (none without a cache root).
  const auto& properties = device->properties();
  if (const rex::Runtime* runtime = rex::Runtime::instance(); runtime && !runtime->cache_root().empty())
    cache_path_ = GetShaderStorageLocalRoot(runtime->cache_root()) /
                  fmt::format("xess_vk_{:04x}_{:04x}.bin", properties.vendorID, properties.deviceID);
  std::vector<char> data;
  if (!cache_path_.empty())
    if (std::ifstream file{cache_path_, std::ios::binary}) data.assign(std::istreambuf_iterator<char>(file), {});
  VkPipelineCacheCreateInfo cache_info{};
  cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  cache_info.initialDataSize = data.size(); cache_info.pInitialData = data.empty() ? nullptr : data.data();
  const auto& fn = device->functions();
  if (fn.vkCreatePipelineCache(device->device(), &cache_info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
    cache_info.initialDataSize = 0; cache_info.pInitialData = nullptr;
    if (fn.vkCreatePipelineCache(device->device(), &cache_info, nullptr, &pipeline_cache_) != VK_SUCCESS)
      pipeline_cache_ = VK_NULL_HANDLE;
  }
  saved_cache_size_ = cache_info.initialDataSize;
  version_ = "SR " + ui::vulkan::XessVersion();
  REXGPU_INFO("XeSS: SR {} context on '{}', pipeline cache {} bytes", ui::vulkan::XessVersion(),
              properties.deviceName, data.size());
  return true;
}

XessUpscaler::~XessUpscaler() {
  submitter_->AwaitAll();  // Replayed executes reference the context.
  // A pending background build finishes before the context goes.
  if (context_) destroy_context_(context_);
  const auto* device = submitter_->vulkan_device();
  if (pipeline_cache_) {
    SaveCache();
    device->functions().vkDestroyPipelineCache(device->device(), pipeline_cache_, nullptr);
  }
}

void XessUpscaler::SaveCache() {
  // After each initialisation (it builds what is missing): the process may end
  // without destroying the upscaler. Written only when the cache grew.
  if (!pipeline_cache_ || build_ == Build::kNone || cache_path_.empty()) return;
  const auto* device = submitter_->vulkan_device();
  const auto& fn = device->functions();
  size_t size = 0;
  if (fn.vkGetPipelineCacheData(device->device(), pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size ||
      size == saved_cache_size_)
    return;
  std::vector<char> data(size);
  if (fn.vkGetPipelineCacheData(device->device(), pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
  std::error_code error;
  std::filesystem::create_directories(cache_path_.parent_path(), error);
  std::ofstream file{cache_path_, std::ios::binary | std::ios::trunc};
  if (!file || !file.write(data.data(), std::streamsize(size))) {
    REXGPU_WARN("XeSS: pipeline cache not saved to {}", cache_path_.string());
    return;
  }
  saved_cache_size_ = size;
  REXGPU_INFO("XeSS: pipeline cache saved, {} bytes", size);
}

uint32_t XessUpscaler::JitterPhases(uint32_t render_width, uint32_t display_width) {
  return UpscalerJitterPhases(render_width, display_width);  // Intel: 8 * (scale)^2.
}

bool XessUpscaler::Init(uint32_t input_width, uint32_t input_height, uint32_t output_width,
                        uint32_t output_height, uint32_t flags) {
  if (initialized_) submitter_->AwaitAll();  // No execute may run during XeSS calls.
  const xess_2d_t output{output_width, output_height};
  static constexpr xess_quality_settings_t kPresets[] = {
      XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY,
      XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
  bool found = false;
  for (const xess_quality_settings_t quality : kPresets) {
    xess_2d_t optimal{}, minimum{}, maximum{};
    if (input_range_(context_, &output, quality, &optimal, &minimum, &maximum) != XESS_RESULT_SUCCESS) continue;
    if (input_width >= minimum.x && input_width <= maximum.x && input_height >= minimum.y &&
        input_height <= maximum.y) {
      init_quality_ = quality; input_min_ = minimum; input_max_ = maximum; found = true;
      break;
    }
  }
  if (!found) {
    last_error_ = fmt::format("{}x{} -> {}x{} is outside XeSS's ratios (up to 3x)", input_width, input_height,
                              output_width, output_height);
    initialized_ = false;
    return false;
  }
  xess_vk_init_params_t params{};
  params.outputResolution = output; params.qualitySetting = init_quality_; params.initFlags = flags;
  params.pipelineCache = pipeline_cache_;
  const uint64_t start = GetTickCount64();
  const xess_result_t result = init_(context_, &params);
  if (result != XESS_RESULT_SUCCESS) {
    // Not again each frame: the provider is given up (SceneFrames sees failed()).
    last_error_ = "init failed: " + ui::vulkan::XessResultText(result);
    initialized_ = false; init_failed_ = true;
    replay_failed_.store(true, std::memory_order_release);
    REXGPU_ERROR("XeSS: {}", last_error_);
    return false;
  }
  initialized_ = true;
  init_flags_ = flags; init_output_width_ = output_width; init_output_height_ = output_height;
  xess_version_t xefx{};
  const bool intel = xefx_version_(context_, &xefx) == XESS_RESULT_SUCCESS && (xefx.major || xefx.minor || xefx.patch);
  version_ = fmt::format("SR {}{}, {}", ui::vulkan::XessVersion(),
                         intel ? fmt::format(" (Intel XeFX {}.{}.{})", xefx.major, xefx.minor, xefx.patch) : "",
                         QualityName(init_quality_));
  REXGPU_INFO("XeSS: init {}x{} -> {}x{}, {} (input {}x{}..{}x{}), flags {:#x}, {} ms; {}", input_width,
              input_height, output_width, output_height, QualityName(init_quality_), input_min_.x, input_min_.y,
              input_max_.x, input_max_.y, flags, GetTickCount64() - start,
              intel ? "Intel XeFX implementation" : "cross-vendor implementation");
  SaveCache();
  return true;
}

void XessUpscaler::Replay(VkCommandBuffer command_buffer, const void* payload, void* user) {
  auto* self = static_cast<XessUpscaler*>(user);
  xess_vk_execute_params_t params;
  std::memcpy(&params, payload, sizeof(params));
  const xess_result_t result = self->execute_(self->context_, command_buffer, &params);
  if (result != XESS_RESULT_SUCCESS) {
    REXGPU_ERROR("XeSS: execute failed: {}", ui::vulkan::XessResultText(result));
    self->replay_failed_.store(true, std::memory_order_release);
  }
}

bool XessUpscaler::Dispatch(const Frame& frame) {
  if (init_failed_) return false;
  if (!frame.color || !frame.depth || !frame.motion || !frame.output || !frame.width || !frame.height) {
    last_error_ = "incomplete inputs"; return false;
  }
  const ColorSpace color = SceneColorSpace(frame.color_format);
  if (int(color) != logged_color_) {
    REXGPU_INFO("XeSS: color {} ({}, guest format {}){}", ColorSpaceName(color), REXCVAR_GET(native_fsr_color_space),
                frame.color_format, color == ColorSpace::kHdr ? "" : ": LDR input");
    logged_color_ = int(color);
  }
  const bool responsive = REXCVAR_GET(native_xess_responsive_mask) && frame.reactive;
  const uint32_t flags = (color == ColorSpace::kHdr ? 0u : uint32_t(XESS_INIT_FLAG_LDR_INPUT_COLOR)) |
                         (frame.camera.depth_inverted ? uint32_t(XESS_INIT_FLAG_INVERTED_DEPTH) : 0u) |
                         (responsive ? uint32_t(XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK) : 0u);
  const uint32_t output_width = frame.output_width ? frame.output_width : frame.width;
  const uint32_t output_height = frame.output_height ? frame.output_height : frame.height;
  if (!initialized_) {
    // First: XeSS's pipelines in the background (the original image meanwhile),
    // for this frame's flags; then the initialisation is short.
    if (build_ == Build::kNone) {
      const xess_result_t result = build_pipelines_(context_, pipeline_cache_, false, flags);
      build_ = result == XESS_RESULT_SUCCESS ? Build::kBuilding : Build::kDone;
      if (result != XESS_RESULT_SUCCESS)
        REXGPU_WARN("XeSS: background pipeline build unavailable ({}); built at initialisation",
                    ui::vulkan::XessResultText(result));
    }
    if (build_ == Build::kBuilding) {
      if (build_status_(context_) == XESS_RESULT_ERROR_OPERATION_IN_PROGRESS) {
        last_error_ = "building XeSS pipelines (the first run per GPU takes a while)"; return false;
      }
      build_ = Build::kDone;
    }
    if (!Init(frame.width, frame.height, output_width, output_height, flags)) return false;
  } else {
    // Re-initialising waits for the GPU and drops the history: for a lasting
    // change only (output size settled, flags for many frames).
    constexpr uint32_t kSizeSettleFrames = 4, kFlagChangeFrames = 30;
    const bool size_changed = output_width != init_output_width_ || output_height != init_output_height_;
    if (size_changed) {
      if (output_width != pending_width_ || output_height != pending_height_) {
        pending_width_ = output_width; pending_height_ = output_height; pending_frames_ = 0;
      }
      if (++pending_frames_ < kSizeSettleFrames) { last_error_ = "output size changing"; return false; }
    }
    pending_frames_ = 0;
    if (!size_changed && flags != init_flags_ && ++flag_mismatch_frames_ < kFlagChangeFrames) {
      last_error_ = "source format differs from the initialisation's (transient)"; return false;
    }
    flag_mismatch_frames_ = 0;
    const bool in_range = frame.width >= input_min_.x && frame.width <= input_max_.x &&
                          frame.height >= input_min_.y && frame.height <= input_max_.y;
    if ((size_changed || flags != init_flags_ || !in_range) &&
        !Init(frame.width, frame.height, output_width, output_height, flags)) {
      return false;
    }
  }
  xess_vk_execute_params_t params{};
  params.colorTexture = View(frame.color);
  params.velocityTexture = View(frame.motion);  // RG of RGBA16F: pixels, current -> previous, no jitter.
  params.depthTexture = View(frame.depth);
  if (responsive) params.responsivePixelMaskTexture = View(frame.reactive);
  params.outputTexture = View(frame.output);
  params.jitterOffsetX = frame.jitter[0]; params.jitterOffsetY = frame.jitter[1];
  params.exposureScale = 1.f;
  params.resetHistory = frame.reset ? 1u : 0u;
  params.inputWidth = frame.width; params.inputHeight = frame.height;
  auto* commands = submitter_->OpenOutsideRenderPass();
  if (!commands) { last_error_ = "no command buffer"; return false; }
  commands->CmdExternal(&XessUpscaler::Replay, this, &params, sizeof(params));
  ++dispatches_;
  return true;
}
}  // namespace
#endif

std::unique_ptr<TemporalUpscaler> LoadXessUpscaler(Submitter& submitter, std::string& reason) {
#if REX_XESS_RUNTIME
  auto* module = static_cast<HMODULE>(ui::vulkan::XessModule(&reason));
  if (!module) return nullptr;
  auto upscaler = std::make_unique<XessUpscaler>(submitter, module);
  if (!upscaler->Initialize(reason)) return nullptr;
  return upscaler;
#else
  (void)submitter;
  reason = "Intel XeSS not available in this build/platform (Windows with REXGLUE_ENABLE_XESS_SDK)";
  return nullptr;
#endif
}
}  // namespace rex::graphics::native
