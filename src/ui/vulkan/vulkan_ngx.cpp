#include <rex/ui/vulkan/ngx.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#if defined(REX_HAS_DLSS_SDK) && REX_HAS_DLSS_SDK && defined(_WIN32)
#define REX_NGX_RUNTIME 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_params_dlssg.h>
#include <nvsdk_ngx_vk.h>
#else
#define REX_NGX_RUNTIME 0
#endif

namespace rex::ui::vulkan::ngx {

namespace {
constexpr const char* kUnavailable =
    "NVIDIA DLSS not available in this build/platform (Windows x64 with REXGLUE_ENABLE_DLSS_SDK)";

const wchar_t* DllName(Feature feature) {
  return feature == Feature::kFrameGeneration ? L"nvngx_dlssg.dll" : L"nvngx_dlss.dll";
}
}  // namespace

const char* FeatureName(Feature feature) {
  return feature == Feature::kFrameGeneration ? "DLSS Frame Generation" : "DLSS";
}

bool RuntimePresent(Feature feature, std::string* reason) {
#if REX_NGX_RUNTIME
  std::error_code ec;
  if (std::filesystem::exists(rex::filesystem::GetExecutableFolder() / DllName(feature), ec)) return true;
  if (reason)
    *reason = fmt::format("{} not found next to the executable",
                          feature == Feature::kFrameGeneration ? "nvngx_dlssg.dll" : "nvngx_dlss.dll");
  return false;
#else
  (void)feature;
  if (reason) *reason = kUnavailable;
  return false;
#endif
}

std::string RuntimeVersion(Feature feature) {
#if REX_NGX_RUNTIME
  const std::wstring path = (rex::filesystem::GetExecutableFolder() / DllName(feature)).wstring();
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!size) return {};
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return {};
  const uint32_t d = LOWORD(info->dwFileVersionLS);
  return d ? fmt::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                         HIWORD(info->dwFileVersionLS), d)
           : fmt::format("{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                         HIWORD(info->dwFileVersionLS));
#else
  (void)feature;
  return {};
#endif
}

const char* SrModeName(SrMode mode) {
  switch (mode) {
    case SrMode::kDlaa: return "DLAA";
    case SrMode::kUltraQuality: return "Ultra Quality";
    case SrMode::kQuality: return "Quality";
    case SrMode::kBalanced: return "Balanced";
    case SrMode::kPerformance: return "Performance";
    case SrMode::kUltraPerformance: return "Ultra Performance";
  }
  return "?";
}

std::string ResultText(uint32_t result) {
  const char* name = "error";
  switch (result) {
    case 0x1: name = "success"; break;
    case 0xBAD00000: name = "failure"; break;
    case 0xBAD00001: name = "feature not supported"; break;
    case 0xBAD00002: name = "platform error"; break;
    case 0xBAD00003: name = "feature already exists"; break;
    case 0xBAD00004: name = "feature not found"; break;
    case 0xBAD00005: name = "invalid parameter"; break;
    case 0xBAD00006: name = "scratch buffer too small"; break;
    case 0xBAD00007: name = "not initialized"; break;
    case 0xBAD00008: name = "unsupported input format"; break;
    case 0xBAD00009: name = "read/write flag missing"; break;
    case 0xBAD0000A: name = "missing input"; break;
    case 0xBAD0000B: name = "unable to initialize the feature (DLL)"; break;
    case 0xBAD0000C: name = "out of date (driver or DLL)"; break;
    case 0xBAD0000D: name = "out of GPU memory"; break;
    case 0xBAD0000E: name = "unsupported format"; break;
    case 0xBAD0000F: name = "unable to write to the data path"; break;
    case 0xBAD00010: name = "unsupported parameter"; break;
    case 0xBAD00011: name = "denied"; break;
    case 0xBAD00012: name = "not implemented"; break;
    default: break;
  }
  return fmt::format("{} ({:#x})", name, result);
}

