#include <catch2/catch_test_macros.hpp>
#include <rex/thread/guest_spinlock.h>
#include <rex/thread/mutex.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("Guest spinlocks serialize contending owners", "[thread][spinlock]") {
  uint32_t word = 0;
  uint32_t count = 0;
  std::vector<std::thread> workers;
  for (uint32_t owner = 1; owner <= 4; ++owner) {
    workers.emplace_back([&, owner] {
      for (uint32_t i = 0; i < 200; ++i) {
        rex::thread::AcquireGuestSpinLock(&word, owner);
        ++count;
        rex::thread::ReleaseGuestSpinLock(&word);
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  CHECK(count == 800);
  CHECK(word == 0);
}

TEST_CASE("A contended guest spinlock releases and restores nested guest regions",
          "[thread][spinlock]") {
  using Region = rex::thread::global_critical_region;
  uint32_t word = 1;
  Region::mutex().lock();
  Region::mutex().lock();
  Region::guest_depth() = 2;
  bool got_region = false;
  std::thread holder([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(got_region = Region::mutex().try_lock()) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    rex::thread::ReleaseGuestSpinLock(&word);
    if (got_region)
      Region::mutex().unlock();
  });
  rex::thread::AcquireGuestSpinLock(&word, 2);
  holder.join();
  const auto depth = Region::guest_depth();
  rex::thread::ReleaseGuestSpinLock(&word);
  Region::guest_depth() = 0;
  Region::mutex().unlock();
  Region::mutex().unlock();
  CHECK(got_region);
  CHECK(depth == 2);
}
