/**
 * @file        kernel/native_memory_test.cpp
 * @brief       Tests for which guest calls the native memory hooks take
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/crt/native_memory.h>

using namespace rex::kernel::crt;

TEST_CASE("Native memory hooks take flat guest RAM only", "[kernel]") {
  CHECK(IsFlatGuestRange(0x40000000, 0x1000));
  CHECK(IsFlatGuestRange(0xA0000000, 0x1000));
  CHECK(IsFlatGuestRange(0xDFFFF000, 0x1000));
  CHECK_FALSE(IsFlatGuestRange(0xDFFFF000, 0x1001));
  CHECK_FALSE(IsFlatGuestRange(0xE0000000, 4));
  CHECK_FALSE(IsFlatGuestRange(0xFFFFFFF0, 0x20));
  CHECK(IsFlatGuestRange(0x7EFFF000, 0x1000));
  CHECK_FALSE(IsFlatGuestRange(0x7EFFF000, 0x1001));
  CHECK_FALSE(IsFlatGuestRange(0x7F000000, 4));
}

TEST_CASE("Native memcpy leaves an overlapping copy to the guest", "[kernel]") {
  CHECK(CanMemcpyNatively(0x40000000, 0x40001000, 0x1000));
  CHECK(CanMemcpyNatively(0x40001000, 0x40000000, 0x1000));
  CHECK_FALSE(CanMemcpyNatively(0x40000000, 0x40000FFF, 0x1000));
  CHECK_FALSE(CanMemcpyNatively(0x40000FFF, 0x40000000, 0x1000));
  CHECK_FALSE(CanMemcpyNatively(0x40000000, 0x40000000, 0x10));
  CHECK_FALSE(CanMemcpyNatively(0x40000000, 0x40001000, 0));
}

TEST_CASE("Native memmove takes overlapping ranges", "[kernel]") {
  CHECK(CanCopyNatively(0x40000000, 0x40000010, 0x100));
  CHECK_FALSE(CanCopyNatively(0x7F000000, 0x40000000, 4));
}

TEST_CASE("Native memset takes a clean fill byte only", "[kernel]") {
  CHECK(CanMemsetNatively(0x40000000, 0, 0x100));
  CHECK(CanMemsetNatively(0x40000000, 0xFF, 0x100));
  CHECK_FALSE(CanMemsetNatively(0x40000000, 0x100, 0x100));
  CHECK_FALSE(CanMemsetNatively(0x40000000, 0xFFFFFFFF, 0x100));
  CHECK_FALSE(CanMemsetNatively(0x40000000, 0, 0));
}
