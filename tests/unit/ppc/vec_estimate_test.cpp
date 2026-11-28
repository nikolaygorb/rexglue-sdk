/**
 * @file        ppc/vec_estimate_test.cpp
 * @brief       Tests for the lane-parallel vrsqrtefp, vrefp, vlogefp and vexptefp against the
 *              scalar estimates
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <bit>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/intrinsics.h>

using namespace rex::ppc;

namespace {

// A prime stride covers every exponent and table segment; the full 2^32 sweep
// was run once outside the unit tests (0 mismatches).
constexpr uint64_t kStride = 997;

simde__m128 Lanes(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return simde_mm_castsi128_ps(simde_mm_setr_epi32(int32_t(a), int32_t(b), int32_t(c), int32_t(d)));
}

template <uint32_t (*Estimate)(uint32_t)>
uint64_t CountMismatches(uint64_t first = 0, uint64_t end = uint64_t(1) << 32,
                         uint64_t stride = kStride) {
  uint64_t mismatches = 0;
  for (uint64_t base = first; base < end; base += stride * 4) {
    alignas(16) uint32_t in[4];
    for (uint32_t i = 0; i < 4; ++i)
      in[i] = uint32_t(base + i * stride);
    alignas(16) uint32_t out[4];
    simde_mm_store_ps(reinterpret_cast<float*>(out),
                      vec_estimate<Estimate>(Lanes(in[0], in[1], in[2], in[3])));
    for (uint32_t i = 0; i < 4; ++i)
      mismatches += out[i] != Estimate(in[i]);
  }
  return mismatches;
}

}  // namespace

TEST_CASE("Vector vrsqrtefp equals the scalar estimate", "[ppc][estimate]") {
  CHECK(CountMismatches<estimate_reciprocal_sqrt>() == 0);
}

TEST_CASE("Vector vrefp equals the scalar estimate", "[ppc][estimate]") {
  CHECK(CountMismatches<estimate_reciprocal>() == 0);
}

TEST_CASE("Vector vlogefp equals the scalar estimate", "[ppc][estimate]") {
  CHECK(CountMismatches<estimate_log2>() == 0);
}

TEST_CASE("Vector vexptefp equals the scalar estimate", "[ppc][estimate]") {
  CHECK(CountMismatches<estimate_pow2>() == 0);
  // Denser where the result goes from 0 through normals to infinity:
  // exponents 118..134 of both signs.
  CHECK(CountMismatches<estimate_pow2>(0x3B000000, 0x43800000, 61) == 0);
  CHECK(CountMismatches<estimate_pow2>(0xBB000000, 0xC3800000, 61) == 0);
}

TEST_CASE("Vector vlogefp and vexptefp take the fast path on their ranges", "[ppc][estimate]") {
  simde__m128 r;
  CHECK(vec_estimate_log2_normal(Lanes(0x00800000, 0x7F7FFFFF, 0x3F800000, 0x3F000000), r));
  CHECK_FALSE(vec_estimate_log2_normal(Lanes(0x3F800000, 0x00000000, 0x3F800000, 0x3F800000), r));
  CHECK_FALSE(vec_estimate_log2_normal(Lanes(0x3F800000, 0xBF800000, 0x3F800000, 0x3F800000), r));
  // -126, 127.99..., a denormal (2^0) and -0.
  CHECK(vec_estimate_pow2_normal(Lanes(0xC2FC0000, 0x42FFFFFF, 0x00000001, 0x80000000), r));
  // -127 (result 0), 128 and NaN take the scalar path.
  CHECK_FALSE(vec_estimate_pow2_normal(Lanes(0xC2FE0000, 0x3F800000, 0x3F800000, 0x3F800000), r));
  CHECK_FALSE(vec_estimate_pow2_normal(Lanes(0x43000000, 0x3F800000, 0x3F800000, 0x3F800000), r));
  CHECK_FALSE(vec_estimate_pow2_normal(Lanes(0x7FC00000, 0x3F800000, 0x3F800000, 0x3F800000), r));
}

TEST_CASE("Vector estimates take the fast path exactly on their ranges", "[ppc][estimate]") {
  simde__m128 r;
  // Smallest and largest positive normal.
  CHECK(vec_estimate_rsqrt_normal(Lanes(0x00800000, 0x7F7FFFFF, 0x3F800000, 0x40000000), r));
  CHECK_FALSE(vec_estimate_rsqrt_normal(Lanes(0x3F800000, 0x3F800000, 0x3F800000, 0x007FFFFF), r));
  CHECK_FALSE(vec_estimate_rsqrt_normal(Lanes(0x3F800000, 0xBF800000, 0x3F800000, 0x3F800000), r));
  CHECK_FALSE(vec_estimate_rsqrt_normal(Lanes(0x7F800000, 0x3F800000, 0x3F800000, 0x3F800000), r));
  // Biased exponent 1..252, either sign.
  CHECK(vec_estimate_recip_normal(Lanes(0x00800000, 0x7E7FFFFF, 0xBF800000, 0x80800000), r));
  CHECK_FALSE(vec_estimate_recip_normal(Lanes(0x3F800000, 0x7E800000, 0x3F800000, 0x3F800000), r));
  CHECK_FALSE(vec_estimate_recip_normal(Lanes(0x3F800000, 0x3F800000, 0x80000000, 0x3F800000), r));
}

TEST_CASE("A vector with one special lane falls back per lane", "[ppc][estimate]") {
  const uint32_t in[4] = {0x40800000, 0x7FC00000, 0x00000001, 0xFF800000};
  alignas(16) uint32_t out[4];
  simde_mm_store_ps(reinterpret_cast<float*>(out),
                    vec_estimate<estimate_reciprocal_sqrt>(Lanes(in[0], in[1], in[2], in[3])));
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(out[i] == estimate_reciprocal_sqrt(in[i]));
  simde_mm_store_ps(reinterpret_cast<float*>(out),
                    vec_estimate<estimate_reciprocal>(Lanes(in[0], in[1], in[2], in[3])));
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(out[i] == estimate_reciprocal(in[i]));
}

TEST_CASE("vec_estimate_host computes the host's 1/x and 1/sqrt(x)", "[ppc][estimate]") {
  vec_estimate_host.store(true);
  alignas(16) float out[4];
  simde_mm_store_ps(out, vec_estimate<estimate_reciprocal_sqrt>(simde_mm_set1_ps(4.0f)));
  CHECK(out[0] == 0.5f);
  simde_mm_store_ps(out, vec_estimate<estimate_reciprocal>(simde_mm_set1_ps(4.0f)));
  CHECK(out[0] == 0.25f);
  vec_estimate_host.store(false);
  simde_mm_store_ps(out, vec_estimate<estimate_reciprocal>(simde_mm_set1_ps(4.0f)));
  CHECK(std::bit_cast<uint32_t>(out[0]) == estimate_reciprocal(std::bit_cast<uint32_t>(4.0f)));
}
