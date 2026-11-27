#include <rex/graphics/native/temporal_upscaler.h>
#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/fsr_versions.h>
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fmt/format.h>

REXCVAR_DEFINE_STRING(native_temporal_upscaler, "off", "GPU",
    "Temporal upscaler on the title's scene (needs a scene producer; turns native_scene_frames on): off, "
    "fsr3 (AMD FSR 3.1), fsr4 (AMD FSR 4 where the GPU offers it: Radeon RX 6000 and newer; elsewhere FSR 3.1, "
    "noted in logs/upscaler.log), xess (Intel XeSS SR: Intel Arc/Iris Xe, other GPUs with "
    "shaderStorageImageWriteWithoutFormat and mutable descriptors), dlss (NVIDIA DLSS SR, DLAA at native "
    "resolution: GeForce RTX); fsr = fsr3. With native_scene_upscale the "
    "scene is drawn at 1x and upscaled to the resolution scale, else native-resolution AA. Unavailable -> "
    "original image.")
    .allowed({"off", "fsr3", "fsr4", "xess", "dlss", "fsr"}).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(native_fsr_sharpness, 0.5, "GPU", "FSR sharpening (RCAS), 0 off .. 1 maximum (XeSS has none).")
    .range(0.0, 1.0).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(native_fsr_color_space, "auto", "GPU",
    "Scene color encoding given to the temporal upscaler: auto (by the guest target format: float/16-bit -> hdr, "
    "else srgb), srgb (perceptual LDR), linear (linear LDR), hdr (linear, values above 1). XeSS: srgb and linear "
    "are its LDR input.")
    .allowed({"auto", "srgb", "linear", "hdr"}).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_fsr_debug_checking, false, "GPU",
    "FSR runtime input validation; messages go to the log.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_fsr_reactive, true, "GPU",
    "Give FSR the reactive mask (opaque snapshot vs final scene). Off: compare when particles/glass smear or the image is under-smoothed.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if defined(REX_HAS_FSR_SDK) && REX_HAS_FSR_SDK && defined(_WIN32)
#define REX_FSR_RUNTIME 1
#include <rex/ui/vulkan/device.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <api/include/vk/ffx_api_vk.h>
#include <upscalers/include/ffx_upscale.h>
#else
#define REX_FSR_RUNTIME 0
#endif

