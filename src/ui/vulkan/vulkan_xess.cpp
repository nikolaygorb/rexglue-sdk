#include <rex/ui/vulkan/xess.h>

#include <filesystem>

#include <fmt/format.h>

#include <rex/filesystem.h>

#if defined(REX_HAS_XESS_SDK) && REX_HAS_XESS_SDK && defined(_WIN32)
#define REX_XESS_RUNTIME 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xess/xess_vk.h>
#else
#define REX_XESS_RUNTIME 0
#endif

namespace rex::ui::vulkan {

#if REX_XESS_RUNTIME
namespace {
struct Library {
  HMODULE module = nullptr;
  std::string reason;
  decltype(&xessGetVersion) get_version = nullptr;
  decltype(&xessVKGetRequiredInstanceExtensions) instance_extensions = nullptr;
  decltype(&xessVKGetRequiredDeviceExtensions) device_extensions = nullptr;
  decltype(&xessVKGetRequiredDeviceFeatures) device_features = nullptr;
};

const Library& GetLibrary() {
  static const Library library = [] {
    Library l;
    // Next to the executable only (staged by rexglue_setup_target with the
    // native plugin), never from the DLL search path.
    const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "libxess.dll";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      l.reason = "libxess.dll not found next to the executable";
      return l;
    }
    l.module = LoadLibraryW(path.c_str());
    if (!l.module) {
      l.reason = fmt::format(
          "libxess.dll failed to load (error {}; it needs the Microsoft Visual C++ 2015-2022 "
          "redistributable)",
          GetLastError());
      return l;
    }
    l.get_version = reinterpret_cast<decltype(l.get_version)>(GetProcAddress(l.module, "xessGetVersion"));
    l.instance_extensions = reinterpret_cast<decltype(l.instance_extensions)>(
        GetProcAddress(l.module, "xessVKGetRequiredInstanceExtensions"));
    l.device_extensions = reinterpret_cast<decltype(l.device_extensions)>(
        GetProcAddress(l.module, "xessVKGetRequiredDeviceExtensions"));
    l.device_features = reinterpret_cast<decltype(l.device_features)>(
        GetProcAddress(l.module, "xessVKGetRequiredDeviceFeatures"));
    if (!l.get_version || !l.instance_extensions || !l.device_extensions || !l.device_features) {
      l.reason = "XeSS Vulkan entry points missing in libxess.dll";
      FreeLibrary(l.module);
      l.module = nullptr;
    }
    return l;
  }();
  return library;
}
}  // namespace
#endif

void* XessModule(std::string* reason) {
#if REX_XESS_RUNTIME
  const Library& library = GetLibrary();
  if (!library.module && reason) *reason = library.reason;
  return library.module;
#else
  if (reason) *reason = "Intel XeSS not available in this build/platform (Windows with REXGLUE_ENABLE_XESS_SDK)";
  return nullptr;
#endif
}

std::string XessVersion() {
#if REX_XESS_RUNTIME
  const Library& library = GetLibrary();
  xess_version_t version{};
  if (library.module && library.get_version(&version) == XESS_RESULT_SUCCESS)
    return fmt::format("{}.{}.{}", version.major, version.minor, version.patch);
#endif
  return {};
}

bool XessInstanceRequirements(std::vector<const char*>& extensions, uint32_t& api_version,
                              std::string& reason) {
  extensions.clear();
  api_version = 0;
#if REX_XESS_RUNTIME
  const Library& library = GetLibrary();
  if (!library.module) { reason = library.reason; return false; }
  uint32_t count = 0;
  const char* const* names = nullptr;
  const xess_result_t result = library.instance_extensions(&count, &names, &api_version);
  if (result != XESS_RESULT_SUCCESS) { reason = "instance requirements: " + XessResultText(result); return false; }
  extensions.assign(names, names + (names ? count : 0));
  return true;
#else
  reason = "Intel XeSS not available in this build/platform (Windows with REXGLUE_ENABLE_XESS_SDK)";
  return false;
#endif
}

bool XessDeviceRequirements(VkInstance instance, VkPhysicalDevice physical_device,
                            std::vector<const char*>& extensions, const void*& features,
                            std::string& reason) {
  extensions.clear();
  features = nullptr;
#if REX_XESS_RUNTIME
  const Library& library = GetLibrary();
  if (!library.module) { reason = library.reason; return false; }
  uint32_t count = 0;
  const char* const* names = nullptr;
  xess_result_t result = library.device_extensions(instance, physical_device, &count, &names);
  if (result != XESS_RESULT_SUCCESS) { reason = "device extensions: " + XessResultText(result); return false; }
  // Null: a new chain of XeSS's own (VkPhysicalDeviceFeatures2 first), merged
  // into the device's chain by the caller.
  void* chain = nullptr;
  result = library.device_features(instance, physical_device, &chain);
  if (result != XESS_RESULT_SUCCESS) { reason = "device features: " + XessResultText(result); return false; }
  extensions.assign(names, names + (names ? count : 0));
  features = chain;
  return true;
#else
  (void)instance; (void)physical_device;
  reason = "Intel XeSS not available in this build/platform (Windows with REXGLUE_ENABLE_XESS_SDK)";
  return false;
#endif
}

std::string XessResultText(int result) {
  const char* name = "error";
  switch (result) {
    case 2: name = "old driver"; break;
    case 1: name = "dump folder missing"; break;
    case 0: name = "success"; break;
    case -1: name = "unsupported device"; break;
    case -2: name = "unsupported driver"; break;
    case -3: name = "uninitialized"; break;
    case -4: name = "invalid argument"; break;
    case -5: name = "out of GPU memory"; break;
    case -6: name = "device error"; break;
    case -7: name = "not implemented"; break;
    case -8: name = "invalid context"; break;
    case -9: name = "operation in progress"; break;
    case -10: name = "unsupported"; break;
    case -11: name = "can't load library"; break;
    case -12: name = "wrong call order"; break;
    default: break;
  }
  return fmt::format("{} ({})", name, result);
}

}  // namespace rex::ui::vulkan