#if REX_NGX_RUNTIME
struct Handle {
  NVSDK_NGX_Handle* handle = nullptr;
  NVSDK_NGX_Parameter* parameters = nullptr;
  VkDevice device = VK_NULL_HANDLE;
};

namespace {
// NGX is not thread safe: all its calls under this lock.
std::mutex& Lock() {
  static std::mutex mutex;
  return mutex;
}

struct DeviceState {
  bool initialized = false;
  std::string error;  // Initialisation failed: why (not retried).
  NVSDK_NGX_Parameter* capabilities = nullptr;
  std::set<Handle*> handles;
};
std::map<VkDevice, DeviceState>& Devices() {
  static std::map<VkDevice, DeviceState> devices;
  return devices;
}

constexpr const char* kEngineVersion = "rexglue";

// Extension names NGX listed, valid for the process.
const char* Keep(const char* name) {
  static std::set<std::string> names;
  return names.emplace(name).first->c_str();
}

// GUID-like, from the executable's name (stable per project, no title input).
const std::string& ProjectId() {
  static const std::string id = [] {
    std::string name = rex::filesystem::GetExecutablePath().stem().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto fnv = [&](uint64_t hash) {
      for (unsigned char c : name) hash = (hash ^ c) * 0x100000001b3ull;
      return hash;
    };
    const uint64_t a = fnv(0xcbf29ce484222325ull), b = fnv(0x84222325cbf29ce4ull ^ name.size());
    // Version 5-like layout, RFC 4122 variant.
    return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", uint32_t(a >> 32), uint32_t(a >> 16) & 0xffff,
                       (uint32_t(a) & 0x0fff) | 0x5000, (uint32_t(b >> 48) & 0x3fff) | 0x8000,
                       b & 0xffffffffffffull);
  }();
  return id;
}

const wchar_t* DataPath() {
  static const std::wstring path = [] {
    const auto directory = rex::filesystem::GetExecutableFolder() / "logs" / "ngx";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory.wstring();
  }();
  return path.c_str();
}

void NVSDK_CONV LogMessage(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  // Any thread; one line each.
  std::string text = message ? message : "";
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  if (text.empty()) return;
  // NGX's normal level is hundreds of lines per start (configuration, kernels,
  // presets): warnings and errors to the log, the rest at debug level.
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  // "failed to load ... from cache": the driver probing its model cache (normal).
  const bool cache_probe = lower.find("from cache") != std::string::npos;
  if (!cache_probe && (lower.find("error") != std::string::npos || lower.find("fail") != std::string::npos ||
                       lower.find("warn") != std::string::npos)) {
    REXLOG_WARN("NGX: {}", text);
  } else {
    REXLOG_DEBUG("NGX: {}", text);
  }
}

const NVSDK_NGX_FeatureCommonInfo* CommonInfo() {
  static const std::wstring folder = rex::filesystem::GetExecutableFolder().wstring();
  static const wchar_t* const paths[] = {folder.c_str()};
  static const NVSDK_NGX_FeatureCommonInfo info = [] {
    NVSDK_NGX_FeatureCommonInfo i{};
    i.PathListInfo.Path = paths;
    i.PathListInfo.Length = 1;
    i.LoggingInfo.LoggingCallback = LogMessage;
    i.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    i.LoggingInfo.DisableOtherLoggingSinks = false;
    return i;
  }();
  return &info;
}

NVSDK_NGX_Feature NgxFeature(Feature feature) {
  return feature == Feature::kFrameGeneration ? NVSDK_NGX_Feature_FrameGeneration
                                              : NVSDK_NGX_Feature_SuperSampling;
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery(Feature feature) {
  NVSDK_NGX_FeatureDiscoveryInfo info{};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NgxFeature(feature);
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = ProjectId().c_str();
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
  info.ApplicationDataPath = DataPath();
  info.FeatureInfo = CommonInfo();
  return info;
}

std::string SupportText(uint32_t bits, const NVSDK_NGX_FeatureRequirement& requirement) {
  std::string text;
  auto add = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
  if (bits & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) add("GPU not supported");
  if (bits & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) add("GPU driver too old");
  if (bits & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported)
    add(fmt::format("Windows too old (needs {})", requirement.MinOSVersion));
  if (bits & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) add("support check unavailable");
  if (bits & NVSDK_NGX_FeatureSupportResult_NotImplemented) add("not implemented");
  return text.empty() ? fmt::format("unsupported ({:#x})", bits) : text;
}

NVSDK_NGX_Resource_VK Resource(const Image& image) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                                image.format, image.width, image.height, image.storage);
}

