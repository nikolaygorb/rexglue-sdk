/**
 * @file        graphics/gamma_midpoint_test.cpp
 * @brief       Tests for decoding raw gamma bytes into linear UNORM16 render targets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/xenos.h>

using namespace rex::graphics;

namespace {

// Mirrors GammaByteToLinearMidpoint in the Vulkan render target cache: the
// midpoint of the linear range that encodes to the byte.
float GammaByteToLinearMidpoint(uint32_t byte) {
  float recip = 1.0f / 1023.0f;
  float offset = 0.5f / 1023.0f;
  struct Piece {
    uint32_t threshold;
    float recip;
    float offset;
  };
  static const Piece kPieces[] = {
      {64, 1.0f / 511.5f, -31.5f / 511.5f},
      {96, 1.0f / 255.75f, -63.5f / 255.75f},
      {192, 1.0f / 127.875f, -127.5f / 127.875f},
  };
  for (const Piece& piece : kPieces) {
    if (byte >= piece.threshold) {
      recip = piece.recip;
      offset = piece.offset;
    }
  }
  return float(byte) * recip + offset;
}

float QuantizeUnorm16(float value) {
  return std::round(value * 65535.0f) * (1.0f / 65535.0f);
}

uint32_t EncodeGammaByte(float linear) {
  return uint32_t(xenos::LinearToPWLGamma(linear) * 255.0f + 0.5f);
}

}  // namespace

TEST_CASE("Gamma byte midpoints survive the UNORM16 round trip", "[graphics][gamma]") {
  for (uint32_t byte = 0; byte < 256; ++byte) {
    float linear = GammaByteToLinearMidpoint(byte);
    INFO("byte " << byte << " linear " << linear);
    CHECK(EncodeGammaByte(linear) == byte);
    CHECK(EncodeGammaByte(QuantizeUnorm16(linear)) == byte);
  }
}

TEST_CASE("Gamma byte midpoints are monotonic and in range", "[graphics][gamma]") {
  float previous = -1.0f;
  for (uint32_t byte = 0; byte < 256; ++byte) {
    float linear = GammaByteToLinearMidpoint(byte);
    CHECK(linear > previous);
    CHECK(linear >= 0.0f);
    CHECK(linear <= 1.0f);
    previous = linear;
  }
}
