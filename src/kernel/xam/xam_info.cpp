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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/title_id_utils.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include <fmt/format.h>
#include <fmt/xchar.h>

// Defined in xboxkrnl_hal.cpp.
REXCVAR_DECLARE(int32_t, avpack);

// xboxkrnl exports the XAPI functions below are built on.
namespace rex::kernel::xboxkrnl {
u32 ExCreateThread_entry(mapped_u32 handle_ptr, u32 stack_size, mapped_u32 thread_id_ptr,
                         u32 xapi_thread_startup, mapped_void start_address,
                         mapped_void start_context, u32 creation_flags);
u32 ExTerminateThread_entry(u32 exit_code);
u32 NtResumeThread_entry(u32 handle, mapped_u32 suspend_count_ptr);
u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, mapped_u64 interval_ptr);
u32 NtWaitForSingleObjectEx_entry(u32 object_handle, u32 wait_mode, u32 alertable,
                                  mapped_u64 timeout_ptr);
u32 XexGetModuleHandle_entry(mapped_string module_name, mapped_u32 hmodule_ptr);
}  // namespace rex::kernel::xboxkrnl

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

u32 XamFeatureEnabled_entry(u64 feature_bit) {
  // Canary tests the bit in std::bitset<36>(0x40FFFFFFFF), which keeps bits 0-31
  // only (and throws past bit 35).
  return feature_bit < 32 ? 1 : 0;
}

// Empty stub schema binary.
uint8_t schema_bin[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
};

u32 XamGetOnlineSchema_entry() {
  static uint32_t schema_guest = 0;

  if (!schema_guest) {
    schema_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(8 + sizeof(schema_bin));
    auto schema = REX_KERNEL_MEMORY()->TranslateVirtual(schema_guest);
    std::memcpy(schema + 8, schema_bin, sizeof(schema_bin));
    memory::store_and_swap<uint32_t>(schema + 0, schema_guest + 8);
    memory::store_and_swap<uint32_t>(schema + 4, sizeof(schema_bin));
  }

  // return pointer to the schema ptr/schema size struct
  return schema_guest;
}

namespace {

// A kernel version packed as 4.4.16.8 bits.
constexpr uint32_t PackSystemVersion(uint16_t major, uint16_t minor, uint16_t build, uint8_t qfe) {
  return uint32_t(major) << 28 | (uint32_t(minor) & 0xF) << 24 | uint32_t(build) << 8 | qfe;
}
// 2.0.2411.0, a value a retail console reported.
static_assert(PackSystemVersion(2, 0, 2411, 0) == 0x20096B00);

// The version XboxKrnlVersion exports.
constexpr uint32_t kXamCurrentSystemVersion = PackSystemVersion(
    kKernelVersionMajor, kKernelVersionMinor, kKernelVersionBuild, kKernelVersionQfe);
constexpr uint32_t kXamBaseSystemVersion =
    PackSystemVersion(kKernelVersionMajor, 0, kBaseKernelBuildVersion, 0);

// FILETIMEs between 2000 and 2100; anything else is not a time, which tells
// the two signatures of the format functions apart.
bool IsValidFileTime(uint64_t filetime) {
  return filetime >= 125911584000000000ull && filetime <= 157469184000000000ull;
}

std::chrono::system_clock::time_point FileTimeToSys(uint64_t filetime) {
  return rex::chrono::WinSystemClock::to_sys(
      rex::chrono::WinSystemClock::from_file_time(filetime));
}

// buffer_size is in bytes.
void XFormatDateString(uint64_t filetime, uint32_t buffer_address, uint32_t buffer_size) {
  auto buffer = REX_KERNEL_MEMORY()->TranslateVirtual<char16_t*>(buffer_address);
  const auto days = std::chrono::floor<std::chrono::days>(FileTimeToSys(filetime));
  const std::chrono::year_month_day date{days};
  auto str = fmt::format(u"{:02d}/{:02d}/{}", static_cast<unsigned>(date.month()),
                         static_cast<unsigned>(date.day()), static_cast<int>(date.year()));
  rex::string::copy_and_swap_truncating(buffer, str, buffer_size / sizeof(char16_t));
}

void XFormatTimeString(uint64_t filetime, uint32_t buffer_address, uint32_t buffer_size) {
  auto buffer = REX_KERNEL_MEMORY()->TranslateVirtual<char16_t*>(buffer_address);
  const auto tp = FileTimeToSys(filetime);
  const auto days = std::chrono::floor<std::chrono::days>(tp);
  const std::chrono::hh_mm_ss time{std::chrono::floor<std::chrono::milliseconds>(tp - days)};
  auto str = fmt::format(u"{:02d}:{:02d}", time.hours().count(), time.minutes().count());
  rex::string::copy_and_swap_truncating(buffer, str, buffer_size / sizeof(char16_t));
}

}  // namespace