void CopyMatrix(const Matrix& from, float (&to)[4][4]) {
  for (size_t row = 0; row < 4; ++row)
    for (size_t column = 0; column < 4; ++column) to[row][column] = from[row * 4 + column];
}

NVSDK_NGX_PerfQuality_Value Quality(SrMode mode) {
  switch (mode) {
    case SrMode::kDlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case SrMode::kUltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case SrMode::kQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case SrMode::kBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case SrMode::kPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case SrMode::kUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
  }
  return NVSDK_NGX_PerfQuality_Value_DLAA;
}

Handle* NewHandle(VkDevice device, std::string& error) {
  auto it = Devices().find(device);
  if (it == Devices().end() || !it->second.initialized) {
    error = "NGX not initialised on the device";
    return nullptr;
  }
  auto* handle = new Handle();
  handle->device = device;
  const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&handle->parameters);
  if (NVSDK_NGX_FAILED(result) || !handle->parameters) {
    error = "parameters: " + ResultText(uint32_t(result));
    delete handle;
    return nullptr;
  }
  return handle;
}

bool Live(Handle* handle) {
  if (!handle) return false;
  auto it = Devices().find(handle->device);
  return it != Devices().end() && it->second.handles.count(handle);
}
}  // namespace
#else
struct Handle {};
#endif

bool InstanceRequirements(std::vector<const char*>& extensions, std::string& reason) {
  extensions.clear();
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  bool any = false;
  for (const Feature feature : {Feature::kSuperResolution, Feature::kFrameGeneration}) {
    std::string why;
    if (!RuntimePresent(feature, &why)) {
      if (reason.empty()) reason = why;
      continue;
    }
    const NVSDK_NGX_FeatureDiscoveryInfo info = Discovery(feature);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const NVSDK_NGX_Result result =
        NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties);
    if (NVSDK_NGX_FAILED(result)) {
      reason = fmt::format("{} instance requirements: {}", FeatureName(feature), ResultText(uint32_t(result)));
      continue;
    }
    for (uint32_t i = 0; i < count && properties; ++i) {
      const char* name = Keep(properties[i].extensionName);
      if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return !std::strcmp(e, name); }))
        extensions.push_back(name);
    }
    any = true;
  }
  if (any) reason.clear();
  return any;
#else
  reason = kUnavailable;
  return false;
#endif
}

