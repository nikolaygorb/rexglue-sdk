/**
 * @file        ppc/vector_store_test.cpp
 * @brief       stvlx / stvrx stores against the byte loops they replace
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/intrinsics.h>

namespace {

constexpr uint32_t kBase = 0x10001040;  // a quadword-aligned guest address

std::array<uint8_t, 16> Register() {
  std::array<uint8_t, 16> reg{};
  for (int i = 0; i < 16; ++i) {
    reg[i] = static_cast<uint8_t>(0xA0 + i);
  }
  return reg;
}

}  // namespace

TEST_CASE("stvlx and stvrx store what the byte loops stored", "[ppc][vector]") {
  const auto reg = Register();
  for (uint32_t offset = 0; offset < 16; ++offset) {
    INFO("ea & 0xF = " << offset);
    // Three quadwords; the store goes to the middle one (stvlx) or ends in it
    // (stvrx). Every other byte must stay as it was.
    std::array<uint8_t, 48> expected{}, actual{};
    expected.fill(0x5A);
    actual.fill(0x5A);
    uint8_t* const middle_expected = expected.data() + 16;
    uint8_t* const middle_actual = actual.data() + 16;
    const uint32_t ea = kBase + offset;

    // stvlx: for i < 16 - (ea & 0xF): byte ea + i = u8[15 - i].
    for (uint32_t i = 0; i < 16 - (ea & 0xF); ++i) {
      middle_expected[offset + i] = reg[15 - i];
    }
    rex::ppc::StoreVectorLeft(middle_actual + offset, ea, reg.data());
    CHECK(actual == expected);

    // stvrx: for i < (ea & 0xF): byte ea - i - 1 = u8[i].
    const uint32_t right_ea = ea + 16;
    for (uint32_t i = 0; i < (right_ea & 0xF); ++i) {
      middle_expected[16 + offset - i - 1] = reg[i];
    }
    rex::ppc::StoreVectorRight(middle_actual + 16, right_ea, reg.data());
    CHECK(actual == expected);
  }
}
