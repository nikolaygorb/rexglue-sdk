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
#include <limits>

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

// Xenon does not implement NI: it reads back 0 and scalar code keeps
// denormals (xenia-edge c5ae0a002, the console's mffs / mtfsfi captures).
TEST_CASE("fpscr: NI is dropped and scalar code keeps denormals", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  const float tiny = FLT_MIN / 4.0f;

  ctx.fpscr.storeFromGuest(PPCFPSCRRegister::kNI | kRoundUp);
  CHECK(ctx.fpscr.loadFromHost() == kRoundUp);
  ctx.fpscr.disableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) != 0.0f);
  ctx.fpscr.enableFlushModeUnconditional();
  ctx.fpscr.disableFlushMode();
  CHECK(Mul(tiny, 1.0f) != 0.0f);
  ctx.fpscr.storeFromGuest(0);
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

// A value the compiler can't fold the conversions and roots of.
static double Opaque(double x) {
  volatile double v = x;
  return v;
}

TEST_CASE("fpscr: FX is sticky, set by a new exception", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == 0);

  // An inexact quotient raises XX, and FX with it.
  CHECK(Div(1.0, 3.0) != 0.0);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kXX));

  // FX cleared by the guest stays clear while XX was already set.
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX);
  CHECK(Div(1.0, 3.0) != 0.0);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == PPCFPSCRRegister::kXX);

  // Clearing XX keeps FX; a write that sets OX sets FX.
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kXX);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == 0);
  ctx.fpscr.storeExceptionsFromGuest(PPCFPSCRRegister::kOX, PPCFPSCRRegister::kOX);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kOX));
  // A write that covers FX sets it as written.
  ctx.fpscr.storeExceptionsFromGuest(PPCFPSCRRegister::kXX, 0xF2000000);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == PPCFPSCRRegister::kXX);
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
}

TEST_CASE("fpscr: VX causes by name, FEX from the enables", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  constexpr uint32_t kVXSNAN = 0x01000000;
  constexpr uint32_t kVXCVI = 0x00000100;
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);

  ctx.fpscr.storeExceptionsFromGuest(kVXSNAN | kVXCVI, kVXSNAN | kVXCVI);
  CHECK(ctx.fpscr.loadExceptionsFromHost() ==
        (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kVX | kVXSNAN | kVXCVI));
  CHECK((Platform::getstatus() & Platform::StatusInvalid) != 0);
  // Clearing one cause keeps VX; clearing the last clears it and the host flag.
  ctx.fpscr.storeExceptionsFromGuest(0, kVXSNAN);
  CHECK(ctx.fpscr.loadExceptionsFromHost() ==
        (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kVX | kVXCVI));
  ctx.fpscr.storeExceptionsFromGuest(0, kVXCVI);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == PPCFPSCRRegister::kFX);
  CHECK((Platform::getstatus() & Platform::StatusInvalid) == 0);

  // An invalid operation of the host arithmetic reads as VXSOFT.
  CHECK(std::isnan(Div(0.0, 0.0)));
  CHECK(ctx.fpscr.loadExceptionsFromHost() ==
        (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kVX | PPCFPSCRRegister::kVXSOFT));

  // VX with VE, XX with XE: FEX.
  ctx.fpscr.storeEnablesFromGuest(PPCFPSCRRegister::kVE, PPCFPSCRRegister::kEnables);
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kFEX) != 0);
  CHECK((ctx.fpscr.loadFromHost() & PPCFPSCRRegister::kEnables) == PPCFPSCRRegister::kVE);
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kVXSOFT);
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kFEX) == 0);
  ctx.fpscr.storeEnablesFromGuest(0, PPCFPSCRRegister::kEnables);
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
}

