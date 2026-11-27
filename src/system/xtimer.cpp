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

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtimer.h>

namespace rex::system {

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  Cancel();
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = rex::thread::Timer::CreateManualResetTimer();
      manual_reset_ = true;
      break;
    case 1:  // SynchronizationTimer
      timer_ = rex::thread::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                          uint32_t routine_arg, bool resume) {
  using rex::chrono::WinSystemClock;
  using rex::chrono::XSystemClock;
  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  // Guests pass an absolute due time of 0 (the 1601 NT epoch) to mean "fire
  // immediately". Casting that to the host's steady_clock overflows int64 and
  // yields a far-future deadline the timer never reaches, so clamp a past-due
  // time here, where the comparison cannot overflow.
  auto now_wsc = WinSystemClock::now();
  if (due_tp < now_wsc) {
    due_tp = now_wsc;
  }

  std::lock_guard lock(callback_mutex_);
  if (callback_active_)
    callback_active_->store(false, std::memory_order_release);
  if (auto expiry = callback_expiry_.lock())
    expiry->Disarm();
  callback_expiry_.reset();
  callback_active_.reset();
  // A host waitable timer's APC only runs in a host alertable wait. Signal the
  // object independently, and enqueue completion via the timer queue/dispatcher.
  const bool result =
      !period_ms ? timer_->SetOnceAt(due_tp, nullptr)
                 : timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms), nullptr);
  if (!result)
    return X_STATUS_UNSUCCESSFUL;
  if (routine) {
    if (!XThread::IsInThread()) {
      timer_->Cancel();
      return X_STATUS_UNSUCCESSFUL;
    }
    const uint32_t thread_id = XThread::GetCurrentThread()->thread_id();
    auto active = std::make_shared<std::atomic<bool>>(true);
    callback_active_ = active;
    auto* kernel = kernel_state();
    auto callback = [kernel, thread_id, routine, routine_arg, active](void*) {
      if (!active->load(std::memory_order_acquire))
        return;
      const uint64_t time = rex::chrono::Clock::QueryGuestSystemTime();
      kernel->PostDeferred([kernel, thread_id, routine, routine_arg, active, time] {
        if (!active->load(std::memory_order_acquire))
          return;
        if (auto thread = kernel->GetThreadByID(thread_id); thread && thread->is_running()) {
          thread->EnqueueApc(routine, routine_arg, static_cast<uint32_t>(time),
                             static_cast<uint32_t>(time >> 32));
        }
      });
    };
    using QueueClock = rex::thread::TimerQueueWaitItem::clock;
    const auto wait = std::max(due_tp - WinSystemClock::now(), WinSystemClock::duration::zero());
    const auto due = QueueClock::now() + std::chrono::duration_cast<QueueClock::duration>(wait);
    callback_expiry_ = period_ms
                           ? rex::thread::QueueTimerRecurring(std::move(callback), nullptr, due,
                                                              std::chrono::milliseconds(period_ms))
                           : rex::thread::QueueTimerOnce(std::move(callback), nullptr, due);
  }

  // The timer is set either way; the caller checks for
  // STATUS_TIMER_RESUME_IGNORED to learn that resume is not supported.
  if (resume) {
    XThread::SetLastError(X_RESULT_FROM_WIN32(0x32));  // ERROR_NOT_SUPPORTED
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard lock(callback_mutex_);
  if (callback_active_)
    callback_active_->store(false, std::memory_order_release);
  if (auto expiry = callback_expiry_.lock())
    expiry->Disarm();
  callback_expiry_.reset();
  callback_active_.reset();
  return !timer_ || timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

bool XTimer::IsSignaled() {
  // A zero wait on the host timer would consume a synchronization timer's
  // signal, so only a notification timer can be read.
  return manual_reset_ &&
         rex::thread::Wait(timer_.get(), false, std::chrono::milliseconds(0)) ==
             rex::thread::WaitResult::kSuccess;
}

}  // namespace rex::system
