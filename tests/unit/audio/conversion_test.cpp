/**
 * @file        audio/conversion_test.cpp
 * @brief       Tests for the guest mix to host output conversion
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <rex/audio/conversion.h>

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// The guest mix arrives big-endian, one channel after another.
std::vector<float> GuestMix(const std::vector<std::vector<float>>& channels) {
  std::vector<float> mix;
  for (const auto& channel : channels) {
    for (float sample : channel) {
      uint32_t bits;
      std::memcpy(&bits, &sample, sizeof(bits));
      bits = rex::byte_swap(bits);
      std::memcpy(&sample, &bits, sizeof(bits));
      mix.push_back(sample);
    }
  }
  return mix;
}

}  // namespace

TEST_CASE("The stereo fold plays a non-finite mix as silence", "[audio]") {
  const std::vector<float> silent(4, 0.0f);
  const auto input = GuestMix(
      {{kNaN, kInf, -kInf, 4.0f}, {-4.0f, 0.0f, 0.0f, 0.0f}, silent, silent, silent, silent});
  // Fronts only, at unity, so each side is its front channel clamped.
  const rex::audio::StereoFold fold = {
      .center = 0.0f, .surround = 0.0f, .lfe = 0.0f, .scale = 1.0f};
  std::vector<float> output(8);
  rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(output.data(), input.data(), 4, fold,
                                                              1.0f);
  REQUIRE(output == std::vector<float>{0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
}

TEST_CASE("The 5.1 passthrough plays a non-finite mix as silence", "[audio]") {
  const std::vector<float> silent(2, 0.0f);
  const auto input =
      GuestMix({{kNaN, -kInf}, {kInf, 0.25f}, {8.0f, -8.0f}, silent, silent, silent});
  std::vector<float> output(12);
  rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(output.data(), input.data(), 2, {},
                                                              1.0f);
  REQUIRE(output == std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,  //
                                       0.0f, 0.25f, -1.0f, 0.0f, 0.0f, 0.0f});
}