bool DeviceRequirements(const VulkanInstance& instance, VkPhysicalDevice physical_device, Feature feature,
                        std::vector<const char*>& extensions, DeviceFeatures& features, std::string& reason) {
  extensions.clear();
  features = DeviceFeatures();
#if REX_NGX_RUNTIME
  if (!RuntimePresent(feature, &reason)) return false;
  const auto& ifn = instance.functions();
  VkPhysicalDeviceProperties properties{};
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
  if (properties.vendorID != 0x10DE) {
    reason = "not an NVIDIA GPU";
    return false;
  }
  std::lock_guard lock(Lock());
  const NVSDK_NGX_FeatureDiscoveryInfo info = Discovery(feature);
  NVSDK_NGX_FeatureRequirement requirement{};
  NVSDK_NGX_Result result =
      NVSDK_NGX_VULKAN_GetFeatureRequirements(instance.instance(), physical_device, &info, &requirement);
  if (NVSDK_NGX_FAILED(result)) {
    reason = "requirements: " + ResultText(uint32_t(result));
    return false;
  }
  if (requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    reason = SupportText(uint32_t(requirement.FeatureSupported), requirement);
    return false;
  }
  uint32_t count = 0;
  VkExtensionProperties* required = nullptr;
  result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance.instance(), physical_device, &info,
                                                                  &count, &required);
  if (NVSDK_NGX_FAILED(result)) {
    reason = "device extensions: " + ResultText(uint32_t(result));
    return false;
  }
  uint32_t available_count = 0;
  ifn.vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &available_count, nullptr);
  std::vector<VkExtensionProperties> available(available_count);
  ifn.vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &available_count, available.data());
  available.resize(available_count);
  bool wants_address = false, wants_timeline = false, wants_sync2 = false;
  for (uint32_t i = 0; i < count && required; ++i) {
    const char* name = required[i].extensionName;
    if (std::none_of(available.begin(), available.end(),
                     [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); })) {
      reason = fmt::format("device extension {} unavailable", name);
      extensions.clear();
      return false;
    }
    extensions.push_back(Keep(name));
    wants_address |= std::strstr(name, "buffer_device_address") != nullptr;
    wants_timeline |= std::strstr(name, "timeline_semaphore") != nullptr;
    wants_sync2 |= std::strstr(name, "synchronization2") != nullptr;
  }
  // The features behind those extensions, where the GPU has them.
  if ((wants_address || wants_timeline || wants_sync2) && ifn.vkGetPhysicalDeviceFeatures2) {
    DeviceFeatures supported;
    supported.head.pNext = &supported.buffer_device_address;
    supported.buffer_device_address.pNext = &supported.timeline_semaphore;
    supported.timeline_semaphore.pNext = properties.apiVersion >= VK_API_VERSION_1_3 ? &supported.synchronization2
                                                                                     : nullptr;
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported.head);
    void** next = &features.head.pNext;
    if (wants_address && supported.buffer_device_address.bufferDeviceAddress) {
      features.buffer_device_address.bufferDeviceAddress = VK_TRUE;
      *next = &features.buffer_device_address;
      next = &features.buffer_device_address.pNext;
    }
    if (wants_timeline && supported.timeline_semaphore.timelineSemaphore) {
      features.timeline_semaphore.timelineSemaphore = VK_TRUE;
      *next = &features.timeline_semaphore;
      next = &features.timeline_semaphore.pNext;
    }
    if (wants_sync2 && supported.synchronization2.synchronization2) {
      features.synchronization2.synchronization2 = VK_TRUE;
      *next = &features.synchronization2;
      next = &features.synchronization2.pNext;
    }
    *next = nullptr;
  }
  return true;
#else
  (void)instance; (void)physical_device; (void)feature;
  reason = kUnavailable;
  return false;
#endif
}

bool Initialize(const VulkanDevice& device, std::string& reason) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  DeviceState& state = Devices()[device.device()];
  if (state.initialized) return true;
  if (!state.error.empty()) {
    reason = state.error;
    return false;
  }
  const VulkanInstance* instance = device.vulkan_instance();
  const auto gipa = instance->functions().vkGetInstanceProcAddr;
  const auto gdpa =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance->instance(), "vkGetDeviceProcAddr"));
  NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      ProjectId().c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, DataPath(), instance->instance(),
      device.physical_device(), device.device(), gipa, gdpa, CommonInfo(), NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(result)) {
    state.error = "NGX initialisation: " + ResultText(uint32_t(result));
    reason = state.error;
    return false;
  }
  result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities);
  if (NVSDK_NGX_FAILED(result) || !state.capabilities) {
    NVSDK_NGX_VULKAN_Shutdown1(device.device());
    state.capabilities = nullptr;
    state.error = "NGX capabilities: " + ResultText(uint32_t(result));
    reason = state.error;
    return false;
  }
  state.initialized = true;
  REXLOG_INFO("NGX: initialised on '{}' (project {}, logs in logs/ngx)", device.properties().deviceName,
              ProjectId());
  return true;
#else
  (void)device;
  reason = kUnavailable;
  return false;
#endif
}