// Newer dashboards drop the first parameter and shift the others, so a
// filetime that is not a time means the shorter signature.
void XamFormatDateString_entry(u32 locale_format, u64 filetime, mapped_void output_buffer,
                               u32 output_size) {
  if (IsValidFileTime(filetime)) {
    XFormatDateString(filetime, output_buffer.guest_address(), output_size);
  } else {
    auto* ctx = rex::runtime::current_ppc_context();
    XFormatDateString(ctx->r3.u64, ctx->r4.u32, ctx->r5.u32);
  }
}

void XamFormatTimeString_entry(u32 user_index, u64 filetime, mapped_void output_buffer,
                               u32 output_size) {
  if (IsValidFileTime(filetime)) {
    XFormatTimeString(filetime, output_buffer.guest_address(), output_size);
  } else {
    auto* ctx = rex::runtime::current_ppc_context();
    XFormatTimeString(ctx->r3.u64, ctx->r4.u32, ctx->r5.u32);
  }
}

u32 keXamBuildResourceLocator(uint64_t module, std::u16string_view container,
                              std::u16string_view resource, mapped_void buffer_ptr,
                              uint32_t buffer_count) {
  std::u16string path;
  if (!module) {
    path = fmt::format(u"file://media:/{}.xzp#{}", container, resource);
    REXKRNL_DEBUG("XamBuildResourceLocator({0}) returning locator to local file {0}.xzp",
                  rex::string::to_utf8(container));
  } else {
    path = fmt::format(u"section://{:X},{}#{}", (uint32_t)module, container, resource);
  }
  // buffer_count is in bytes.
  rex::string::copy_and_swap_truncating(buffer_ptr.as<char16_t*>(), path,
                                        buffer_count / sizeof(char16_t));
  return 0;
}

u32 XamBuildResourceLocator_entry(u64 module, mapped_wstring container, mapped_wstring resource,
                                  mapped_void buffer_ptr, u32 buffer_count) {
  return keXamBuildResourceLocator(module, container.value(), resource.value(), buffer_ptr,
                                   buffer_count);
}

u32 XamBuildGamercardResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                           u32 buffer_count) {
  // On an actual xbox these funcs would return a locator to xam.xex resources,
  // but for Xenia we can return a locator to the resources as local files. (big
  // thanks to MS for letting XamBuildResourceLocator return local file
  // locators!)

  // If you're running an app that'll need them, make sure to extract xam.xex
  // resources with xextool ("xextool -d . xam.xex") and add a .xzp extension.

  return keXamBuildResourceLocator(0, u"gamercrd", filename.value(), buffer_ptr, buffer_count);
}

u32 XamBuildSharedSystemResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                              u32 buffer_count) {
  // see notes inside XamBuildGamercardResourceLocator above
  return keXamBuildResourceLocator(0, u"shrdres", filename.value(), buffer_ptr, buffer_count);
}

u32 XamBuildLegacySystemResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                              u32 buffer_count) {
  return XamBuildSharedSystemResourceLocator_entry(filename, buffer_ptr, buffer_count);
}

u32 XamBuildXamResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                     u32 buffer_count) {
  return keXamBuildResourceLocator(0, u"xam", filename.value(), buffer_ptr, buffer_count);
}

u32 XamGetSystemVersion_entry() {
  return kXamCurrentSystemVersion;
}

void XCustomRegisterDynamicActions_entry() {
  // ???
}

u32 XGetAVPack_entry() {
  // DWORD
  // Not sure what the values are for this, but 6 is VGA.
  // Other likely values are 3/4/8 for HDMI or something.
  // Games seem to use this as a PAL check - if the result is not 3/4/6/8
  // they explode with errors if not in PAL mode.
  REXKRNL_IMPORT_RESULT("XGetAVPack", "6");
  return 6;
}

uint32_t xeXGetGameRegion() {
  return 0xFFFFu;
}

u32 XGetGameRegion_entry() {
  return xeXGetGameRegion();
}

// Defined in xam_locale.cpp.
XLanguage xeGetLanguage(bool extended_languages_support);

u32 XGetLanguage_entry() {
  return uint32_t(xeGetLanguage(false));
}

u32 XamGetCurrentTitleId_entry() {
  // NOTE(tomc): Switched this up to get title ID from executable module instead of runtime
  // (emulator)
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  if (module) {
    return module->title_id();
  }
  return 0;
}

