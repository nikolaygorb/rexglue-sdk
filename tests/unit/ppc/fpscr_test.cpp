/**
 * @file        ppc/fpscr_test.cpp
 * @brief       Tests for PPCFPSCRRegister host control-word handling
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cfloat>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

using namespace rex::ppc;

namespace {

using Platform = PPCFPSCRRegister::Platform;

#if defined(__x86_64__) || defined(_M_X64)
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr & ~Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == Platform::ExceptionMask;
}
#else
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr | Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == 0;
}
#endif

struct HostCsrGuard {
  uint32_t saved = Platform::getcsr();
  ~HostCsrGuard() { Platform::setcsr(saved); }
};

double Div(double a, double b) {
  volatile double x = a, y = b;
  return x / y;
}

float Mul(float a, float b) {
  volatile float x = a, y = b;
  return x * y;
}

// The shape of recompiled code: guest registers in a __restrict context, the
// rounding mode changed through ctx.fpscr in between, the default
// floating-point model (tests build as the titles' generated code does). f1 =
// 1, f2 = 3, f3 = -1; quotients in f4..f11. Without the barrier in the fpscr
// helpers the compiler computed each quotient once, under the first mode.
[[gnu::noinline]] void DivideInEveryMode(PPCContext& __restrict ctx) {
  ctx.fpscr.storeFromGuest(kRoundNearest);
  ctx.f4.f64 = ctx.f1.f64 / ctx.f2.f64;
  ctx.f5.f64 = ctx.f3.f64 / ctx.f2.f64;
  ctx.fpscr.storeFromGuest(kRoundUp);
  ctx.f6.f64 = ctx.f1.f64 / ctx.f2.f64;
  ctx.f7.f64 = ctx.f3.f64 / ctx.f2.f64;
  ctx.fpscr.storeFromGuest(kRoundDown);
  ctx.f8.f64 = ctx.f1.f64 / ctx.f2.f64;
  ctx.f9.f64 = ctx.f3.f64 / ctx.f2.f64;
  ctx.fpscr.storeFromGuest(kRoundTowardZero);
  ctx.f10.f64 = ctx.f1.f64 / ctx.f2.f64;
  ctx.f11.f64 = ctx.f3.f64 / ctx.f2.f64;
  ctx.fpscr.storeFromGuest(kRoundNearest);
}

}  // namespace

TEST_CASE("fpscr: guest rounding modes round host arithmetic correctly", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.f1.f64 = 1.0;
  ctx.f2.f64 = 3.0;
  ctx.f3.f64 = -1.0;

  DivideInEveryMode(ctx);

  const double rn_pos = ctx.f4.f64, rn_neg = ctx.f5.f64;
  const double up_pos = ctx.f6.f64, up_neg = ctx.f7.f64;
  const double dn_pos = ctx.f8.f64, dn_neg = ctx.f9.f64;
  const double tz_pos = ctx.f10.f64, tz_neg = ctx.f11.f64;

  CHECK(up_pos > dn_pos);
  CHECK(up_neg > dn_neg);
  CHECK(tz_pos == dn_pos);
  CHECK(tz_neg == up_neg);
  CHECK(rn_pos == dn_pos);
  CHECK(rn_neg == up_neg);
}

TEST_CASE("fpscr: the guest reads back the rounding mode it wrote", "[ppc][fpscr]") {
  // The host encodings differ (x86 MXCSR: 01 down, 10 up; ARM FPCR: 01 up,
  // 10 down), so this catches a table written for the other one (issue #477).
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  for (uint32_t mode : {kRoundNearest, kRoundTowardZero, kRoundUp, kRoundDown}) {
    ctx.fpscr.storeFromGuest(mode);
    CHECK(ctx.fpscr.loadFromHost() == mode);
  }
  ctx.fpscr.storeFromGuest(kRoundNearest);
}

TEST_CASE("fpscr: flush mode toggles denormal flushing", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();

  const float tiny = FLT_MIN / 4.0f;

  ctx.fpscr.disableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) != 0.0f);

  ctx.fpscr.enableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) == 0.0f);
  CHECK(Mul(FLT_MIN, 0.5f) == 0.0f);

  ctx.fpscr.disableFlushMode();
  CHECK(Mul(tiny, 1.0f) != 0.0f);
}

TEST_CASE("fpscr: setcsr keeps host traps masked and preserves guest bits", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.fpscr.storeFromGuest(kRoundTowardZero);
  ctx.fpscr.enableFlushModeUnconditional();

  const uint32_t before = ctx.fpscr.getcsr();
  ctx.fpscr.setcsr(ArmTraps(before));
  const uint32_t after = ctx.fpscr.getcsr();

  CHECK(TrapsMasked(after));
  CHECK((after & PPCFPSCRRegister::GuestMask) == (before & PPCFPSCRRegister::GuestMask));
  CHECK(ctx.fpscr.loadFromHost() == kRoundTowardZero);

  CHECK(Div(0.0, 0.0) != Div(0.0, 0.0));
  CHECK(Div(1.0, 0.0) > DBL_MAX);
}

TEST_CASE("fpscr: storeFromGuest on a zeroed csr does not arm traps", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.csr = ArmTraps(0);

  ctx.fpscr.storeFromGuest(kRoundUp);

  CHECK(TrapsMasked(ctx.fpscr.getcsr()));
  CHECK(Div(0.0, 0.0) != Div(0.0, 0.0));
}

TEST_CASE("fpscr: NI keeps scalar code in the flush mode", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  const float tiny = FLT_MIN / 4.0f;

  ctx.fpscr.storeFromGuest(PPCFPSCRRegister::kNI);
  CHECK(ctx.fpscr.loadFromHost() == PPCFPSCRRegister::kNI);
  ctx.fpscr.disableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) == 0.0f);
  ctx.fpscr.disableFlushMode();
  CHECK(Mul(tiny, 1.0f) == 0.0f);

  ctx.fpscr.storeFromGuest(0);
  ctx.fpscr.disableFlushMode();
  CHECK(Mul(tiny, 1.0f) != 0.0f);
}

TEST_CASE("fpscr: FPRF classes", "[ppc][fpscr]") {
  CHECK(PPCFPSCRRegister::classifyResult(1.0, false) == 0x04);
  CHECK(PPCFPSCRRegister::classifyResult(-1.0, false) == 0x08);
  CHECK(PPCFPSCRRegister::classifyResult(0.0, false) == 0x02);
  CHECK(PPCFPSCRRegister::classifyResult(-0.0, false) == 0x12);
  CHECK(PPCFPSCRRegister::classifyResult(DBL_MIN / 2.0, false) == 0x14);
  CHECK(PPCFPSCRRegister::classifyResult(-DBL_MIN / 2.0, false) == 0x18);
  CHECK(PPCFPSCRRegister::classifyResult(double(FLT_MIN) / 2.0, true) == 0x14);
  CHECK(PPCFPSCRRegister::classifyResult(double(FLT_MIN) / 2.0, false) == 0x04);
  CHECK(PPCFPSCRRegister::classifyResult(HUGE_VAL, false) == 0x05);
  CHECK(PPCFPSCRRegister::classifyResult(-HUGE_VAL, false) == 0x09);
  CHECK(PPCFPSCRRegister::classifyResult(Div(0.0, 0.0), false) == 0x11);
}

TEST_CASE("fpscr: FI of one instruction, XX kept", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kExceptionBits);

  ctx.fpscr.beginResult();
  const double third = Div(1.0, 3.0);
  CHECK(ctx.fpscr.endInexact());
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kXX) != 0);

  // Exact now, while XX stays set from before.
  ctx.fpscr.beginResult();
  CHECK(Div(1.0, 4.0) == 0.25);
  CHECK(!ctx.fpscr.endInexact());
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kXX) != 0);

  ctx.fpscr.setArithmeticResult(third, false, true, fp_rounded_away_quotient(third, 1.0, 3.0));
  // 1/3 rounds down to nearest.
  CHECK(ctx.fpscr.result_bits == (PPCFPSCRRegister::kFI | (0x04 << 12)));
}

TEST_CASE("fpscr: FR from the exact error", "[ppc][fpscr]") {
  // 1/10 rounds up to nearest, 1/3 down.
  CHECK(fp_rounded_away_quotient(Div(1.0, 10.0), 1.0, 10.0) == 1);
  CHECK(fp_rounded_away_quotient(Div(1.0, 3.0), 1.0, 3.0) == 0);
  CHECK(fp_rounded_away_quotient(Div(-1.0, 10.0), -1.0, 10.0) == 1);
  CHECK(fp_rounded_away_quotient(Div(1.0, -10.0), 1.0, -10.0) == 1);
  // 1 + 2^-53 rounds to 1 (ties to even): down; 1 + 3 * 2^-54 rounds up.
  CHECK(fp_rounded_away_sum(1.0, 1.0, 0x1p-53) == 0);
  CHECK(fp_rounded_away_sum(1.0 + 0x1p-52, 1.0, 0x3p-54) == 1);
  CHECK(fp_rounded_away_product(1.0, 1.0 + 0x1p-30, 1.0 - 0x1p-30) == 1);
  // sqrt(2) = 1.41421356237309504..., the double 1.41421356237309514...
  CHECK(fp_rounded_away_root(std::sqrt(2.0), 2.0) == 1);
  CHECK(fp_rounded_away_from(double(float(0.1)), 0.1) == 1);
  CHECK(fp_rounded_away_from_integer(9223372036854775808.0, INT64_MAX) == 1);
  // 2^63 - 1023 is a double 2^63 - 1024.
  CHECK(fp_rounded_away_from_integer(double(INT64_C(0x7FFFFFFFFFFFFC01)),
                                     INT64_C(0x7FFFFFFFFFFFFC01)) == 0);
}

TEST_CASE("fpscr: result bits read back as written", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.fpscr.storeResultBitsFromGuest(0xFFFFFFFF, 0x000FF000);
  CHECK(ctx.fpscr.loadFromHost() == PPCFPSCRRegister::kResultBits);
  ctx.fpscr.setCompareResult(true, false, false, false);
  CHECK((ctx.fpscr.result_bits & PPCFPSCRRegister::kFPCC) == 0x8000);
  CHECK((ctx.fpscr.result_bits & ~PPCFPSCRRegister::kFPCC) ==
        (PPCFPSCRRegister::kFR | PPCFPSCRRegister::kFI | 0x10000));
}