void Shutdown(VkDevice device) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  auto it = Devices().find(device);
  if (it == Devices().end()) return;
  DeviceState& state = it->second;
  for (Handle* handle : state.handles) {
    if (handle->handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle->handle);
    if (handle->parameters) NVSDK_NGX_VULKAN_DestroyParameters(handle->parameters);
    delete handle;
  }
  state.handles.clear();
  if (state.capabilities) NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
  if (state.initialized) NVSDK_NGX_VULKAN_Shutdown1(device);
  Devices().erase(it);
#else
  (void)device;
#endif
}

bool Available(const VulkanDevice& device, Feature feature, std::string& reason) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  auto it = Devices().find(device.device());
  if (it == Devices().end() || !it->second.initialized || !it->second.capabilities) {
    reason = "NGX not initialised on the device";
    return false;
  }
  NVSDK_NGX_Parameter* caps = it->second.capabilities;
  const bool fg = feature == Feature::kFrameGeneration;
  int available = 0, needs_driver = 0;
  unsigned int major = 0, minor = 0;
  int init_result = 0;
  NVSDK_NGX_Parameter_GetI(caps, fg ? NVSDK_NGX_Parameter_FrameGeneration_Available
                                    : NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (available) return true;
  NVSDK_NGX_Parameter_GetI(caps, fg ? NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver
                                    : NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  if (needs_driver) {
    NVSDK_NGX_Parameter_GetUI(caps, fg ? NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor
                                       : NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(caps, fg ? NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor
                                       : NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    reason = fmt::format("needs GPU driver {}.{} or newer", major, minor);
    return false;
  }
  NVSDK_NGX_Parameter_GetI(caps, fg ? NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult
                                    : NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
  reason = init_result ? "unavailable: " + ResultText(uint32_t(init_result)) : std::string("unavailable on this GPU");
  return false;
#else
  (void)device; (void)feature;
  reason = kUnavailable;
  return false;
#endif
}

void Release(Handle* handle) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  if (!Live(handle)) return;  // Released with its device (Shutdown).
  if (handle->handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle->handle);
  if (handle->parameters) NVSDK_NGX_VULKAN_DestroyParameters(handle->parameters);
  Devices()[handle->device].handles.erase(handle);
  delete handle;
#else
  (void)handle;
#endif
}

bool SrInputRange(const VulkanDevice& device, SrMode mode, uint32_t output_width, uint32_t output_height,
                  SrRange& range) {
  range = {};
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  auto it = Devices().find(device.device());
  if (it == Devices().end() || !it->second.capabilities) return false;
  NVSDK_NGX_Parameter* caps = it->second.capabilities;
  void* callback = nullptr;
  NVSDK_NGX_Parameter_GetVoidPointer(caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
  if (!callback) return false;
  NVSDK_NGX_Parameter_SetUI(caps, NVSDK_NGX_Parameter_Width, output_width);
  NVSDK_NGX_Parameter_SetUI(caps, NVSDK_NGX_Parameter_Height, output_height);
  NVSDK_NGX_Parameter_SetI(caps, NVSDK_NGX_Parameter_PerfQualityValue, Quality(mode));
  NVSDK_NGX_Parameter_SetI(caps, NVSDK_NGX_Parameter_RTXValue, 0);  // Older DLLs still read it.
  if (NVSDK_NGX_FAILED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(caps))) return false;
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_OutWidth, &range.optimal_width);
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_OutHeight, &range.optimal_height);
  range.min_width = range.max_width = range.optimal_width;
  range.min_height = range.max_height = range.optimal_height;
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &range.max_width);
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &range.max_height);
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &range.min_width);
  NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &range.min_height);
  return range.optimal_width && range.optimal_height;
#else
  (void)device; (void)mode; (void)output_width; (void)output_height;
  return false;
#endif
}