u32 XamGetExecutionId_entry(mapped_u32 info_ptr) {
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  assert_not_null(module);

  uint32_t guest_hdr_ptr;
  X_STATUS result = module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &guest_hdr_ptr);

  if (XFAILED(result)) {
    return result;
  }

  *info_ptr = guest_hdr_ptr;
  return X_STATUS_SUCCESS;
}

constexpr uint32_t MAX_LAUNCH_DATA_SIZE = 0x3FC;

u32 XamLoaderSetLaunchData_entry(mapped_void data, u32 size) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  loader_data.launch_data_present = size ? true : false;
  const uint32_t copy_size = std::min(size, MAX_LAUNCH_DATA_SIZE);
  loader_data.launch_data.resize(copy_size);
  std::memcpy(loader_data.launch_data.data(), data, copy_size);
  return 0;
}

u32 XamLoaderGetLaunchDataSize_entry(mapped_u32 size_ptr) {
  if (!size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (loader_data.launch_data.empty()) {
    *size_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  *size_ptr = uint32_t(xam->loader_data().launch_data.size());
  return X_ERROR_SUCCESS;
}

u32 XamLoaderGetLaunchData_entry(mapped_void buffer_ptr, u32 buffer_size) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (!buffer_size) {
    return X_ERROR_NOT_FOUND;
  }

  uint32_t copy_size = std::min(uint32_t(loader_data.launch_data.size()), uint32_t(buffer_size));
  std::memcpy(buffer_ptr, loader_data.launch_data.data(), copy_size);
  return X_ERROR_SUCCESS;
}

void XamLoaderLaunchTitle_entry(mapped_string raw_name_ptr, u32 flags) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  auto& loader_data = xam->loader_data();
  loader_data.launch_flags = flags;

  // Translate the launch path to a full path.
  if (raw_name_ptr) {
    // A copy: path takes the joined string below, which a string_view would
    // only point at until the end of that statement.
    std::string path(raw_name_ptr.value());
    if (path.empty()) {
      loader_data.launch_path = "game:\\default.xex";
    } else {
      if (rex::string::utf8_find_name_from_guest_path(path) == path) {
        path = rex::string::utf8_join_guest_paths(
            rex::string::utf8_find_base_guest_path(
                REX_KERNEL_STATE()->GetExecutableModule()->path()),
            path);
      }
      loader_data.launch_path = path;
    }
  } else {
    assert_always("Game requested exit to dashboard via XamLoaderLaunchTitle");
  }

  // This function does not return.
  REX_KERNEL_STATE()->TerminateTitle();
}

void XamLoaderTerminateTitle_entry() {
  // This function does not return.
  REX_KERNEL_STATE()->TerminateTitle();
}

// Navigates to the URI pushed with XamPushBackURI, or with none reboots to the
// dashboard. XamPushBackURI is not implemented, so this always exits.
void XamNavigateBack_entry() {
  // This function does not return.
  REX_KERNEL_STATE()->TerminateTitle();
}

u32 XamAlloc_entry(u32 unk, u32 size, mapped_u32 out_ptr) {
  assert_true(unk == 0);

  // Allocate from the heap. Not sure why XAM does this specially, perhaps
  // it keeps stuff in a separate heap?
  uint32_t ptr = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  *out_ptr = ptr;

  return X_ERROR_SUCCESS;
}

u32 XamFree_entry(mapped_u32 ptr) {
  REX_KERNEL_MEMORY()->SystemHeapFree(ptr.guest_address());

  return X_ERROR_SUCCESS;
}

u32 XamQueryLiveHiveW_entry(mapped_wstring name, mapped_void out_buf, u32 out_size,
                            u32 type /* guess */) {
  return X_STATUS_INVALID_PARAMETER_1;
}

u32 XamLoaderGetDvdTrayState_entry(mapped_u32 out_state) {
  // 0 = tray open, 1 = tray closed with disc
  if (out_state)
    *out_state = 1;
  return X_STATUS_SUCCESS;
}

u32 XamSwapDisc_entry(u32 disc_number) {
  // Stub for multi-disc games. Single-disc games (like Blue Dragon's reblue test)
  // don't need this, but the game may look it up dynamically via XexGetProcedureAddress.
  REXKRNL_DEBUG("XamSwapDisc({}) - stub, returning success", (uint32_t)disc_number);
  return X_STATUS_SUCCESS;
}

//=============================================================================
// XAPI exports and XAM info, ported from xenia-canary (xam_info.cc) for the
// ReXGlue fork
//=============================================================================