namespace rex::graphics::native {

std::unique_ptr<TemporalUpscaler> TemporalUpscaler::Load(UpscalerProvider provider, Submitter& submitter,
                                                         std::string& reason) {
  switch (provider) {
    case UpscalerProvider::kXess: return LoadXessUpscaler(submitter, reason);
    case UpscalerProvider::kDlss: return LoadDlssUpscaler(submitter, reason);
    default: return LoadFsrUpscaler(submitter, reason);
  }
}

TemporalUpscaler::ColorSpace TemporalUpscaler::SceneColorSpace(uint32_t color_format) {
  return UpscalerColorSpaceFor(REXCVAR_GET(native_fsr_color_space), color_format);
}

#if REX_FSR_RUNTIME
namespace {
std::atomic<PFN_vkGetDeviceProcAddr> g_get_device_proc_addr{nullptr};
// Some drivers expose Vulkan 1.1+ core entry points but not the KHR aliases
// the FidelityFX Vulkan backend asks for.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
  const auto get = g_get_device_proc_addr.load(std::memory_order_relaxed);
  if (!get || !name) return nullptr;
  if (auto proc = get(device, name)) return proc;
  static constexpr const char* kAliases[][2] = {
      {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
      {"vkGetImageMemoryRequirements2KHR", "vkGetImageMemoryRequirements2"},
      {"vkBindBufferMemory2KHR", "vkBindBufferMemory2"},
      {"vkBindImageMemory2KHR", "vkBindImageMemory2"}};
  for (const auto& alias : kAliases)
    if (!std::strcmp(name, alias[0])) return get(device, alias[1]);
  return nullptr;
}
void Message(uint32_t type, const wchar_t* message) {
  std::string text;
  for (const wchar_t* c = message; c && *c; ++c) text.push_back(*c < 128 ? char(*c) : '?');
  if (type == FFX_API_MESSAGE_TYPE_ERROR) REXGPU_ERROR("FSR: {}", text); else REXGPU_WARN("FSR: {}", text);
}
FfxApiResource Resource(const ResolvedTexture* image, uint32_t format, bool storage) {
  if (!image || !image->image) return {};
  FfxApiResourceDescription description{};
  description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D; description.format = format;
  description.width = image->host_width; description.height = image->host_height; description.depth = 1;
  description.mipCount = 1; description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  description.usage = storage ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
  return ffxApiGetResourceVK(image->image, description,
                             storage ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
}

class FsrUpscaler final : public TemporalUpscaler {
 public:
  FsrUpscaler(Submitter& submitter, HMODULE module)
      : TemporalUpscaler(UpscalerProvider::kFsr, submitter), module_(module) {}
  ~FsrUpscaler() override {
    submitter_->AwaitAll();  // Replayed dispatches reference the context.
    DestroyContext();
    if (module_) FreeLibrary(module_);
  }
  bool Initialize(std::string& reason);
  bool Dispatch(const Frame& frame) override;
  void SetWantedMajor(int wanted) override;
  uint32_t JitterPhases(uint32_t render_width, uint32_t display_width) override;

 private:
  bool EnsureContext(uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height,
                     uint32_t flags);
  void DestroyContext();
  static void Replay(VkCommandBuffer command_buffer, const void* payload, void* user);

  HMODULE module_ = nullptr;
  PfnFfxCreateContext create_ = nullptr; PfnFfxDestroyContext destroy_ = nullptr;
  PfnFfxQuery query_ = nullptr; PfnFfxDispatch dispatch_ = nullptr;
  void* context_ = nullptr;  // ffxContext.
  uint32_t context_width_ = 0, context_height_ = 0, context_flags_ = 0;
  uint32_t context_output_width_ = 0, context_output_height_ = 0;
  std::vector<uint64_t> version_ids_;  // Providers on this device, newest first.
  std::vector<std::string> version_names_;
  int major_ = 3;          // The wanted version, if the GPU offers it.
  int wanted_major_ = 0;   // SetWantedMajor's last value.
  int logged_color_ = -1; float logged_sharpness_ = -1.f;  // Logged on change.
  // Frames in a row whose context flags differ from the live context's: a
  // one-frame outlier (another source target) is skipped, not re-created.
  uint32_t flag_mismatch_frames_ = 0;
};

bool FsrUpscaler::Initialize(std::string& reason) {
  create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module_, "ffxCreateContext"));
  destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module_, "ffxDestroyContext"));
  query_ = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module_, "ffxQuery"));
  dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module_, "ffxDispatch"));
  if (!create_ || !destroy_ || !query_ || !dispatch_) { reason = "ffx-api entry points missing in the loader DLL"; return false; }
  const auto* device = submitter_->vulkan_device();
  const auto* instance = device->vulkan_instance();
  auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      instance->functions().vkGetInstanceProcAddr(instance->instance(), "vkGetDeviceProcAddr"));
  if (!get) { reason = "vkGetDeviceProcAddr unavailable"; return false; }
  g_get_device_proc_addr.store(get, std::memory_order_relaxed);
  // Available upscaler versions on this device (newest first): the fallback chain.
  VkDeviceContext context{device->device(), device->physical_device(), GetDeviceProcAddr};
  uint64_t count = 0;
  ffxQueryDescGetVersions versions{};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = &context; versions.outputCount = &count;
  if (query_(nullptr, &versions.header) == FFX_API_RETURN_OK && count) {
    std::vector<const char*> names(count, nullptr);
    version_ids_.resize(count);
    versions.versionIds = version_ids_.data(); versions.versionNames = names.data();
    if (query_(nullptr, &versions.header) == FFX_API_RETURN_OK) {
      version_ids_.resize(std::min<size_t>(size_t(count), names.size()));
      std::string list;
      for (size_t i = 0; i < version_ids_.size(); ++i) {
        version_names_.emplace_back(names[i] ? names[i] : "");
        list += fmt::format("{}{}", i ? ", " : "", version_names_.back());
      }
      REXGPU_INFO("FSR: upscaler versions on this device: {}", list);
    } else version_ids_.clear();
  }
  return true;
}

void FsrUpscaler::SetWantedMajor(int wanted) {
  if (wanted == wanted_major_) return;
  wanted_major_ = wanted;
  // FSR 4 only where the GPU offers it; FSR 3 otherwise.
  const bool fsr4 = rex::ui::vulkan::FsrHasMajor(version_names_, 4);
  const int major = wanted >= 4 && fsr4 ? 4 : 3;
  fallback_note_ = wanted >= 4 && !fsr4 ? " (FSR 4 unavailable on this GPU)" : "";
  if (wanted >= 4 && !fsr4) REXGPU_WARN("FSR: FSR 4 is not offered for this GPU, FSR 3 instead");
  if (major == major_) return;
  major_ = major;
  if (context_) { submitter_->AwaitAll(); DestroyContext(); }  // The next dispatch creates it.
}

