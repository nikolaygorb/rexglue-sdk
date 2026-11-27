#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/system/xio.h>
#include <rex/types.h>

using rex::X_RESULT;

TEST_CASE("Atomic endian words retain their guest layout", "[core][atomic]") {
  rex::be<uint32_t> big{};
  rex::le<uint32_t> little{};
  static_assert(sizeof(big) == 4 && sizeof(little) == 4);
  big.store(0x12345678, std::memory_order_release);
  little.store(0x12345678, std::memory_order_release);
  const auto* b = reinterpret_cast<const uint8_t*>(&big);
  const auto* l = reinterpret_cast<const uint8_t*>(&little);
  CHECK(b[0] == 0x12);
  CHECK(b[3] == 0x78);
  CHECK(l[0] == 0x78);
  CHECK(l[3] == 0x12);
  CHECK(big.load(std::memory_order_acquire) == 0x12345678);
  CHECK(little.load(std::memory_order_acquire) == 0x12345678);
}

TEST_CASE("An overlapped result publishes length and extended error", "[kernel][overlapped]") {
  using namespace rex::system;
  XAM_OVERLAPPED overlapped{};
  XOverlappedSetResult(&overlapped, X_ERROR_IO_PENDING);
  std::thread writer([&] {
    XOverlappedSetLength(&overlapped, 1234);
    XOverlappedSetExtendedError(&overlapped, 5678);
    XOverlappedSetResult(&overlapped, 0);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  uint32_t result;
  do {
    result = XOverlappedGetResult(&overlapped);
    if (result != X_ERROR_IO_PENDING)
      break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  if (result == 0) {
    CHECK(XOverlappedGetLength(&overlapped) == 1234);
    CHECK(uint32_t(overlapped.extended_error) == 5678);
  }
  writer.join();
  CHECK(result == 0);
}