namespace {

constexpr X_HRESULT kXamPending = X_HRESULT_FROM_WIN32(X_ERROR_IO_PENDING);
constexpr X_HRESULT kXamNotImplemented = static_cast<X_HRESULT>(0x80004001L);
constexpr X_HRESULT kXamOnlineNotLoggedOn = static_cast<X_HRESULT>(0x80151802L);
constexpr uint32_t kWaitFailed = 0xFFFFFFFF;

std::atomic<uint32_t> dash_context{0};

bool IsNtError(uint32_t status) {
  return (status >> 30) == 3;
}

// Per-process XAM random table, as XAM seeds it.
int32_t xam_rtl_random_table[128] = {
    1284227242, 1275210071, 573735546,  790525478,  2139871995, 1547161642, 179605362,  789336058,
    688789844,  1801674531, 1563985344, 1957994488, 1364589140, 1645522239, 287218729,  606747145,
    1972579041, 1085031214, 1425521274, 1482476501, 1844823847, 57989841,   1897051121, 1935655697,
    1078145449, 1960408961, 1682526488, 842925246,  1500820517, 1214440339, 1647877149, 682003330,
    261478967,  2052111302, 162531612,  583907252,  1336601894, 1715567821, 413027322,  484763869,
    1383384898, 1004336348, 764733703,  854245398,  651377827,  1614895754, 838170752,  1757981266,
    602609370,  1644491937, 926492609,  220523388,  115176313,  725345543,  261903793,  746137067,
    920026266,  1123561945, 1580818891, 1708537768, 616249376,  1292428093, 562591055,  343818398,
    1788223648, 1659004503, 2077806209, 299502267,  1604221776, 602162358,  630328440,  1606980848,
    1580436667, 1078081533, 492894223,  839522115,  1979792683, 117609710,  1767777339, 1454471165,
    1965331169, 1844237615, 308236825,  329068152,  412668190,  796845957,  1303643608, 436374069,
    1677128483, 527237240,  813497703,  1060284298, 1770027372, 1177238915, 884357618,  1409082233,
    1958367476, 448539723,  1592454029, 861567501,  963894560,  73586283,   362288127,  507921405,
    113007714,  823518204,  152049171,  1202660629, 1326574676, 2025429265, 1035525444, 515967899,
    1532298954, 2000478354, 1450960922, 1417001333, 2049760794, 1229272821, 879983540,  1993962763,
    706699826,  776561741,  2111687655, 1343024091, 1637723038, 1220945662, 484061587,  1390067357};

}  // namespace

u32 RtlGetLastError_entry() {
  return XThread::GetLastError();
}

u32 GetLastError_entry() {
  return RtlGetLastError_entry();
}

u32 RtlSetLastNTError_entry(u32 error_code) {
  const uint32_t result = xboxkrnl::xeRtlNtStatusToDosError(error_code);
  XThread::SetLastError(result);
  return result;
}

u32 GetCurrentThreadId_entry() {
  return XThread::GetCurrentThreadId();
}

u32 GetTickCount_entry() {
  return chrono::Clock::QueryGuestUptimeMillis();
}

void GetSystemTimeAsFileTime_entry(mapped_u64 time_ptr) {
  if (time_ptr) {
    *time_ptr = chrono::Clock::QueryGuestSystemTime();
  }
}

u32 QueryPerformanceFrequency_entry(mapped_u64 frequency_ptr) {
  if (frequency_ptr) {
    *frequency_ptr = static_cast<uint32_t>(chrono::Clock::guest_tick_frequency());
  }
  return 1;
}

u32 RtlSleep_entry(u32 milliseconds, u32 alertable) {
  // 100 ns units, negative for a relative delay.
  rex::be<uint64_t> delay = milliseconds == 0xFFFFFFFF
                                ? static_cast<uint64_t>(LLONG_MAX)
                                : static_cast<uint64_t>(-10000ll * int64_t(milliseconds));
  uint32_t result =
      xboxkrnl::KeDelayExecutionThread_entry(1, alertable, mapped_u64::from_host(&delay));
  // An APC interrupted the delay: keep delaying.
  while (alertable && result == X_STATUS_ALERTED) {
    result = xboxkrnl::KeDelayExecutionThread_entry(1, alertable, mapped_u64::from_host(&delay));
  }
  return result == X_STATUS_SUCCESS ? X_STATUS_SUCCESS : X_STATUS_USER_APC;
}

void Sleep_entry(u32 milliseconds) {
  RtlSleep_entry(milliseconds, 0);
}

u32 SleepEx_entry(u32 milliseconds, u32 alertable) {
  return RtlSleep_entry(milliseconds, alertable);
}

