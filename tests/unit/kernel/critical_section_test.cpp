/**
 * @file        tests/unit/kernel/critical_section_test.cpp
 * @brief       Critical section LockCount kept big-endian in guest memory
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xboxkrnl/rtl.h>

namespace {
// The four bytes as guest memory holds them.
uint32_t GuestBytes(int32_t raw) {
  uint8_t bytes[4];
  std::memcpy(bytes, &raw, sizeof(bytes));
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}
}  // namespace

TEST_CASE("Critical section LockCount is counted big-endian", "[kernel][rtl]") {
  using rex::kernel::xboxkrnl::CriticalSectionAddLockCount;
  // -1: free (the same bytes in either order).
  volatile int32_t lock_count = -1;
  CHECK(CriticalSectionAddLockCount(&lock_count, 1) == 0);
  CHECK(GuestBytes(lock_count) == 0u);
  // A recursive entry: the guest reads 1, not 0x01000000.
  CHECK(CriticalSectionAddLockCount(&lock_count, 1) == 1);
  CHECK(GuestBytes(lock_count) == 1u);
  CHECK(CriticalSectionAddLockCount(&lock_count, 1) == 2);
  CHECK(GuestBytes(lock_count) == 2u);
  CHECK(CriticalSectionAddLockCount(&lock_count, -1) == 1);
  CHECK(CriticalSectionAddLockCount(&lock_count, -1) == 0);
  CHECK(CriticalSectionAddLockCount(&lock_count, -1) == -1);
  CHECK(GuestBytes(lock_count) == 0xFFFFFFFFu);
}

TEST_CASE("Critical section LockCount reads a guest-written count", "[kernel][rtl]") {
  using rex::kernel::xboxkrnl::CriticalSectionAddLockCount;
  // A section the title placed in its data with LockCount = 4 (big-endian).
  const uint8_t guest[4] = {0x00, 0x00, 0x00, 0x04};
  int32_t raw;
  std::memcpy(&raw, guest, sizeof(raw));
  volatile int32_t lock_count = raw;
  CHECK(CriticalSectionAddLockCount(&lock_count, 1) == 5);
  CHECK(GuestBytes(lock_count) == 5u);
}
