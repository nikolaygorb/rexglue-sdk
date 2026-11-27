// Copyright (c) 2026 Tom Clay. BSD 3-Clause; see LICENSE.
// Hardware cases from rexglue/rexglue-sdk 115171b4, adapted to this fork's helpers.
#include <array>
#include <cstdint>
#include <span>

#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/intrinsics.h>
#include "upstream_cases/multiply_add_cases.h"

namespace {
template <auto MultiplyAdd>
void CheckCases(std::span<const MultiplyAddCase> cases) {
  const auto saved = simde_mm_getcsr();
  struct Restore {
    unsigned int csr;
    ~Restore() { simde_mm_setcsr(csr); }
  } restore{saved};
  simde_mm_setcsr(0x1F80 | 0x8040);
  for (size_t first = 0; first < cases.size(); ++first) {
    std::array<uint32_t, 4> a{}, b{}, c{}, result{};
    for (size_t lane = 0; lane < 4; ++lane) {
      const auto& sample = cases[(first + lane * 17) % cases.size()];
      a[lane] = sample.a;
      b[lane] = sample.b;
      c[lane] = sample.c;
    }
    const auto load = [](const auto& words) {
      return simde_mm_castsi128_ps(
          simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(words.data())));
    };
    // Our helpers take the encoding order: multiplier A, multiplier C, addend B.
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(result.data()),
                          simde_mm_castps_si128(MultiplyAdd(load(a), load(b), load(c))));
    for (size_t lane = 0; lane < 4; ++lane) {
      const auto& sample = cases[(first + lane * 17) % cases.size()];
      CAPTURE(first, lane, sample.a, sample.b, sample.c);
      CHECK(result[lane] == sample.out);
    }
  }
}
}  // namespace

TEST_CASE("vmaddfp matches upstream hardware samples", "[ppc][upstream][multiply_add]") {
  CheckCases<rex::ppc::vec_madd>(kMultiplyAddCases);
}

TEST_CASE("vnmsubfp matches upstream hardware samples", "[ppc][upstream][multiply_add]") {
  CheckCases<rex::ppc::vec_nmsub>(kNegativeMultiplySubtractCases);
}