void FsrUpscaler::DestroyContext() {
  if (context_) destroy_(reinterpret_cast<ffxContext*>(&context_), nullptr);
  context_ = nullptr; context_width_ = context_height_ = context_flags_ = 0;
}

bool FsrUpscaler::EnsureContext(uint32_t width, uint32_t height, uint32_t output_width,
                                uint32_t output_height, uint32_t flags) {
  // One context per flags for sizes up to its own; render and output sizes
  // below them are dispatch parameters (dynamic resolution, upscaleSize), so a
  // frame drawn at another scale or a letterbox step costs no re-creation (a
  // GPU wait and lost history).
  flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
  width = std::max(width, output_width); height = std::max(height, output_height);
  if (context_ && width <= context_width_ && height <= context_height_ && output_width <= context_output_width_ &&
      output_height <= context_output_height_ && flags == context_flags_) return true;
  const bool same_flags = context_ && flags == context_flags_;
  if (context_) { submitter_->AwaitAll(); DestroyContext(); }
  const auto* device = submitter_->vulkan_device();
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext = {device->device(), device->physical_device(), GetDeviceProcAddr};
  ffxCreateContextDescUpscale create{};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; create.header.pNext = &backend.header;
  create.flags = flags; create.maxRenderSize = {width, height}; create.maxUpscaleSize = {output_width, output_height};
  create.fpMessage = Message;
  // The chosen major version's providers, newest first, then FSR 3 (FSR 4 ->
  // FSR 3.1); none listed: the loader's default.
  ffxOverrideVersion override_version{};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  const std::vector<size_t> order = rex::ui::vulkan::FsrVersionOrder(version_names_, major_);
  ffxReturnCode_t result = FFX_API_RETURN_ERROR;
  for (size_t i = 0; i < std::max<size_t>(order.size(), 1); ++i) {
    context_ = nullptr;
    if (!order.empty()) { override_version.versionId = version_ids_[order[i]]; backend.header.pNext = &override_version.header; }
    result = create_(reinterpret_cast<ffxContext*>(&context_), &create.header, nullptr);
    if (result == FFX_API_RETURN_OK && context_) break;
  }
  if (result != FFX_API_RETURN_OK || !context_) {
    context_ = nullptr; last_error_ = fmt::format("context creation failed ({})", result); return false;
  }
  context_width_ = width; context_height_ = height; context_flags_ = flags;
  context_output_width_ = output_width; context_output_height_ = output_height;
  ffxQueryGetProviderVersion provider{};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (query_(reinterpret_cast<ffxContext*>(&context_), &provider.header) == FFX_API_RETURN_OK && provider.versionName)
    version_ = provider.versionName;
  if (major_ >= 4 && rex::ui::vulkan::FsrMajor(version_) < 4) fallback_note_ = " (FSR 4 failed on this GPU)";
  // A larger size alone (the scene grew): in the debug log.
  if (same_flags) {
    REXGPU_DEBUG("FSR: context {}x{} -> {}x{} flags {:#x}, provider {}", width, height, output_width, output_height,
                 flags, version_.empty() ? "?" : version_);
  } else {
    REXGPU_INFO("FSR: context {}x{} -> {}x{} flags {:#x}, provider {}", width, height, output_width, output_height,
                flags, version_.empty() ? "?" : version_);
  }
  return true;
}

uint32_t FsrUpscaler::JitterPhases(uint32_t render_width, uint32_t display_width) {
  int32_t phases = 0;
  ffxQueryDescUpscaleGetJitterPhaseCount query{};
  query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  query.renderWidth = render_width; query.displayWidth = display_width; query.pOutPhaseCount = &phases;
  if (context_ && query_(reinterpret_cast<ffxContext*>(&context_), &query.header) == FFX_API_RETURN_OK && phases > 0)
    return uint32_t(phases);
  return 8;  // FSR's count at a 1:1 ratio.
}

void FsrUpscaler::Replay(VkCommandBuffer command_buffer, const void* payload, void* user) {
  auto* self = static_cast<FsrUpscaler*>(user);
  ffxDispatchDescUpscale dispatch;
  std::memcpy(&dispatch, payload, sizeof(dispatch));
  dispatch.commandList = command_buffer;
  if (self->dispatch_(reinterpret_cast<ffxContext*>(&self->context_), &dispatch.header) != FFX_API_RETURN_OK)
    self->replay_failed_.store(true, std::memory_order_release);
}

