/**
 * @file        core/threading_win.cpp
 * @brief       Windows platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <algorithm>
#include <atomic>
#include <memory>

#include <spdlog/spdlog.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/thread/timer_queue.h>

#define LOG_LASTERROR() \
  { spdlog::error("Win32 Error 0x{:08X} in {}(...)", GetLastError(), __FUNCTION__); }

typedef HANDLE (*SetThreadDescriptionFn)(HANDLE hThread, PCWSTR lpThreadDescription);

namespace rex::thread {

void EnableAffinityConfiguration() {
  HANDLE process_handle = GetCurrentProcess();
  DWORD_PTR process_affinity_mask;
  DWORD_PTR system_affinity_mask;
  GetProcessAffinityMask(process_handle, &process_affinity_mask, &system_affinity_mask);
  SetProcessAffinityMask(process_handle, system_affinity_mask);
}

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(GetCurrentThreadId());
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
struct THREADNAME_INFO {
  DWORD dwType;      // Must be 0x1000.
  LPCSTR szName;     // Pointer to name (in user addr space).
  DWORD dwThreadID;  // Thread ID (-1=caller thread).
  DWORD dwFlags;     // Reserved for future use, must be zero.
};
#pragma pack(pop)

void raise_thread_name_exception(HANDLE thread, const std::string& name) {
  if (!IsDebuggerPresent()) {
    return;
  }
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name.c_str();
  info.dwThreadID = ::GetThreadId(thread);
  info.dwFlags = 0;
  __try {
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<ULONG_PTR*>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
}

static void set_thread_name_impl(HANDLE thread, const std::string_view name) {
  auto kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto func = (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription");
    if (func) {
      // Convert UTF-8 name to UTF-16 using Windows API
      int size_needed =
          MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
      std::wstring u16name(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), u16name.data(),
                          size_needed);
      func(thread, u16name.c_str());
    }
  }
  raise_thread_name_exception(thread, std::string(name));
}

void set_current_thread_name(const std::string_view name) {
  set_thread_name_impl(GetCurrentThread(), name);
}

void MaybeYield() {
  SwitchToThread();
  MemoryBarrier();
}

void SyncMemory() {
  MemoryBarrier();
}

// ::Sleep wakes on the system timer's tick and takes whole milliseconds: a
// 1.5 ms sleep is 1 ms, and a 1 ms sleep takes up to a tick longer (1 to 2 ms
// at 1 ms, 15.6 ms when Windows does not apply the program's timer request,
// which Windows 11 may drop while it thinks the window is hidden). A
// high-resolution waitable timer (Windows 10 1803 and later) wakes within a
// fraction of a millisecond of the time asked, like nanosleep on Linux. One
// per thread, made on its first sleep and closed when the thread ends; null
// where Windows has none (then ::Sleep as before). From rexglue PR #524 (cod3
// 0029).
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE ThreadSleepTimer() {
  struct Holder {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~Holder() {
      if (timer) {
        CloseHandle(timer);
      }
    }
  };
  thread_local Holder holder;
  return holder.timer;
}

// Arms the thread's timer `duration` from now; false if there is no timer or
// the duration is too long for it (then the caller sleeps as before).
static bool ArmThreadSleepTimer(HANDLE timer, std::chrono::microseconds duration) {
  // Relative due time in 100 ns units; a day is far beyond any real sleep.
  if (!timer || duration > std::chrono::hours(24)) {
    return false;
  }
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(duration.count()) * 10;
  return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

void Sleep(std::chrono::microseconds duration) {
  if (duration.count() < 100) {
    MaybeYield();
    return;
  }
  HANDLE timer = ThreadSleepTimer();
  if (ArmThreadSleepTimer(timer, duration) &&
      WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
    return;
  }
  ::Sleep(static_cast<DWORD>(duration.count() / 1000));
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  if (duration.count() >= 100) {
    HANDLE timer = ThreadSleepTimer();
    if (ArmThreadSleepTimer(timer, duration)) {
      // Alertable: a queued APC ends the wait early, as SleepEx does.
      const DWORD result = WaitForSingleObjectEx(timer, INFINITE, TRUE);
      if (result == WAIT_IO_COMPLETION) {
        CancelWaitableTimer(timer);
        return SleepResult::kAlerted;
      }
      if (result == WAIT_OBJECT_0) {
        return SleepResult::kSuccess;
      }
    }
  }
  if (SleepEx(static_cast<DWORD>(duration.count() / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  return TlsAlloc();
}

bool FreeTlsHandle(TlsHandle handle) {
  return TlsFree(handle) ? true : false;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(TlsGetValue(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return TlsSetValue(handle, reinterpret_cast<void*>(value)) ? true : false;
}

template <typename T>
class Win32Handle : public T {
 public:
  explicit Win32Handle(HANDLE handle) : handle_(handle) { assert_not_null(handle); }
  ~Win32Handle() override {
    CloseHandle(handle_);
    handle_ = nullptr;
  }

 protected:
  void* native_handle() const override { return handle_; }

  HANDLE handle_ = nullptr;
};

namespace {
using NtWaitForSingleObjectFn = LONG(NTAPI*)(HANDLE handle, BOOLEAN alertable,
                                             PLARGE_INTEGER timeout);
NtWaitForSingleObjectFn GetNtWaitForSingleObject() {
  static const auto fn = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<NtWaitForSingleObjectFn>(
                       reinterpret_cast<void*>(GetProcAddress(ntdll, "NtWaitForSingleObject")))
                 : nullptr;
  }();
  return fn;
}
}  // namespace

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle = wait_handle->native_handle();
  const DWORD timeout_dw = DWORD(timeout.count());
  DWORD result;
  // Windows restarts a relative timeout in full when the waiting thread is
  // suspended and resumed, so a guest thread suspended past its timeout waited
  // the whole time again: wait until an absolute due time instead
  // (xenia-canary b1fa39d1c). Alertable waits keep WaitForSingleObjectEx, as
  // canary does, for its handling of the activation context around APCs.
  NtWaitForSingleObjectFn nt_wait = (!is_alertable && timeout_dw && timeout_dw != INFINITE)
                                        ? GetNtWaitForSingleObject()
                                        : nullptr;
  if (nt_wait) {
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    LARGE_INTEGER due_time;
    // Positive: an absolute system time in 100 ns units.
    due_time.QuadPart =
        int64_t((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) +
        10000LL * int64_t(timeout_dw);
    const LONG status = nt_wait(handle, FALSE, &due_time);
    // STATUS_WAIT_0, STATUS_ABANDONED_WAIT_0 and STATUS_TIMEOUT have the values
    // of WAIT_OBJECT_0, WAIT_ABANDONED and WAIT_TIMEOUT; errors are negative.
    result = status < 0 ? WAIT_FAILED : DWORD(status);
  } else {
    result = WaitForSingleObjectEx(handle, timeout_dw, is_alertable ? TRUE : FALSE);
  }
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle_to_signal = wait_handle_to_signal->native_handle();
  HANDLE handle_to_wait_on = wait_handle_to_wait_on->native_handle();
  DWORD result = SignalObjectAndWait(handle_to_signal, handle_to_wait_on, DWORD(timeout.count()),
                                     is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  if (!wait_handle_count)
    return {WaitResult::kSuccess, 0};
  std::vector<HANDLE> handles(wait_handle_count);
  for (size_t i = 0; i < wait_handle_count; ++i) {
    handles[i] = wait_handles[i]->native_handle();
  }
  DWORD result =
      WaitForMultipleObjectsEx(DWORD(handles.size()), handles.data(), wait_all ? TRUE : FALSE,
                               DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kSuccess, result - WAIT_OBJECT_0);
  } else if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kAbandoned, result - WAIT_ABANDONED_0);
  }
  switch (result) {
    case WAIT_IO_COMPLETION:
      return std::pair<WaitResult, size_t>(WaitResult::kUserCallback, 0);
    case WAIT_TIMEOUT:
      return std::pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
    default:
    case WAIT_FAILED:
      return std::pair<WaitResult, size_t>(WaitResult::kFailed, 0);
  }
}

class Win32Event : public Win32Handle<Event> {
 public:
  explicit Win32Event(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Event() override = default;
  void Set() override { SetEvent(handle_); }
  void Reset() override { ResetEvent(handle_); }
  void Pulse() override { PulseEvent(handle_); }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, TRUE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, FALSE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Semaphore : public Win32Handle<Semaphore> {
 public:
  explicit Win32Semaphore(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Semaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    return ReleaseSemaphore(handle_, release_count, reinterpret_cast<LPLONG>(out_previous_count))
               ? true
               : false;
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  HANDLE handle = CreateSemaphore(nullptr, initial_count, maximum_count, nullptr);
  if (handle) {
    return std::make_unique<Win32Semaphore>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Mutant : public Win32Handle<Mutant> {
 public:
  explicit Win32Mutant(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Mutant() = default;
  bool Release() override { return ReleaseMutex(handle_) ? true : false; }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  HANDLE handle = CreateMutex(nullptr, initial_owner ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Mutant>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Timer : public Win32Handle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit Win32Timer(HANDLE handle) : Win32Handle(handle) {}
  // The repeating item uses the handle, so it is stopped first.
  ~Win32Timer() override { DisarmRepeating(); }

  // SetWaitableTimer treats a negative due time as relative, so a past
  // absolute one is clamped to fire at once.
  static LONGLONG AbsoluteDueTime(WClock_::time_point due_time) {
    return std::max(due_time.time_since_epoch().count(), WClock_::rep(0));
  }

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback) override {
    return SetOnceAt(WClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(GClock_::time_point due_time, std::function<void()> opt_callback) override {
    return SetOnceAt(std::chrono::clock_cast<WClock_>(due_time), std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time, std::function<void()> opt_callback) override {
    DisarmRepeating();
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = AbsoluteDueTime(due_time);
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : NULL;
    return SetWaitableTimer(handle_, &due_time_li, 0, completion_routine, this, FALSE) ? true
                                                                                       : false;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  // A periodic waitable timer re-arms from when it fired, rounded up to the
  // clock tick, so a 16 ms period fired every 31.25 ms on the default 15.625 ms
  // tick, whatever resolution was requested, and a title pacing itself with one
  // ran at half speed (xenia-canary f8e4e6bb5). The timer queue schedules each
  // fire one period after the previous due time instead, as on POSIX. A fire
  // signals the waitable timer and, as SetWaitableTimer's completion routine
  // did, queues the callback as an APC to the thread that set the timer, which
  // runs it in an alertable wait.
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    DisarmRepeating();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_ = nullptr;
      // Unsignaled until the first fire.
      if (!CancelWaitableTimer(handle_)) {
        return false;
      }
    }

    std::shared_ptr<void> setting_thread;
    if (opt_callback) {
      HANDLE thread = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                           0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return false;
      }
      setting_thread = std::shared_ptr<void>(thread, CloseHandle);
    }

    auto active = std::make_shared<std::atomic<bool>>(true);
    HANDLE timer_handle = handle_;
    auto fire = [timer_handle, setting_thread, active,
                 callback = std::move(opt_callback)](void*) {
      // A due time in the past signals at once.
      LARGE_INTEGER past;
      past.QuadPart = 0;
      SetWaitableTimer(timer_handle, &past, 0, nullptr, nullptr, FALSE);
      if (callback) {
        // Each APC owns its copy: the timer may be reset or destroyed before
        // the setting thread runs it, and one queued before a Cancel or a new
        // Set is dropped.
        auto* apc = new RepeatingApcData{callback, active};
        if (!QueueUserAPC(RepeatingApc, static_cast<HANDLE>(setting_thread.get()),
                          reinterpret_cast<ULONG_PTR>(apc))) {
          delete apc;
        }
      }
    };

    auto wait_item = QueueTimerRecurring(std::move(fire), nullptr, due_time, period);
    std::lock_guard<std::mutex> lock(repeating_mutex_);
    repeating_item_ = std::move(wait_item);
    repeating_active_ = std::move(active);
    return true;
  }

  bool Cancel() override {
    DisarmRepeating();
    // Reset the callback immediately so that any completions don't call it.
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    return CancelWaitableTimer(handle_) ? true : false;
  }

 private:
  static void CompletionRoutine(Win32Timer* timer, DWORD timer_low, DWORD timer_high) {
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    callback();
  }

  struct RepeatingApcData {
    std::function<void()> callback;
    std::shared_ptr<std::atomic<bool>> active;
  };

  static void CALLBACK RepeatingApc(ULONG_PTR parameter) {
    std::unique_ptr<RepeatingApcData> apc(reinterpret_cast<RepeatingApcData*>(parameter));
    if (apc->active->load(std::memory_order_acquire)) {
      apc->callback();
    }
  }

  // Waits for a fire running on the timer queue's thread.
  void DisarmRepeating() {
    std::weak_ptr<TimerQueueWaitItem> item;
    {
      std::lock_guard<std::mutex> lock(repeating_mutex_);
      item = std::move(repeating_item_);
      repeating_item_.reset();
      if (repeating_active_) {
        repeating_active_->store(false, std::memory_order_release);
        repeating_active_.reset();
      }
    }
    if (auto locked_item = item.lock()) {
      locked_item->Disarm();
    }
  }

  std::mutex mutex_;
  std::function<void()> callback_;
  std::mutex repeating_mutex_;
  std::weak_ptr<TimerQueueWaitItem> repeating_item_;
  std::shared_ptr<std::atomic<bool>> repeating_active_;
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, TRUE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, FALSE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Thread : public Win32Handle<Thread> {
 public:
  explicit Win32Thread(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Thread() = default;

  void set_name(std::string name) override {
    set_thread_name_impl(handle_, name);
    Thread::set_name(name);
  }

  int32_t priority() override { return GetThreadPriority(handle_); }
  uint32_t system_id() const override { return GetThreadId(handle_); }

  void set_priority(int32_t new_priority) override { SetThreadPriority(handle_, new_priority); }

  uint64_t affinity_mask() override {
    DWORD_PTR prev = SetThreadAffinityMask(handle_, 1);
    if (prev) {
      SetThreadAffinityMask(handle_, prev);
    }
    return static_cast<uint64_t>(prev);
  }

  void set_affinity_mask(uint64_t new_affinity_mask) override {
    SetThreadAffinityMask(handle_, new_affinity_mask);
  }

  struct ApcData {
    std::function<void()> callback;
  };
  static void NTAPI DispatchApc(ULONG_PTR parameter) {
    auto apc_data = reinterpret_cast<ApcData*>(parameter);
    apc_data->callback();
    delete apc_data;
  }

  bool QueueUserCallback(std::function<void()> callback) override {
    auto apc_data = new ApcData({std::move(callback)});
    if (!QueueUserAPC(DispatchApc, handle_, reinterpret_cast<ULONG_PTR>(apc_data))) {
      const DWORD error = GetLastError();
      delete apc_data;
      spdlog::warn("QueueUserAPC failed ({})", error);
      return false;
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = ResumeThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = SuspendThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  void Terminate(int exit_code) override { TerminateThread(handle_, exit_code); }

 private:
  void AssertCallingThread() { assert_true(GetCurrentThreadId() == GetThreadId(handle_)); }
};

thread_local std::unique_ptr<Win32Thread> current_thread_ = nullptr;

struct ThreadStartData {
  std::function<void()> start_routine;
};
DWORD WINAPI ThreadStartRoutine(LPVOID parameter) {
  current_thread_ = std::make_unique<Win32Thread>(::GetCurrentThread());

  auto start_data = reinterpret_cast<ThreadStartData*>(parameter);
  start_data->start_routine();
  delete start_data;
  return 0;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto start_data = new ThreadStartData({std::move(start_routine)});
  HANDLE handle = CreateThread(NULL, params.stack_size, ThreadStartRoutine, start_data,
                               params.create_suspended ? CREATE_SUSPENDED : 0, NULL);
  if (handle) {
    return std::make_unique<Win32Thread>(handle);
  } else {
    LOG_LASTERROR();
    delete start_data;
    return nullptr;
  }
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_.get();
  }

  HANDLE handle = ::GetCurrentThread();
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }

  current_thread_ = std::make_unique<Win32Thread>(handle);
  return current_thread_.get();
}

void Thread::Exit(int exit_code) {
  ExitThread(exit_code);
}

}  // namespace rex::thread