Handle* SrCreate(const VulkanDevice& device, VkCommandBuffer command_buffer, const SrCreateInfo& info,
                 std::string& error) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  Handle* handle = NewHandle(device.device(), error);
  if (!handle) return nullptr;
  NVSDK_NGX_Parameter* p = handle->parameters;
  for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
    NVSDK_NGX_Parameter_SetUI(p, name, info.preset);
  NVSDK_NGX_DLSS_Create_Params create{};
  create.Feature.InWidth = info.render_width;
  create.Feature.InHeight = info.render_height;
  create.Feature.InTargetWidth = info.output_width;
  create.Feature.InTargetHeight = info.output_height;
  create.Feature.InPerfQualityValue = Quality(info.mode);
  // Motion at the input resolution, without jitter.
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                (info.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
                                (info.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                                (info.auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
  create.InEnableOutputSubrects = false;
  const NVSDK_NGX_Result result =
      NGX_VULKAN_CREATE_DLSS_EXT1(device.device(), command_buffer, 1, 1, &handle->handle, p, &create);
  if (NVSDK_NGX_FAILED(result) || !handle->handle) {
    error = "feature creation: " + ResultText(uint32_t(result));
    NVSDK_NGX_VULKAN_DestroyParameters(p);
    delete handle;
    return nullptr;
  }
  Devices()[device.device()].handles.insert(handle);
  return handle;
#else
  (void)device; (void)command_buffer; (void)info;
  error = kUnavailable;
  return nullptr;
#endif
}

bool SrEvaluate(VkCommandBuffer command_buffer, Handle* handle, const SrEvaluateInfo& info, std::string& error) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  if (!Live(handle)) {
    error = "feature released";
    return false;
  }
  NVSDK_NGX_Resource_VK color = Resource(info.color), depth = Resource(info.depth),
                        motion = Resource(info.motion), output = Resource(info.output);
  NVSDK_NGX_VK_DLSS_Eval_Params eval{};
  eval.Feature.pInColor = &color;
  eval.Feature.pInOutput = &output;
  eval.pInDepth = &depth;
  eval.pInMotionVectors = &motion;
  eval.InJitterOffsetX = info.jitter_x;
  eval.InJitterOffsetY = info.jitter_y;
  eval.InRenderSubrectDimensions = {info.render_width, info.render_height};
  eval.InReset = info.reset ? 1 : 0;
  eval.InMVScaleX = info.motion_scale_x;
  eval.InMVScaleY = info.motion_scale_y;
  eval.InFrameTimeDeltaInMsec = info.delta_ms;
  const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, handle->handle, handle->parameters, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    error = "evaluate: " + ResultText(uint32_t(result));
    return false;
  }
  return true;
#else
  (void)command_buffer; (void)handle; (void)info;
  error = kUnavailable;
  return false;
#endif
}

Handle* FgCreate(const VulkanDevice& device, VkCommandBuffer command_buffer, const FgCreateInfo& info,
                 std::string& error) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  Handle* handle = NewHandle(device.device(), error);
  if (!handle) return nullptr;
  NVSDK_NGX_Parameter* p = handle->parameters;
  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = info.width;
  create.Height = info.height;
  create.NativeBackbufferFormat = uint32_t(info.backbuffer_format);
  create.RenderWidth = info.render_width;
  create.RenderHeight = info.render_height;
  create.DynamicResolutionScaling = false;
  NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Width, info.width);
  NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Height, info.height);
  NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalWidth, info.render_width);
  NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalHeight, info.render_height);
  NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0);
  const NVSDK_NGX_Result result = NGX_VK_CREATE_DLSSG(command_buffer, 1, 1, &handle->handle, p, &create);
  if (NVSDK_NGX_FAILED(result) || !handle->handle) {
    error = "feature creation: " + ResultText(uint32_t(result));
    NVSDK_NGX_VULKAN_DestroyParameters(p);
    delete handle;
    return nullptr;
  }
  Devices()[device.device()].handles.insert(handle);
  return handle;
#else
  (void)device; (void)command_buffer; (void)info;
  error = kUnavailable;
  return nullptr;
#endif
}

