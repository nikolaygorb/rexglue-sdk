/**
 * @file        tests/unit/graphics/memexport_write_back_test.cpp
 * @brief       Tests for writing memexport results back to guest memory
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include <rex/graphics/util/draw.h>

using rex::graphics::draw_util::WriteBackChangedBytes;

TEST_CASE("WriteBackChangedBytes keeps what the CPU wrote during the draw", "[gpu][memexport]") {
  const std::array<uint8_t, 6> before = {1, 2, 3, 4, 5, 6};
  // The shaders changed bytes 1 and 4, also to a value in a single byte of a dword.
  const std::array<uint8_t, 6> after = {1, 20, 3, 4, 50, 6};
  // The CPU changed bytes 0 and 1 meanwhile.
  std::array<uint8_t, 6> guest = {100, 101, 3, 4, 5, 6};

  WriteBackChangedBytes(guest.data(), before.data(), after.data(), uint32_t(guest.size()));

  // The export wins where both wrote.
  const std::array<uint8_t, 6> expected = {100, 20, 3, 4, 50, 6};
  CHECK(guest == expected);
}

TEST_CASE("WriteBackChangedBytes leaves guest memory alone without changes", "[gpu][memexport]") {
  const std::array<uint8_t, 4> same = {1, 2, 3, 4};
  std::array<uint8_t, 4> guest = {9, 9, 9, 9};

  WriteBackChangedBytes(guest.data(), same.data(), same.data(), uint32_t(guest.size()));

  const std::array<uint8_t, 4> expected = {9, 9, 9, 9};
  CHECK(guest == expected);
}