u32 XapiFormatTimeOut_entry(mapped_u64 result, u32 milliseconds) {
  if (milliseconds == 0xFFFFFFFF || !result) {
    return 0;
  }
  *result = static_cast<uint64_t>(-10000ll * int64_t(milliseconds));
  return result.guest_address();
}

u32 WaitForSingleObjectEx_entry(u32 handle, u32 milliseconds, u32 alertable) {
  rex::be<uint64_t> timeout = static_cast<uint64_t>(-10000ll * int64_t(milliseconds));
  auto timeout_ptr = milliseconds == 0xFFFFFFFF ? mapped_u64() : mapped_u64::from_host(&timeout);
  uint32_t result = xboxkrnl::NtWaitForSingleObjectEx_entry(handle, 1, alertable, timeout_ptr);
  while (alertable && result == X_STATUS_ALERTED) {
    result = xboxkrnl::NtWaitForSingleObjectEx_entry(handle, 1, alertable, timeout_ptr);
  }
  // Win32 semantics: a wait status (object, abandoned, APC, timeout) is
  // returned as is, an error sets the last error and returns WAIT_FAILED.
  if (IsNtError(result)) {
    RtlSetLastNTError_entry(result);
    return kWaitFailed;
  }
  return result;
}

u32 WaitForSingleObject_entry(u32 handle, u32 milliseconds) {
  return WaitForSingleObjectEx_entry(handle, milliseconds, 0);
}

u32 ResumeThread_entry(u32 thread_handle) {
  rex::be<uint32_t> suspend_count = 0;
  const uint32_t status =
      xboxkrnl::NtResumeThread_entry(thread_handle, mapped_u32::from_host(&suspend_count));
  if (IsNtError(status)) {
    RtlSetLastNTError_entry(status);
    return kWaitFailed;
  }
  return suspend_count;
}

void ExitThread_entry(u32 exit_code) {
  xboxkrnl::ExTerminateThread_entry(exit_code);
}

u32 XapipCreateThread_entry(mapped_u32 thread_attributes, u32 stack_size, mapped_void start_address,
                            mapped_void parameter, u32 creation_flags, u32 thread_processor,
                            mapped_u32 thread_id) {
  // CREATE_SUSPENDED (4) becomes ExCreateThread's suspended flag (1); a
  // processor goes in the affinity byte.
  uint32_t flags = (creation_flags >> 2) & 1;
  if (thread_processor != 0xFFFFFFFF) {
    flags |= (1u << thread_processor) << 24;
  }

  rex::be<uint32_t> handle = 0;
  const uint32_t status = xboxkrnl::ExCreateThread_entry(
      mapped_u32::from_host(&handle), stack_size, thread_id, 0, start_address, parameter, flags);
  if (IsNtError(status)) {
    RtlSetLastNTError_entry(status);
    return 0;
  }
  return handle;
}

u32 CreateThread_entry(mapped_u32 thread_attributes, u32 stack_size, mapped_void start_address,
                       mapped_void parameter, u32 creation_flags, mapped_u32 thread_id) {
  return XapipCreateThread_entry(thread_attributes, stack_size, start_address, parameter,
                                 creation_flags, 0xFFFFFFFF, thread_id);
}

u32 GetModuleHandleA_entry(mapped_string module_name) {
  rex::be<uint32_t> module_ptr = 0;
  const uint32_t status =
      xboxkrnl::XexGetModuleHandle_entry(module_name, mapped_u32::from_host(&module_ptr));
  if (status != X_ERROR_SUCCESS) {
    RtlSetLastNTError_entry(status);
    return 0;
  }
  return module_ptr;
}

u32 lstrlenW_entry(mapped_wstring string) {
  if (!string) {
    return 0;
  }
  const char16_t* chars = string;
  uint32_t length = 0;
  while (chars[length]) {
    length++;
  }
  return length;
}

u32 RtlRandom_entry(mapped_u32 seed_ptr) {
  // RtlUniform's generator, in 32-bit unsigned arithmetic like NT's.
  auto uniform = [](uint32_t seed) { return (0x7FFFFFEDu * seed + 0x7FFFFFC3u) % 0x7FFFFFFFu; };
  const uint32_t table_seed = uniform(*seed_ptr);
  const uint32_t param_seed = uniform(table_seed);
  *seed_ptr = param_seed;

  int32_t& slot = xam_rtl_random_table[param_seed & 0x7F];
  const int32_t result = slot;
  slot = static_cast<int32_t>(table_seed);
  return static_cast<uint32_t>(result);
}

