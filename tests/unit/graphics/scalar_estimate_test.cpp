/**
 * @file        graphics/scalar_estimate_test.cpp
 * @brief       Tests for xenos::Rcp, Rsq, Sqrt, Exp and Log at exact results
 *              and special values
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <bit>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/xenos.h>

using namespace rex::graphics;

namespace {

uint32_t Bits(float value) {
  return std::bit_cast<uint32_t>(value);
}

constexpr uint32_t kPositiveInfinity = 0x7F800000;
constexpr uint32_t kNegativeInfinity = 0xFF800000;
constexpr uint32_t kDefaultNaN = 0xFFC00000;

}  // namespace

TEST_CASE("Scalar estimates are exact at the start of a segment", "[graphics]") {
  CHECK(xenos::Rcp(1.0f) == 1.0f);
  CHECK(xenos::Rsq(1.0f) == 1.0f);
  CHECK(xenos::Sqrt(1.0f) == 1.0f);
  CHECK(xenos::Sqrt(4.0f) == 2.0f);
  CHECK(xenos::Exp(1.0f) == 2.0f);
  CHECK(xenos::Exp(-1.0f) == 0.5f);
}

TEST_CASE("Scalar estimates treat denormals as zeros", "[graphics]") {
  const float denormal = std::bit_cast<float>(uint32_t(1));
  CHECK(Bits(xenos::Rcp(0.0f)) == kPositiveInfinity);
  CHECK(Bits(xenos::Rcp(-0.0f)) == kNegativeInfinity);
  CHECK(Bits(xenos::Rcp(denormal)) == kPositiveInfinity);
  CHECK(Bits(xenos::Rsq(0.0f)) == kPositiveInfinity);
  CHECK(Bits(xenos::Sqrt(denormal)) == 0);
  CHECK(xenos::Exp(denormal) == 1.0f);
  CHECK(Bits(xenos::Log(denormal)) == kNegativeInfinity);
}

TEST_CASE("Scalar estimates of negative numbers and infinities", "[graphics]") {
  const float infinity = std::bit_cast<float>(kPositiveInfinity);
  CHECK(Bits(xenos::Rsq(-1.0f)) == kDefaultNaN);
  CHECK(Bits(xenos::Sqrt(-1.0f)) == kDefaultNaN);
  CHECK(Bits(xenos::Log(-1.0f)) == kDefaultNaN);
  CHECK(Bits(xenos::Rcp(infinity)) == 0);
  CHECK(Bits(xenos::Rsq(infinity)) == 0);
  CHECK(Bits(xenos::Sqrt(infinity)) == kPositiveInfinity);
  CHECK(Bits(xenos::Exp(infinity)) == kPositiveInfinity);
  CHECK(Bits(xenos::Exp(-infinity)) == 0);
}

TEST_CASE("Scalar estimates pass NaNs through", "[graphics]") {
  const float nan = std::bit_cast<float>(uint32_t(0x7FC12345));
  CHECK(Bits(xenos::Rcp(nan)) == 0x7FC12345);
  CHECK(Bits(xenos::Rsq(nan)) == 0x7FC12345);
  CHECK(Bits(xenos::Sqrt(nan)) == 0x7FC12345);
  CHECK(Bits(xenos::Exp(nan)) == 0x7FC12345);
  CHECK(Bits(xenos::Log(nan)) == 0x7FC12345);
}
