/**
 * @file        core/event_pulse_test.cpp
 * @brief       Tests that Event::Pulse releases the threads waiting at that moment, and only them
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <rex/thread.h>

namespace {

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::WaitResult;

// Long enough for the waiters to block, and for released ones to run on a loaded machine.
constexpr auto kSettle = 200ms;
constexpr auto kWaitTimeout = 1500ms;

// Starts `count` threads waiting on `event` (alone, or with an unsignalled second event through
// WaitAny), pulses it once they have blocked and returns how many were released. Right after the
// pulse this thread starts a wait of its own, which the pulse must not release: it owes its
// wake-ups to the threads that were waiting, and a late waiter taking one would leave one of them
// asleep.
int PulseWaiters(Event* event, int count, bool wait_any) {
  auto other = Event::CreateManualResetEvent(false);
  std::atomic<int> released{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&] {
      bool ok;
      if (wait_any) {
        rex::thread::WaitHandle* handles[] = {event, other.get()};
        auto result = rex::thread::WaitAny(handles, 2, false, kWaitTimeout);
        ok = result.first == WaitResult::kSuccess && result.second == 0;
      } else {
        ok = rex::thread::Wait(event, false, kWaitTimeout) == WaitResult::kSuccess;
      }
      if (ok) ++released;
    });
  }
  std::this_thread::sleep_for(kSettle);
  event->Pulse();
  CHECK(rex::thread::Wait(event, false, 0ms) == WaitResult::kTimeout);
  for (auto& thread : threads) thread.join();
  return released.load();
}

}  // namespace

TEST_CASE("Pulse of an auto-reset event releases one waiter", "[thread]") {
  auto event = Event::CreateAutoResetEvent(false);
  CHECK(PulseWaiters(event.get(), 3, false) == 1);
  // And leaves the event reset.
  CHECK(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Pulse of a manual-reset event releases every waiter", "[thread]") {
  auto event = Event::CreateManualResetEvent(false);
  CHECK(PulseWaiters(event.get(), 3, false) == 3);
  CHECK(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Pulse releases threads in a multi-object wait", "[thread]") {
  auto manual = Event::CreateManualResetEvent(false);
  CHECK(PulseWaiters(manual.get(), 3, true) == 3);
  auto automatic = Event::CreateAutoResetEvent(false);
  CHECK(PulseWaiters(automatic.get(), 3, true) == 1);
}

TEST_CASE("Pulse with nobody waiting does nothing", "[thread]") {
  auto automatic = Event::CreateAutoResetEvent(false);
  automatic->Pulse();
  CHECK(rex::thread::Wait(automatic.get(), false, 0ms) == WaitResult::kTimeout);
  auto manual = Event::CreateManualResetEvent(false);
  manual->Pulse();
  CHECK(rex::thread::Wait(manual.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Set and Reset in a row release a thread waiting on a manual-reset event", "[thread]") {
  auto event = Event::CreateManualResetEvent(false);
  WaitResult result = WaitResult::kFailed;
  std::thread waiter([&] { result = rex::thread::Wait(event.get(), false, kWaitTimeout); });
  std::this_thread::sleep_for(kSettle);
  event->Set();
  event->Reset();
  waiter.join();
  CHECK(result == WaitResult::kSuccess);
}

TEST_CASE("Set of an auto-reset event goes to the thread already waiting", "[thread]") {
  auto event = Event::CreateAutoResetEvent(false);
  WaitResult result = WaitResult::kFailed;
  std::thread waiter([&] { result = rex::thread::Wait(event.get(), false, kWaitTimeout); });
  std::this_thread::sleep_for(kSettle);
  event->Set();
  CHECK(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
  waiter.join();
  CHECK(result == WaitResult::kSuccess);
}

TEST_CASE("Set after the waiter timed out leaves an auto-reset event signalled", "[thread]") {
  auto event = Event::CreateAutoResetEvent(false);
  CHECK(rex::thread::Wait(event.get(), false, 1ms) == WaitResult::kTimeout);
  event->Set();
  CHECK(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
}

TEST_CASE("SignalAndWait waits from before it signals", "[thread]") {
  auto go = Event::CreateAutoResetEvent(false);
  auto back = Event::CreateAutoResetEvent(false);
  // Released by the signal, this thread answers and tries to take its own answer back.
  bool released = false, stolen = true;
  std::thread partner([&] {
    released = rex::thread::Wait(go.get(), false, kWaitTimeout) == WaitResult::kSuccess;
    back->Set();
    stolen = rex::thread::Wait(back.get(), false, 0ms) == WaitResult::kSuccess;
  });
  std::this_thread::sleep_for(kSettle);
  CHECK(rex::thread::SignalAndWait(go.get(), back.get(), false, kWaitTimeout) ==
        WaitResult::kSuccess);
  partner.join();
  CHECK(released);
  CHECK_FALSE(stolen);
}
