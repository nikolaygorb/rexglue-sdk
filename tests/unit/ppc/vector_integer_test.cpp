/**
 * @file        ppc/vector_integer_test.cpp
 * @brief       Tests for the vsubsws, vpkuhus, vpkuwus and vrlw helpers against scalar references
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <bit>
#include <climits>
#include <cstdint>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/intrinsics.h>

using namespace rex::ppc;

namespace {

constexpr int kRandomVectors = 200000;

constexpr std::array<uint32_t, 16> kEdges = {
    0x00000000, 0x00000001, 0x0000FFFF, 0x00010000, 0x000000FF, 0x00000100, 0x00007FFF, 0x00008000,
    0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFF0000, 0xFFFF8000, 0xFFFFFFFE, 0xFFFFFFFF,
};

simde__m128i Load(const std::array<uint32_t, 4>& v) {
  return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(v.data()));
}

template <typename T, size_t N>
std::array<T, N> Store(simde__m128i v) {
  std::array<T, N> out;
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out.data()), v);
  return out;
}

// Random vectors with edge values mixed in, so saturation and wrap-around are hit often.
template <typename Check>
void ForVectors(Check check) {
  std::mt19937 rng(1234);
  auto word = [&] { return (rng() & 3) == 0 ? kEdges[rng() % kEdges.size()] : uint32_t(rng()); };
  for (int n = 0; n < kRandomVectors; ++n) {
    const std::array<uint32_t, 4> a = {word(), word(), word(), word()};
    const std::array<uint32_t, 4> b = {word(), word(), word(), word()};
    check(a, b);
  }
  for (uint32_t x : kEdges)
    for (uint32_t y : kEdges)
      check({x, y, x, y}, {y, x, y, x});
}

}  // namespace

TEST_CASE("simde_mm_subs_epi32 saturates like vsubsws", "[ppc][vector]") {
  uint64_t mismatches = 0;
  ForVectors([&](const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) {
    const auto r = Store<int32_t, 4>(simde_mm_subs_epi32(Load(a), Load(b)));
    for (int i = 0; i < 4; ++i) {
      const int64_t d = int64_t(int32_t(a[i])) - int64_t(int32_t(b[i]));
      const int32_t expected = d > INT_MAX ? INT_MAX : d < INT_MIN ? INT_MIN : int32_t(d);
      mismatches += r[i] != expected;
    }
  });
  CHECK(mismatches == 0);
}

TEST_CASE("simde_mm_packus_epu16 saturates unsigned halfwords like vpkuhus", "[ppc][vector]") {
  uint64_t mismatches = 0;
  ForVectors([&](const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) {
    const auto lo = std::bit_cast<std::array<uint16_t, 8>>(a);
    const auto hi = std::bit_cast<std::array<uint16_t, 8>>(b);
    const auto r = Store<uint8_t, 16>(simde_mm_packus_epu16(Load(a), Load(b)));
    for (int i = 0; i < 8; ++i) {
      mismatches += r[i] != (lo[i] > 0xFF ? 0xFF : lo[i]);
      mismatches += r[8 + i] != (hi[i] > 0xFF ? 0xFF : hi[i]);
    }
  });
  CHECK(mismatches == 0);
}

TEST_CASE("simde_mm_packus_epu32 saturates unsigned words like vpkuwus", "[ppc][vector]") {
  uint64_t mismatches = 0;
  ForVectors([&](const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) {
    const auto r = Store<uint16_t, 8>(simde_mm_packus_epu32(Load(a), Load(b)));
    for (int i = 0; i < 4; ++i) {
      mismatches += r[i] != (a[i] > 0xFFFF ? 0xFFFF : a[i]);
      mismatches += r[4 + i] != (b[i] > 0xFFFF ? 0xFFFF : b[i]);
    }
  });
  CHECK(mismatches == 0);
}

TEST_CASE("simde_mm_rolv_epi32 rotates each word like vrlw", "[ppc][vector]") {
  uint64_t mismatches = 0;
  ForVectors([&](const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) {
    const auto r = Store<uint32_t, 4>(simde_mm_rolv_epi32(Load(a), Load(b)));
    for (int i = 0; i < 4; ++i)
      mismatches += r[i] != std::rotl(a[i], int(b[i] & 31));
  });
  // Every count in every lane.
  for (uint32_t count = 0; count < 64; ++count) {
    const std::array<uint32_t, 4> a = {0x80000001, 0x12345678, 0xFFFF0000, 0x0000FFFF};
    const std::array<uint32_t, 4> b = {count, count + 1, count + 2, count + 3};
    const auto r = Store<uint32_t, 4>(simde_mm_rolv_epi32(Load(a), Load(b)));
    for (int i = 0; i < 4; ++i)
      mismatches += r[i] != std::rotl(a[i], int(b[i] & 31));
  }
  CHECK(mismatches == 0);
}