TEST_CASE("fpscr: scalar arithmetic results and flags", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  const double qnan_negative = std::bit_cast<double>(0xFFF8000000000000ull);
  const double snan = std::bit_cast<double>(0x7FF4000000000000ull);

  // The default QNaN is positive; a NaN operand is the result, quieted.
  CHECK(std::bit_cast<uint64_t>(fp_nan_result(qnan_negative, 0.0, 0.0)) == 0x7FF8000000000000ull);
  CHECK(std::bit_cast<uint64_t>(fp_nan_result(qnan_negative, 1.0, snan)) == 0x7FFC000000000000ull);
  CHECK(fp_nan_result(2.0, 1.0, 1.0) == 2.0);

  // fsel: the bits of A, no flag for an SNaN.
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
  CHECK(fp_select_nonnegative(0.0));
  CHECK(fp_select_nonnegative(-0.0));
  CHECK(fp_select_nonnegative(HUGE_VAL));
  CHECK(!fp_select_nonnegative(-1.0));
  CHECK(!fp_select_nonnegative(-HUGE_VAL));
  CHECK(!fp_select_nonnegative(snan));
  CHECK(!fp_select_nonnegative(qnan_negative));
  CHECK(ctx.fpscr.loadExceptionsFromHost() == 0);

  // fctiw: sign-extended word, saturated, invalid for a NaN or infinity.
  CHECK(fp_to_int32(Opaque(-1.0)) == -1);
  CHECK(fp_to_int32(Opaque(HUGE_VAL)) == INT32_MAX);
  CHECK(ctx.fpscr.loadExceptionsFromHost() ==
        (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kVX | PPCFPSCRRegister::kVXSOFT));
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
  CHECK(std::bit_cast<uint64_t>(fp_to_int32_truncate(Opaque(qnan_negative))) ==
        0xFFFFFFFF80000000ull);
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kVX) != 0);
  CHECK(fp_to_int64(Opaque(9.3e18)) == INT64_MAX);
  CHECK(fp_to_int64_truncate(Opaque(-2.5)) == -2);

  // 0 x infinity with a QNaN addend is invalid.
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
  const double qnan = std::bit_cast<double>(0x7FF8000000000001ull);
  const double r = fp_madd_result(qnan, 0.0, qnan, HUGE_VAL, false);
  CHECK(std::bit_cast<uint64_t>(r) == 0x7FF8000000000001ull);
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kVX) != 0);

  // The host square root raises invalid for an SNaN.
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
  CHECK(std::isnan(fp_sqrt(Opaque(snan))));
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kVX) != 0);
  ctx.fpscr.storeExceptionsFromGuest(0, PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits);
}

TEST_CASE("fpscr: fres and frsqrte estimates and flags", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  constexpr uint32_t kAll = PPCFPSCRRegister::kFX | PPCFPSCRRegister::kExceptionBits;
  ctx.fpscr.storeExceptionsFromGuest(0, kAll);

  // In range: the table's estimate, no flag (not even XX).
  CHECK(std::bit_cast<uint64_t>(fp_fres(Opaque(1.0))) == 0x3FEFE00000000000ull);
  CHECK(std::bit_cast<uint64_t>(fp_frsqrte(Opaque(1.0))) == 0x3FEF100000000000ull);
  // 2^31 - 1: a fraction whose index arithmetic would be inexact.
  CHECK(std::bit_cast<uint64_t>(fp_fres(Opaque(2147483647.0))) == 0x3E00000000000000ull);
  CHECK(ctx.fpscr.loadExceptionsFromHost() == 0);

  // A zero: ZX.
  CHECK(std::isinf(fp_fres(Opaque(0.0))));
  CHECK(ctx.fpscr.loadExceptionsFromHost() == (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kZX));
  ctx.fpscr.storeExceptionsFromGuest(0, kAll);

  // fres of a denormal overflows: OX and XX.
  CHECK(std::isinf(fp_fres(Opaque(std::bit_cast<double>(0x0000000000000001ull)))));
  CHECK(ctx.fpscr.loadExceptionsFromHost() ==
        (PPCFPSCRRegister::kFX | PPCFPSCRRegister::kOX | PPCFPSCRRegister::kXX));
  ctx.fpscr.storeExceptionsFromGuest(0, kAll);

  // frsqrte of a negative value: VX.
  CHECK(std::isnan(fp_frsqrte(Opaque(-1.0))));
  CHECK((ctx.fpscr.loadExceptionsFromHost() & PPCFPSCRRegister::kVX) != 0);
  ctx.fpscr.storeExceptionsFromGuest(0, kAll);

  // The vector estimates: vrefp(1) and vexptefp(2) = 4.
  CHECK(estimate_reciprocal(0x3F800000u) == 0x3F7FF800u);
  CHECK(estimate_pow2(0x40000000u) == 0x40800000u);
}

TEST_CASE("fpscr: a single-precision operation sees a denormal operand", "[ppc][fpscr]") {
  using rex::ppc::fp_single_denormal_operand;
  const double denormal = std::numeric_limits<double>::denorm_min() * 3;
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(fp_single_denormal_operand(denormal));
  CHECK(fp_single_denormal_operand(-denormal, 1.0));
  CHECK(fp_single_denormal_operand(1.0, 2.0, denormal));
  CHECK_FALSE(fp_single_denormal_operand(0.0, -0.0));
  CHECK_FALSE(fp_single_denormal_operand(DBL_MIN, 1.0));
  CHECK_FALSE(fp_single_denormal_operand(1.0, 2.0, 3.0));
  // A NaN or an infinity takes precedence.
  CHECK_FALSE(fp_single_denormal_operand(denormal, inf));
  CHECK_FALSE(fp_single_denormal_operand(nan, 1.0, denormal));
  CHECK_FALSE(fp_single_denormal_operand(denormal, 1.0, -inf));
}