bool FgEvaluate(VkCommandBuffer command_buffer, Handle* handle, const FgEvaluateInfo& info, std::string& error) {
#if REX_NGX_RUNTIME
  std::lock_guard lock(Lock());
  if (!Live(handle)) {
    error = "feature released";
    return false;
  }
  NVSDK_NGX_Resource_VK backbuffer = Resource(info.backbuffer), depth = Resource(info.depth),
                        motion = Resource(info.motion), hudless = Resource(info.hudless),
                        output = Resource(info.output);
  NVSDK_NGX_VK_DLSSG_Eval_Params eval{};
  eval.pBackbuffer = &backbuffer;
  eval.pDepth = &depth;
  eval.pMVecs = &motion;
  eval.pHudless = info.hudless.image ? &hudless : nullptr;
  eval.pOutputInterpFrame = &output;
  NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
  opt.multiFrameCount = 1;
  opt.multiFrameIndex = 1;
  CopyMatrix(info.view_to_clip, opt.cameraViewToClip);
  CopyMatrix(info.clip_to_view, opt.clipToCameraView);
  Matrix identity{};
  for (size_t i = 0; i < 4; ++i) identity[i * 5] = 1.f;
  CopyMatrix(identity, opt.clipToLensClip);
  CopyMatrix(info.clip_to_previous_clip, opt.clipToPrevClip);
  CopyMatrix(info.previous_clip_to_clip, opt.prevClipToClip);
  opt.jitterOffset[0] = info.jitter_x;
  opt.jitterOffset[1] = info.jitter_y;
  opt.mvecScale[0] = info.motion_scale_x;
  opt.mvecScale[1] = info.motion_scale_y;
  for (size_t i = 0; i < 3; ++i) {
    opt.cameraPos[i] = info.position[i];
    opt.cameraUp[i] = info.up[i];
    opt.cameraRight[i] = info.right[i];
    opt.cameraFwd[i] = info.forward[i];
  }
  opt.cameraNear = info.near_distance;
  opt.cameraFar = info.far_distance;
  opt.cameraFOV = info.fov_y;
  opt.cameraAspectRatio = info.aspect;
  opt.colorBuffersHDR = false;  // SDR swapchains (tonemapped backbuffer).
  opt.depthInverted = info.depth_inverted;
  opt.cameraMotionIncluded = true;
  opt.reset = info.reset;
  opt.motionVectorsInvalidValue = FLT_MAX;  // No invalid marker: every value is motion.
  opt.mvecsSubrectBase = {info.render_x, info.render_y};
  opt.mvecsSubrectSize = {info.render_width, info.render_height};
  opt.depthSubrectBase = {info.render_x, info.render_y};
  opt.depthSubrectSize = {info.render_width, info.render_height};
  const NVSDK_NGX_Coordinates base{uint32_t(std::max(info.rect_x, 0)), uint32_t(std::max(info.rect_y, 0))};
  const NVSDK_NGX_Dimensions size{info.rect_width, info.rect_height};
  opt.backbufferSubrectBase = base;
  opt.backbufferSubrectSize = size;
  opt.outputInterpSubrectBase = base;
  opt.outputInterpSubrectSize = size;
  if (info.hudless.image) {
    opt.hudLessSubrectBase = base;
    opt.hudLessSubrectSize = size;
  }
  NVSDK_NGX_Parameter_SetULL(handle->parameters, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, info.frame_id);
  NVSDK_NGX_Parameter_SetF(handle->parameters, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale,
                           info.depth_scale > 0 ? info.depth_scale : 1.f);
  const NVSDK_NGX_Result result = NGX_VK_EVALUATE_DLSSG(command_buffer, handle->handle, handle->parameters, &eval, &opt);
  if (NVSDK_NGX_FAILED(result)) {
    error = "evaluate: " + ResultText(uint32_t(result));
    return false;
  }
  return true;
#else
  (void)command_buffer; (void)handle; (void)info;
  error = kUnavailable;
  return false;
#endif
}

}  // namespace rex::ui::vulkan::ngx
