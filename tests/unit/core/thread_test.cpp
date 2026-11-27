#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/platform.h>
#include <rex/thread.h>

TEST_CASE("Empty host waits succeed without entering a platform wait", "[thread][wait]") {
  for (bool alertable : {false, true}) {
    CHECK(rex::thread::WaitAll(nullptr, 0, alertable, std::chrono::milliseconds::max()) ==
          rex::thread::WaitResult::kSuccess);
    const auto any = rex::thread::WaitAny(nullptr, 0, alertable, std::chrono::milliseconds::max());
    CHECK(any.first == rex::thread::WaitResult::kSuccess);
    CHECK(any.second == 0);
  }
}

TEST_CASE("A queued host callback runs on its target in an alertable wait", "[thread][apc]") {
  std::atomic<int> calls{0};
  std::atomic<bool> correct_thread{false};
  std::atomic<uint32_t> target_id{0};
  auto thread = rex::thread::Thread::Create({}, [&] {
    target_id.store(rex::thread::current_thread_system_id());
    for (int attempt = 0; attempt < 100 && !calls.load(std::memory_order_acquire); ++attempt) {
      rex::thread::AlertableSleep(std::chrono::milliseconds(50));
    }
  });
  REQUIRE(thread);
  // Windows runs user APCs queued before a thread starts while it starts, before
  // its routine: queue only once the routine runs.
  for (int attempt = 0; attempt < 1000 && !target_id.load(); ++attempt) {
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }
  REQUIRE(target_id.load() != 0);
  CHECK(thread->QueueUserCallback([&] {
    correct_thread.store(rex::thread::current_thread_system_id() == target_id.load());
    calls.fetch_add(1, std::memory_order_release);
  }));
  REQUIRE(rex::thread::Wait(thread.get(), false, std::chrono::seconds(10)) ==
          rex::thread::WaitResult::kSuccess);
  CHECK(calls.load() == 1);
  CHECK(correct_thread.load());
  CHECK_FALSE(thread->QueueUserCallback([] {}));
}

TEST_CASE("resuming a newly created suspended thread starts its routine", "[thread]") {
  constexpr int kIterations = 64;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    std::atomic<int> starts{0};
    rex::thread::Thread::CreationParameters parameters{};
    parameters.create_suspended = true;
    auto thread = rex::thread::Thread::Create(
        parameters, [&] { starts.fetch_add(1, std::memory_order_release); });
    REQUIRE(thread);

    uint32_t previous_suspend_count = 0;
    REQUIRE(thread->Resume(&previous_suspend_count));
    CHECK(previous_suspend_count == 1);
    REQUIRE(rex::thread::Wait(thread.get(), false, std::chrono::seconds(5)) ==
            rex::thread::WaitResult::kSuccess);
    CHECK(starts.load(std::memory_order_acquire) == 1);
  }
}

#if REX_PLATFORM_WIN32
TEST_CASE("A host wait suspended past its timeout ends after the resume", "[thread][wait]") {
  // xenia-canary b1fa39d1c: a relative timeout restarted in full after a
  // suspend; the wait now has an absolute due time.
  auto event = rex::thread::Event::CreateManualResetEvent(false);
  REQUIRE(event);
  std::atomic<bool> waiting{false};
  std::atomic<int64_t> waited_ms{-1};
  std::atomic<int> result{-1};
  auto thread = rex::thread::Thread::Create({}, [&] {
    waiting.store(true, std::memory_order_release);
    const auto start = std::chrono::steady_clock::now();
    result.store(int(rex::thread::Wait(event.get(), false, std::chrono::milliseconds(300))));
    waited_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count());
  });
  REQUIRE(thread);
  for (int attempt = 0; attempt < 1000 && !waiting.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(waiting.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(thread->Suspend());
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  REQUIRE(thread->Resume());
  REQUIRE(rex::thread::Wait(thread.get(), false, std::chrono::seconds(5)) ==
          rex::thread::WaitResult::kSuccess);
  CHECK(result.load() == int(rex::thread::WaitResult::kTimeout));
  // About 650 ms: the due time passed while suspended. A restarted relative
  // timeout would end near 950 ms.
  CHECK(waited_ms.load() >= 290);
  CHECK(waited_ms.load() < 850);
}
#endif
