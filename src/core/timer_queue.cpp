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
#include <chrono>
#include <forward_list>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>
#ifndef _WIN32
#include <disruptorplus/blocking_wait_strategy.hpp>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifdef _WIN32
#include "platform_win.h"
#endif

#include <rex/assert.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

namespace dp = disruptorplus;

// The timer thread waits for the next due timer or a new one. The spin strategy polls in
// between (pause loops, then sched_yield 19 times out of 20 and a 1 ms sleep the 20th): with
// the 1 ms timestamp timer that was 5-7% of a core in a race, and up to 1 ms late in its sleep
// phase. On POSIX it blocks on a condition variable with the due time as the deadline (a
// futex: as precise as the thread's timer slack, set to 1 us below). Windows producers keep the
// spin strategy (its condition variable deadlines are whole timer ticks); the dispatch thread
// waits on Win32 handles instead (WaitUntilPublished), since a spin wait sleeps 1 ms at a time
// and fires timers 1-3 ms late.
#ifdef _WIN32
using TimerWaitStrategy = dp::spin_wait_strategy;
#else
using TimerWaitStrategy = dp::blocking_wait_strategy;
#endif

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
#ifdef _WIN32
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!wait_timer_) {
      // Before Windows 10 1803.
      wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
#endif
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

#ifdef _WIN32
    // The handles must outlive the dispatch thread.
    dispatch_thread_.join();
    CloseHandle(wait_timer_);
    CloseHandle(wake_event_);
#endif
    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");
#ifdef __linux__
    prctl(PR_SET_TIMERSLACK, 1000UL);
#endif

    while (!stop_token.stop_requested()) {
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available = WaitUntilPublished(
            next_sequence,
            wait_queue_.empty() ? clock::time_point::max() : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);
#ifdef _WIN32
    // The dispatch thread looks for new items before it waits, so a callback re-arming its own
    // timer needs no wake.
    if (std::this_thread::get_id() != dispatch_thread_.get_id()) {
      SetEvent(wake_event_);
    }
#endif

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // Returns the last published sequence, or next_sequence - 1 if nothing new was published.
  dp::sequence_t WaitUntilPublished(dp::sequence_t next_sequence, clock::time_point timeout) {
#ifdef _WIN32
    dp::sequence_t available = claim_strategy_.last_published_after(next_sequence - 1);
    if (available != next_sequence - 1) {
      return available;
    }
    HANDLE handles[] = {wake_event_, wait_timer_};
    DWORD handle_count = 1;
    DWORD timeout_ms = INFINITE;
    if (timeout != clock::time_point::max()) {
      auto wait = timeout - clock::now();
      if (wait <= clock::duration::zero()) {
        return available;
      }
      // A negative due time is relative, in 100 ns units.
      LARGE_INTEGER due_time;
      due_time.QuadPart =
          -std::chrono::ceil<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(wait)
               .count();
      if (wait_timer_ && SetWaitableTimer(wait_timer_, &due_time, 0, nullptr, nullptr, FALSE)) {
        handle_count = 2;
      } else {
        timeout_ms = DWORD(std::min<int64_t>(
            std::chrono::ceil<std::chrono::milliseconds>(wait).count(), INFINITE - 1));
      }
    }
    WaitForMultipleObjects(handle_count, handles, FALSE, timeout_ms);
    // A publish from another thread sets the event once it is visible. This thread's own
    // publishes came before the check above.
    return claim_strategy_.last_published_after(next_sequence - 1);
#else
    return claim_strategy_.wait_until_published(next_sequence, next_sequence - 1, timeout);
#endif
  }

  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  TimerWaitStrategy wait_strategy_;
  dp::multi_threaded_claim_strategy<TimerWaitStrategy> claim_strategy_;
  dp::sequence_barrier<TimerWaitStrategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
#ifdef _WIN32
  // Set after a publish from another thread.
  HANDLE wake_event_ = nullptr;
  HANDLE wait_timer_ = nullptr;
#endif
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
