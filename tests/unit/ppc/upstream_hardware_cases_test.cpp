/**
 * @file        ppc/upstream_hardware_cases_test.cpp
 * @brief       Our estimates and dot products against upstream ReXGlue's
 *              sampled hardware results
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// The case files in upstream_cases/ are upstream rexglue/rexglue-sdk's
// (development 351bc94d, tests/unit/ppc, Tom Clay, BSD 3-Clause), sampled on
// a console. Our estimates come from xenia-canary's tables (rex/ppc/
// estimates.h): two independent sources checked against each other. Our dot
// products (vec_msum3 / vec_msum4) are upstream's fixed point model since these
// cases failed Xenia's float64 order in about three of four; the fast form
// (vec_msum3_fast, codegen fast_dot_products) is not checked here. Vector
// elements are in guest order: element i is host lane 3 - i.
//
// The VSCR[NJ] clear estimate cases are [!mayfail]: our vector estimates always
// treat a denormal input as zero (NJ set, which titles run with). They report
// how far that is from the hardware without failing the run. The dot products
// take flush as a parameter, so their NJ clear cases must pass.

#include <array>
#include <bit>
#include <cstdint>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#include "upstream_cases/dot_product_cases.h"
#include "upstream_cases/dot_product_nj0_cases.h"
#include "upstream_cases/estimate_cases.h"
#include "upstream_cases/estimate_nj0_cases.h"

using namespace rex::ppc;

namespace {

using Platform = PPCFPSCRRegister::Platform;

// The host mode of vector code (flush, as with VSCR[NJ] set) or of scalar
// code (no flush) for the scope, put back after.
struct HostMode {
  uint32_t saved = Platform::getcsr();
  PPCContext ctx{};
  explicit HostMode(bool flush) {
    ctx.fpscr.InitHost();
    if (flush) {
      ctx.fpscr.enableFlushModeUnconditional();
    } else {
      ctx.fpscr.disableFlushModeUnconditional();
    }
  }
  ~HostMode() { Platform::setcsr(saved); }
};

template <uint32_t (*Estimate)(uint32_t)>
void CheckLanes(std::span<const EstimateCase32> cases) {
  for (const EstimateCase32& expected : cases) {
    CAPTURE(expected.in);
    CHECK(Estimate(expected.in) == expected.out);
  }
}

template <double (*Estimate)(double)>
void CheckScalar(std::span<const EstimateCase64> cases) {
  HostMode mode(false);
  for (const EstimateCase64& expected : cases) {
    CAPTURE(expected.in);
    CHECK(std::bit_cast<uint64_t>(Estimate(std::bit_cast<double>(expected.in))) == expected.out);
  }
}

simde__m128 LoadElements(const std::array<u32, 4>& elements) {
  alignas(16) std::array<uint32_t, 4> host;
  for (size_t i = 0; i < 4; ++i) {
    host[3 - i] = elements[i];
  }
  return simde_mm_castsi128_ps(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(host.data())));
}

std::array<uint32_t, 4> StoreElements(simde__m128 v) {
  alignas(16) std::array<uint32_t, 4> host;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host.data()), simde_mm_castps_si128(v));
  return {host[3], host[2], host[1], host[0]};
}

template <typename Dot>
void CheckDot(Dot dot, std::span<const DotProductCase> cases) {
  for (const DotProductCase& expected : cases) {
    CAPTURE(expected.a[0], expected.a[1], expected.a[2], expected.a[3], expected.b[0],
            expected.b[1], expected.b[2], expected.b[3]);
    const simde__m128 r = dot(LoadElements(expected.a), LoadElements(expected.b));
    for (const uint32_t lane : StoreElements(r)) {
      CHECK(lane == expected.out);
    }
  }
}

}  // namespace

TEST_CASE("upstream cases: vrefp", "[ppc][estimate][upstream]") {
  CheckLanes<estimate_reciprocal>(kReciprocalEstimateCases);
}

TEST_CASE("upstream cases: vrsqrtefp", "[ppc][estimate][upstream]") {
  CheckLanes<estimate_reciprocal_sqrt>(kReciprocalSqrtEstimateCases);
}

TEST_CASE("upstream cases: vlogefp", "[ppc][estimate][upstream]") {
  HostMode mode(true);
  CheckLanes<estimate_log2>(kLog2EstimateCases);
}

TEST_CASE("upstream cases: vexptefp", "[ppc][estimate][upstream]") {
  CheckLanes<estimate_pow2>(kExp2EstimateCases);
}

TEST_CASE("upstream cases: fres", "[ppc][estimate][upstream]") {
  CheckScalar<estimate_reciprocal_single>(kScalarReciprocalEstimateCases);
}

TEST_CASE("upstream cases: frsqrte", "[ppc][estimate][upstream]") {
  CheckScalar<estimate_reciprocal_sqrt_double>(kScalarReciprocalSqrtEstimateCases);
}

TEST_CASE("upstream cases: vector estimates with VSCR[NJ] clear",
          "[ppc][estimate][upstream][!mayfail]") {
  for (const EstimateNJ0Case& expected : kEstimateNJ0Cases) {
    uint32_t (*estimate)(uint32_t) = expected.family == 0   ? estimate_reciprocal
                                     : expected.family == 1 ? estimate_reciprocal_sqrt
                                     : expected.family == 2 ? estimate_log2
                                                            : estimate_pow2;
    for (size_t i = 0; i < 4; ++i) {
      CAPTURE(expected.family, i, expected.in[i]);
      CHECK(estimate(expected.in[i]) == expected.out[i]);
    }
  }
}

TEST_CASE("upstream cases: vmsum3fp128", "[ppc][dot_product][upstream]") {
  HostMode mode(true);
  CheckDot([](simde__m128 a, simde__m128 b) { return vec_msum3(a, b); }, kDotProduct3Cases);
}

TEST_CASE("upstream cases: vmsum4fp128", "[ppc][dot_product][upstream]") {
  HostMode mode(true);
  CheckDot([](simde__m128 a, simde__m128 b) { return vec_msum4(a, b); }, kDotProduct4Cases);
}

TEST_CASE("upstream cases: dot products with VSCR[NJ] clear", "[ppc][dot_product][upstream]") {
  HostMode mode(false);
  for (const DotProductNJ0Case& expected : kDotProductNJ0Cases) {
    CAPTURE(expected.terms, expected.a[0], expected.a[1], expected.a[2], expected.a[3],
            expected.b[0], expected.b[1], expected.b[2], expected.b[3]);
    const simde__m128 r = expected.terms == 3
                              ? vec_msum3(LoadElements(expected.a), LoadElements(expected.b), false)
                              : vec_msum4(LoadElements(expected.a), LoadElements(expected.b), false);
    for (const uint32_t lane : StoreElements(r)) {
      CHECK(lane == expected.out);
    }
  }
}