u32 XGetOverlappedExtendedError_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!overlapped_ptr) {
    return XThread::GetLastError();
  }
  if (overlapped_ptr->result == X_ERROR_IO_PENDING) {
    return X_ERROR_IO_INCOMPLETE;
  }
  return overlapped_ptr->extended_error;
}

u32 XGetAudioFlags_entry() {
  // No XConfig store: an A/V pack without digital audio (2, PAL-60 SCART) gets
  // stereo, everything else canary's default when the setting is unset.
  if (REXCVAR_GET(avpack) == 2) {
    return 2;
  }
  return 0x10000 | 0x1;
}

u32 XamIsSystemTitleId_entry(u32 title_id) {
  return IsSystemTitle(title_id);
}

u32 XamIsSystemExperienceTitleId_entry(u32 title_id) {
  return IsSystemExperienceTitle(title_id);
}

u32 XamIsXbox1TitleId_entry(u32 title_id) {
  return IsOriginalXboxTitle(title_id);
}

u32 XamIsCurrentTitleDash_entry() {
  return REX_KERNEL_STATE()->title_id() == kDashboardID;
}

u32 XamGetDashContext_entry() {
  return dash_context.load();
}

u32 XamSetDashContext_entry(u32 value) {
  // canary also broadcasts kXNotificationSystemDashContextChanged; only the
  // dashboard listens for it.
  dash_context.store(value);
  return 0;
}

u32 XamGetDashBackstackData_entry(mapped_void backstack_data, mapped_u32 count) {
  // There is no dashboard backstack.
  return X_ERROR_SUCCESS;
}

u32 XamIsChildAccountSignedIn_entry() {
  // Local profiles here are never child accounts.
  return 0;
}

u32 XamIsIptvEnabled_entry() {
  return X_E_FAIL;
}

u32 Refresh_entry() {
  return X_ERROR_SUCCESS;
}

u32 XamDoesOmniNeedConfiguration_entry() {
  return 0;
}

u32 XamFirstRunExperienceShouldRun_entry() {
  return 0;
}

