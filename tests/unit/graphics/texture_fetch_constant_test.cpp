/**
 * @file        graphics/texture_fetch_constant_test.cpp
 * @brief       Tests for texture fetch constant fields read by the shader translators
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

#include <rex/graphics/xenos.h>

using namespace rex::graphics;

// xenia-canary 8486e97a0. The DXBC and SPIR-V translators read MipMinLevel and
// MipMaxLevel from word 4 with hardcoded bit offsets (2:5 and 6:9).
TEST_CASE("Texture fetch constant mip levels are at word 4 bits 2:5 and 6:9",
          "[graphics][texture]") {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.mip_min_level = 0b1011;
  fetch.mip_max_level = 0b0110;
  CHECK(((fetch.dword_4 >> 2) & 0xF) == 0b1011);
  CHECK(((fetch.dword_4 >> 6) & 0xF) == 0b0110);
  CHECK((fetch.dword_4 & ~(UINT32_C(0xFF) << 2)) == 0);
}

// xenia-canary 0f2980de4. The translators read the per-axis gradient exponent biases
// (LodBiasH/V) from word 4 as signed 5-bit fields at bits 22 and 27.
TEST_CASE("Texture fetch constant gradient exponent biases are at word 4 bits 22 and 27",
          "[graphics][texture]") {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.grad_exp_adjust_h = -16;
  fetch.grad_exp_adjust_v = 15;
  CHECK(int32_t(fetch.dword_4 << 5) >> 27 == -16);
  CHECK(int32_t(fetch.dword_4) >> 27 == 15);
}

// xenia-canary 0f2980de4. DXBC turns a bias into a gradient scale with an integer
// multiply-add: (bias << 23) + asuint(1.0f) is the bit pattern of 2^bias.
TEST_CASE("Gradient exponent bias integer trick gives 2^bias", "[graphics][texture]") {
  for (int32_t bias = -16; bias <= 15; ++bias) {
    uint32_t bits = uint32_t(bias * (int32_t(1) << 23)) + UINT32_C(0x3F800000);
    float scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    CHECK(scale == std::ldexp(1.0f, bias));
  }
}

// xenia-canary 9e9d3cdd3. Vertex fetch bounds use the address (dwords, bits 2:31 of
// word 0) and the size (words, bits 2:25 of word 1); the end in bytes is
// (word_0 & ~3) + size * 4.
TEST_CASE("Vertex fetch constant end is base plus size", "[graphics][fetch]") {
  xenos::xe_gpu_vertex_fetch_t fetch = {};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = 0x1234567;
  fetch.endian = xenos::Endian::k8in32;
  fetch.size = 0xABCDEF;
  CHECK((fetch.dword_0 >> 2) == 0x1234567);
  CHECK(((fetch.dword_1 >> 2) & ((UINT32_C(1) << 24) - 1)) == 0xABCDEF);
  CHECK((fetch.dword_0 & ~UINT32_C(3)) + ((fetch.dword_1 >> 2) & 0xFFFFFF) * 4 ==
        (UINT32_C(0x1234567) + UINT32_C(0xABCDEF)) * 4);
}