bool FsrUpscaler::Dispatch(const Frame& frame) {
  if (!frame.color || !frame.depth || !frame.motion || !frame.output || !frame.width || !frame.height) {
    last_error_ = "incomplete inputs"; return false;
  }
  const ColorSpace color = SceneColorSpace(frame.color_format);
  const bool srgb = color == ColorSpace::kSrgb;
  const float sharpness = float(REXCVAR_GET(native_fsr_sharpness));
  if (int(color) != logged_color_ || sharpness != logged_sharpness_) {
    REXGPU_INFO("FSR: color {} ({}, guest format {}), sharpness {:.2f}", ColorSpaceName(color),
                REXCVAR_GET(native_fsr_color_space), frame.color_format, sharpness);
    logged_color_ = int(color); logged_sharpness_ = sharpness;
  }
  uint32_t flags = (srgb ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0) |
                   (color == ColorSpace::kHdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
                   (frame.camera.depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
                   (frame.camera.infinite_far ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0) |
                   (REXCVAR_GET(native_fsr_debug_checking) ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
  // Re-creating the context waits for the GPU and drops the history: only
  // for a lasting change (cvar, scene format), not a stray frame.
  constexpr uint32_t kFlagChangeFrames = 30;
  const uint32_t output_width = frame.output_width ? frame.output_width : frame.width;
  const uint32_t output_height = frame.output_height ? frame.output_height : frame.height;
  if (context_ && (flags | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != context_flags_ &&
      output_width <= context_output_width_ && output_height <= context_output_height_ &&
      ++flag_mismatch_frames_ < kFlagChangeFrames) {
    last_error_ = "source format differs from the context's (transient)"; return false;
  }
  flag_mismatch_frames_ = 0;
  if (!EnsureContext(frame.width, frame.height, output_width, output_height, flags)) return false;
  ffxDispatchDescUpscale dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.color = Resource(frame.color, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, false);
  dispatch.depth = Resource(frame.depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, false);
  dispatch.motionVectors = Resource(frame.motion, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, false);
  dispatch.reactive = Resource(frame.reactive, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, false);
  dispatch.output = Resource(frame.output, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, true);
  dispatch.jitterOffset = {frame.jitter[0], frame.jitter[1]};
  dispatch.motionVectorScale = {1.f, 1.f};  // Already scene pixels.
  dispatch.renderSize = {frame.width, frame.height}; dispatch.upscaleSize = {output_width, output_height};
  dispatch.enableSharpening = sharpness > 0; dispatch.sharpness = sharpness;
  dispatch.frameTimeDelta = frame.delta_ms > 0 ? frame.delta_ms : 1000.f / 60.f;
  dispatch.preExposure = 1.f; dispatch.reset = frame.reset;
  dispatch.cameraNear = float(frame.camera.near_distance);
  dispatch.cameraFar = frame.camera.infinite_far ? FLT_MAX : float(frame.camera.far_distance);
  dispatch.cameraFovAngleVertical = float(frame.camera.fov_y);
  dispatch.viewSpaceToMetersFactor = frame.meters_per_unit > 0 ? frame.meters_per_unit : 1.f;
  dispatch.flags = srgb ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
  auto* commands = submitter_->OpenOutsideRenderPass();
  if (!commands) { last_error_ = "no command buffer"; return false; }
  commands->CmdExternal(&FsrUpscaler::Replay, this, &dispatch, sizeof(dispatch));
  ++dispatches_;
  return true;
}
}  // namespace
#endif

std::unique_ptr<TemporalUpscaler> LoadFsrUpscaler(Submitter& submitter, std::string& reason) {
#if REX_FSR_RUNTIME
  // Next to the executable (staged by rexglue_setup_target with the native plugin).
  HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (!module) { reason = "amd_fidelityfx_loader_vk.dll not found next to the executable"; return nullptr; }
  auto upscaler = std::make_unique<FsrUpscaler>(submitter, module);
  if (!upscaler->Initialize(reason)) return nullptr;
  return upscaler;
#else
  (void)submitter;
  reason = "AMD FSR SDK not available in this build/platform (Windows with REXGLUE_ENABLE_FSR_SDK)";
  return nullptr;
#endif
}
}  // namespace rex::graphics::native