u32 XamGetDvrStorage_entry(mapped_u32 dvr_storage, mapped_u32 used_dvr_storage,
                           mapped_u32 hdd_unused_space) {
  if (dvr_storage) {
    *dvr_storage = 0;
  }
  if (used_dvr_storage) {
    *used_dvr_storage = 0;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSetDvrStorage_entry(u32 dvr_storage_size, mapped_void overlapped_ptr) {
  return X_ERROR_SUCCESS;
}

u32 XamGetStagingMode_entry() {
  return 0;
}

u32 XamLogLocalizationEtx_entry(u32 error_code, u32 unk) {
  if (error_code == 0x80300034 || error_code == 0x80300035) {
    return X_ERROR_SUCCESS;
  }
  return kXamNotImplemented;
}

u32 XamLookupCommonStringByIndex_entry(u32 string_index) {
  return 0;
}

u32 XamUpdateGetBaseSystemVersion_entry() {
  return kXamBaseSystemVersion;
}

u32 XamUpdateGetCurrentSystemVersion_entry() {
  return kXamCurrentSystemVersion;
}

u32 XamXStudioRequest_entry(u32 unk1, mapped_u32 unk2) {
  return X_E_FAIL;
}

void XampWebInstrumentationSetProfileCounts_entry(u32 live_profiles, u32 local_profiles,
                                                  u32 adult_profiles, u32 child_profiles) {}

// Live hive values: not signed in to Live.
static u32 LiveHiveResult(bool args_valid, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr) {
  const X_RESULT result = args_valid ? kXamOnlineNotLoggedOn : X_E_INVALIDARG;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

u32 XamGetLiveHiveValueA_entry(mapped_string feature_name, mapped_string value_ptr,
                               u32 value_buffer_size, u32 unk,
                               ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr) {
  return LiveHiveResult(feature_name && value_ptr && value_buffer_size, overlapped_ptr);
}

u32 XamGetLiveHiveValueW_entry(mapped_wstring feature_name, mapped_wstring value_ptr,
                               u32 value_buffer_size, u32 unk,
                               ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr) {
  return LiveHiveResult(feature_name && value_ptr && value_buffer_size, overlapped_ptr);
}

u32 XamQueryLiveHiveA_entry(mapped_string feature_name, mapped_string value_ptr,
                            u32 value_buffer_size, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr) {
  const X_RESULT extended_error =
      feature_name && value_ptr && value_buffer_size ? kXamOnlineNotLoggedOn : X_E_INVALIDARG;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(),
                                                      X_STATUS_SUCCESS, extended_error, 0);
    return kXamPending;
  }
  return extended_error;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamFeatureEnabled, rex::kernel::xam::XamFeatureEnabled_entry)
REX_EXPORT(__imp__XamGetOnlineSchema, rex::kernel::xam::XamGetOnlineSchema_entry)
REX_EXPORT(__imp__XamFormatDateString, rex::kernel::xam::XamFormatDateString_entry)
REX_EXPORT(__imp__XamFormatTimeString, rex::kernel::xam::XamFormatTimeString_entry)
REX_EXPORT(__imp__XamBuildResourceLocator, rex::kernel::xam::XamBuildResourceLocator_entry)
REX_EXPORT(__imp__XamBuildGamercardResourceLocator,
           rex::kernel::xam::XamBuildGamercardResourceLocator_entry)
REX_EXPORT(__imp__XamBuildSharedSystemResourceLocator,
           rex::kernel::xam::XamBuildSharedSystemResourceLocator_entry)
REX_EXPORT(__imp__XamBuildLegacySystemResourceLocator,
           rex::kernel::xam::XamBuildLegacySystemResourceLocator_entry)
REX_EXPORT(__imp__XamBuildXamResourceLocator, rex::kernel::xam::XamBuildXamResourceLocator_entry)
REX_EXPORT(__imp__XamGetSystemVersion, rex::kernel::xam::XamGetSystemVersion_entry)
REX_EXPORT(__imp__XCustomRegisterDynamicActions,
           rex::kernel::xam::XCustomRegisterDynamicActions_entry)
REX_EXPORT(__imp__XGetAVPack, rex::kernel::xam::XGetAVPack_entry)
REX_EXPORT(__imp__XGetGameRegion, rex::kernel::xam::XGetGameRegion_entry)
REX_EXPORT(__imp__XGetLanguage, rex::kernel::xam::XGetLanguage_entry)
REX_EXPORT(__imp__XamGetCurrentTitleId, rex::kernel::xam::XamGetCurrentTitleId_entry)
REX_EXPORT(__imp__XamGetExecutionId, rex::kernel::xam::XamGetExecutionId_entry)
REX_EXPORT(__imp__XamLoaderSetLaunchData, rex::kernel::xam::XamLoaderSetLaunchData_entry)
REX_EXPORT(__imp__XamLoaderGetLaunchDataSize, rex::kernel::xam::XamLoaderGetLaunchDataSize_entry)
REX_EXPORT(__imp__XamLoaderGetLaunchData, rex::kernel::xam::XamLoaderGetLaunchData_entry)
REX_EXPORT(__imp__XamLoaderLaunchTitle, rex::kernel::xam::XamLoaderLaunchTitle_entry)
REX_EXPORT(__imp__XamLoaderTerminateTitle, rex::kernel::xam::XamLoaderTerminateTitle_entry)
REX_EXPORT(__imp__XamNavigateBack, rex::kernel::xam::XamNavigateBack_entry)
REX_EXPORT(__imp__XamAlloc, rex::kernel::xam::XamAlloc_entry)
REX_EXPORT(__imp__XamFree, rex::kernel::xam::XamFree_entry)
REX_EXPORT(__imp__XamQueryLiveHiveW, rex::kernel::xam::XamQueryLiveHiveW_entry)
REX_EXPORT(__imp__XamLoaderGetDvdTrayState, rex::kernel::xam::XamLoaderGetDvdTrayState_entry)
REX_EXPORT(__imp__XamSwapDisc, rex::kernel::xam::XamSwapDisc_entry)
REX_EXPORT(__imp__RtlGetLastError, rex::kernel::xam::RtlGetLastError_entry)
REX_EXPORT(__imp__GetLastError, rex::kernel::xam::GetLastError_entry)
REX_EXPORT(__imp__RtlSetLastNTError, rex::kernel::xam::RtlSetLastNTError_entry)
REX_EXPORT(__imp__GetCurrentThreadId, rex::kernel::xam::GetCurrentThreadId_entry)
REX_EXPORT(__imp__GetTickCount, rex::kernel::xam::GetTickCount_entry)
REX_EXPORT(__imp__GetSystemTimeAsFileTime, rex::kernel::xam::GetSystemTimeAsFileTime_entry)
REX_EXPORT(__imp__QueryPerformanceFrequency, rex::kernel::xam::QueryPerformanceFrequency_entry)
REX_EXPORT(__imp__RtlSleep, rex::kernel::xam::RtlSleep_entry)
REX_EXPORT(__imp__Sleep, rex::kernel::xam::Sleep_entry)
REX_EXPORT(__imp__SleepEx, rex::kernel::xam::SleepEx_entry)
REX_EXPORT(__imp__XapiFormatTimeOut, rex::kernel::xam::XapiFormatTimeOut_entry)
REX_EXPORT(__imp__WaitForSingleObjectEx, rex::kernel::xam::WaitForSingleObjectEx_entry)
REX_EXPORT(__imp__WaitForSingleObject, rex::kernel::xam::WaitForSingleObject_entry)
REX_EXPORT(__imp__ResumeThread, rex::kernel::xam::ResumeThread_entry)
REX_EXPORT(__imp__ExitThread, rex::kernel::xam::ExitThread_entry)
REX_EXPORT(__imp__XapipCreateThread, rex::kernel::xam::XapipCreateThread_entry)
REX_EXPORT(__imp__CreateThread, rex::kernel::xam::CreateThread_entry)
REX_EXPORT(__imp__GetModuleHandleA, rex::kernel::xam::GetModuleHandleA_entry)
REX_EXPORT(__imp__lstrlenW, rex::kernel::xam::lstrlenW_entry)
REX_EXPORT(__imp__RtlRandom, rex::kernel::xam::RtlRandom_entry)
REX_EXPORT(__imp__XGetOverlappedExtendedError, rex::kernel::xam::XGetOverlappedExtendedError_entry)
REX_EXPORT(__imp__XGetAudioFlags, rex::kernel::xam::XGetAudioFlags_entry)
REX_EXPORT(__imp__XamIsSystemTitleId, rex::kernel::xam::XamIsSystemTitleId_entry)
REX_EXPORT(__imp__XamIsSystemExperienceTitleId,
           rex::kernel::xam::XamIsSystemExperienceTitleId_entry)
REX_EXPORT(__imp__XamIsXbox1TitleId, rex::kernel::xam::XamIsXbox1TitleId_entry)
REX_EXPORT(__imp__XamIsCurrentTitleDash, rex::kernel::xam::XamIsCurrentTitleDash_entry)
REX_EXPORT(__imp__XamGetDashContext, rex::kernel::xam::XamGetDashContext_entry)
REX_EXPORT(__imp__XamSetDashContext, rex::kernel::xam::XamSetDashContext_entry)
REX_EXPORT(__imp__XamGetDashBackstackData, rex::kernel::xam::XamGetDashBackstackData_entry)
REX_EXPORT(__imp__XamIsChildAccountSignedIn, rex::kernel::xam::XamIsChildAccountSignedIn_entry)
REX_EXPORT(__imp__XamIsIptvEnabled, rex::kernel::xam::XamIsIptvEnabled_entry)
REX_EXPORT(__imp__Refresh, rex::kernel::xam::Refresh_entry)
REX_EXPORT(__imp__XamDoesOmniNeedConfiguration,
           rex::kernel::xam::XamDoesOmniNeedConfiguration_entry)
REX_EXPORT(__imp__XamFirstRunExperienceShouldRun,
           rex::kernel::xam::XamFirstRunExperienceShouldRun_entry)
REX_EXPORT(__imp__XamGetDvrStorage, rex::kernel::xam::XamGetDvrStorage_entry)
REX_EXPORT(__imp__XamSetDvrStorage, rex::kernel::xam::XamSetDvrStorage_entry)
REX_EXPORT(__imp__XamGetStagingMode, rex::kernel::xam::XamGetStagingMode_entry)
REX_EXPORT(__imp__XamLogLocalizationEtx, rex::kernel::xam::XamLogLocalizationEtx_entry)
REX_EXPORT(__imp__XamLookupCommonStringByIndex,
           rex::kernel::xam::XamLookupCommonStringByIndex_entry)
REX_EXPORT(__imp__XamUpdateGetBaseSystemVersion,
           rex::kernel::xam::XamUpdateGetBaseSystemVersion_entry)
REX_EXPORT(__imp__XamUpdateGetCurrentSystemVersion,
           rex::kernel::xam::XamUpdateGetCurrentSystemVersion_entry)
REX_EXPORT(__imp__XamXStudioRequest, rex::kernel::xam::XamXStudioRequest_entry)
REX_EXPORT(__imp__XampWebInstrumentationSetProfileCounts,
           rex::kernel::xam::XampWebInstrumentationSetProfileCounts_entry)
REX_EXPORT(__imp__XamGetLiveHiveValueA, rex::kernel::xam::XamGetLiveHiveValueA_entry)
REX_EXPORT(__imp__XamGetLiveHiveValueW, rex::kernel::xam::XamGetLiveHiveValueW_entry)
REX_EXPORT(__imp__XamQueryLiveHiveA, rex::kernel::xam::XamQueryLiveHiveA_entry)
