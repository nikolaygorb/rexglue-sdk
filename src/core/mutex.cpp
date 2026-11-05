/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#include <chrono>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/thread.h>

REXCVAR_DEFINE_INT32(global_lock_spin_us, -1, "Kernel",
                     "How long a thread that finds the global lock taken keeps checking for it "
                     "before it sleeps, in microseconds. Kernel calls hold the lock for a few "
                     "microseconds, while a sleeping thread takes longer to wake. -1 = auto: 30 "
                     "with 6 or more logical processors, 0 (sleep at once) with fewer")
    .range(-1, 1000);

namespace rex::thread {

namespace {

// Whether the lock looks free (glibc: its futex word is 0), so a waiter only
// writes to the lock's cache line with a try when it may get the lock.
// Elsewhere, always try.
bool LooksFree() {
#if defined(__GLIBC__)
  return __atomic_load_n(&global_critical_region::mutex().native_handle()->__data.__lock,
                         __ATOMIC_RELAXED) == 0;
#else
  return true;
#endif
}

void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

// Spinning only pays with cores to spare: on a 2-4 core PC the holder may be
// waiting for the very core the spinner takes.
int64_t SpinMicros() {
  const int32_t setting = REXCVAR_GET(global_lock_spin_us);
  if (setting >= 0) {
    return setting;
  }
  static const int64_t by_machine = logical_processor_count() >= 6 ? 30 : 0;
  return by_machine;
}

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

void global_critical_region::LockContended(std::unique_lock<std::recursive_mutex>& lock) {
  const int64_t start = NowMicros();
  // Spin first: blocking costs a sleep and a wake-up, longer than most holds.
  // cod3 measured the GPU thread, which takes the region for every draw's
  // memory ranges, waiting 9-22% of its time for it without this (Linux).
  bool got = false;
  if (const int64_t spin_us = SpinMicros(); spin_us > 0) {
    for (uint32_t i = 1;; ++i) {
      CpuRelax();
      if (LooksFree() && lock.try_lock()) {
        got = true;
        break;
      }
      // The clock is read every 64 checks (a few microseconds).
      if ((i & 63) == 0 && NowMicros() - start >= spin_us) {
        break;
      }
    }
  }
  if (!got) {
    lock.lock();
  }
  PROFILE_CRITICAL_REGION_CONTENTION();
  PERF_counter_add(kCriticalRegionWaitUs, NowMicros() - start);
  if (got) {
    PERF_counter_inc(kCriticalRegionSpinWins);
  }
}

}  // namespace rex::thread
