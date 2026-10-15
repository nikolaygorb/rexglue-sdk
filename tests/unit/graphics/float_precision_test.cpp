/**
 * @file        graphics/float_precision_test.cpp
 * @brief       Tests for the Xenos scalar approximation precision reduction
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/graphics/pipeline/shader/interpreter.h>

using rex::graphics::ShaderInterpreter;

namespace {

uint32_t Reduce21(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  float result = ShaderInterpreter::ReduceFloatPrecision(value, 21);
  uint32_t result_bits;
  std::memcpy(&result_bits, &result, sizeof(result_bits));
  return result_bits;
}

}  // namespace

// xenia-canary 3a44f20c7: EXP, LOG, RCP, RSQ, SQRT and variants keep 21 mantissa bits, rounded to
// nearest with halfway values away from zero.
TEST_CASE("ReduceFloatPrecision rounds to 21 mantissa bits", "[graphics][alu]") {
  CHECK(Reduce21(0x3F800000) == 0x3F800000);  // 1.0 is exact.
  CHECK(Reduce21(0x3F800001) == 0x3F800000);  // Below half: truncated.
  CHECK(Reduce21(0x3F800002) == 0x3F800004);  // Exactly half: away from zero.
  CHECK(Reduce21(0x3F800003) == 0x3F800004);  // Above half: rounded up.
  CHECK(Reduce21(0xBF800002) == 0xBF800004);  // Negative half: away from zero.
  CHECK(Reduce21(0x3FFFFFFE) == 0x40000000);  // Carry into the exponent.
}

TEST_CASE("ReduceFloatPrecision keeps special values", "[graphics][alu]") {
  CHECK(Reduce21(0x00000000) == 0x00000000);  // +0
  CHECK(Reduce21(0x80000000) == 0x80000000);  // -0 stays signed.
  CHECK(Reduce21(0x7F800000) == 0x7F800000);  // +Inf
  CHECK(Reduce21(0xFF800000) == 0xFF800000);  // -Inf
  CHECK(Reduce21(0x7FC00001) == 0x7FC00001);  // NaN payload untouched.
  // FLT_MAX would round to infinity: the truncated value is kept instead.
  CHECK(Reduce21(0x7F7FFFFF) == 0x7F7FFFFC);
}
