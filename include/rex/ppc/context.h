/**
 * @file        ppc/context.h
 * @brief       PPC thread context, guest function macros, and interrupt handling
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp PPCContext infrastructure
 */

#pragma once

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

//=============================================================================
// Pack/Unpack Constants (NORMPACKED32 - 2:10:10:10 format)
//=============================================================================

constexpr float kPack2101010_Min10 = std::bit_cast<float>(0x403FFE01u);
constexpr float kPack2101010_Max10 = std::bit_cast<float>(0x404001FFu);
constexpr float kPack2101010_Min2 = std::bit_cast<float>(0x40400000u);
constexpr float kPack2101010_Max2 = std::bit_cast<float>(0x40400003u);

namespace rex::ppc {

//=============================================================================
// General Purpose Register
//=============================================================================

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

//=============================================================================
// Fixed-Point Exception Register (XER)
//=============================================================================

struct XERRegister {
  uint8_t so;
  uint8_t ov;
  uint8_t ca;
};

//=============================================================================
// Condition Register (CR) Field
//=============================================================================

struct CRRegister {
  uint8_t lt;
  uint8_t gt;
  uint8_t eq;
  union {
    uint8_t so;
    uint8_t un;
  };

  inline uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  inline void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  inline void compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  inline void compare(double left, double right) noexcept {
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

//=============================================================================
// Vector Register (128-bit)
//=============================================================================

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

//=============================================================================
// Floating-Point Status and Control Register (FPSCR)
//=============================================================================

constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

struct FPSCRRegister {
  uint32_t csr;

  using Platform = platform::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;

  // Bits the guest owns; the rest is host policy seeded by InitHost.
  static constexpr uint32_t GuestMask = uint32_t(RoundMaskVal) | uint32_t(FlushMask);

  // Exception bits (ReXGlue fork). They live in the host's sticky exception
  // status flags, which the guest's arithmetic sets as the Xenon FPU would set
  // these: OX, UX, ZX, XX, and VX for an invalid operation (reported as
  // VXSOFT, the host doesn't say which kind). FX is reported while any is set;
  // FEX stays 0 (the guest's exception enables are kept masked). Host code
  // running on the thread (the kernel, the runtime) sets them too.
  static constexpr uint32_t kFX = 0x80000000;
  static constexpr uint32_t kVX = 0x20000000;
  static constexpr uint32_t kOX = 0x10000000;
  static constexpr uint32_t kUX = 0x08000000;
  static constexpr uint32_t kZX = 0x04000000;
  static constexpr uint32_t kXX = 0x02000000;
  static constexpr uint32_t kVXSOFT = 0x00000400;
  // VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ, VXVC, VXSOFT, VXSQRT, VXCVI.
  static constexpr uint32_t kVXCauses = 0x01F80000 | 0x00000700;
  // What a guest write can change.
  static constexpr uint32_t kExceptionBits = kOX | kUX | kZX | kXX | kVXCauses;

  // NI, non-IEEE mode (ReXGlue fork): while set, scalar arithmetic flushes
  // denormals as on Xenon, by staying in the host's flush mode (the one VMX
  // code runs in). Read and written with RN.
  static constexpr uint32_t kNI = 0x00000004;
  uint32_t ni = 0;

  // FR, FI and FPRF (C and FPCC) (ReXGlue fork). Kept as the guest wrote
  // them; with the codegen option fpscr_result_bits the arithmetic sets them
  // as Xenon's does (setArithmeticResult), and compares set FPCC.
  static constexpr uint32_t kFR = 0x00040000;
  static constexpr uint32_t kFI = 0x00020000;
  static constexpr uint32_t kFPRF = 0x0001F000;
  static constexpr uint32_t kFPCC = 0x0000F000;
  static constexpr uint32_t kResultBits = kFR | kFI | kFPRF;
  uint32_t result_bits = 0;
  // Whether the host's inexact flag was set before the instruction that
  // beginResult / endInexact measure.
  bool inexact_held = false;

  inline void storeResultBitsFromGuest(uint32_t value, uint32_t mask) noexcept {
    mask &= kResultBits;
    result_bits = (result_bits & ~mask) | (value & mask);
  }

  // Compiler barrier around every access to the host's control and status
  // registers. Recompiled functions take the context as __restrict, so the
  // compiler assumes ldmxcsr / msr fpcr / mrs fpsr cannot touch it: it moved
  // floating-point arithmetic on context registers across a rounding or flush
  // mode change, and reused a result computed under the previous mode (the
  // default floating-point model the titles' generated code builds with does
  // not order arithmetic against the host environment). This asm takes a
  // pointer derived from the context, so every context field is observable to
  // it: arithmetic before it stays before, arithmetic after it reloads its
  // operands. A plain "memory" clobber is not enough, noalias memory is not
  // reachable from it. Values held in C++ locals (the *_as_local options for
  // f and v registers, the codegen's own temporaries) are not covered.
  inline void fenceGuestState() const noexcept {
    __asm__ __volatile__("" : : "r"(this) : "memory");
  }

  // fpscr_result_bits: the host's inexact flag is sticky (XX), so it is moved
  // aside before an instruction and put back after, to see whether that
  // instruction alone was inexact (FI).
  inline void beginResult() noexcept {
    fenceGuestState();
    const uint32_t status = Platform::getstatus();
    inexact_held = (status & Platform::StatusInexact) != 0;
    if (inexact_held) {
      Platform::setstatus(status & ~uint32_t(Platform::StatusInexact));
    }
    fenceGuestState();
  }
  inline bool endInexact() noexcept {
    fenceGuestState();
    const uint32_t status = Platform::getstatus();
    const bool inexact = (status & Platform::StatusInexact) != 0;
    if (inexact_held && !inexact) {
      Platform::setstatus(status | Platform::StatusInexact);
    }
    inexact_held = false;
    fenceGuestState();
    return inexact;
  }
  // Around the host arithmetic that works out FR, which must not leave
  // exception flags of its own.
  inline uint32_t saveStatus() noexcept {
    fenceGuestState();
    return Platform::getstatus();
  }
  inline void restoreStatus(uint32_t status) noexcept {
    fenceGuestState();
    Platform::setstatus(status);
    fenceGuestState();
  }

  // FPRF of a result: C, FL, FG, FE, FU. A single-precision result is
  // denormal below FLT_MIN.
  static inline uint32_t classifyResult(double value, bool single) noexcept {
    if (std::isnan(value)) {
      return 0x11;
    }
    const bool negative = std::signbit(value);
    if (std::isinf(value)) {
      return negative ? 0x09 : 0x05;
    }
    if (value == 0.0) {
      return negative ? 0x12 : 0x02;
    }
    const double magnitude = std::fabs(value);
    if (magnitude < (single ? double(FLT_MIN) : DBL_MIN)) {
      return negative ? 0x18 : 0x14;
    }
    return negative ? 0x08 : 0x04;
  }

  inline bool isRoundingToNearest() noexcept {
    return Platform::HostToGuest[(getcsr() & RoundMaskVal) >> RoundShift] == 0;
  }

  // FR when the instruction was inexact: rounded_away is 1 if the result is
  // larger in magnitude than the exact one, 0 if not, -1 if not known, which
  // the rounding mode answers for directed modes (nearest then gives 0).
  inline bool fractionRounded(double result, bool inexact, int rounded_away) noexcept {
    if (!inexact || std::isnan(result)) {
      return false;
    }
    if (rounded_away >= 0) {
      return rounded_away != 0;
    }
    switch (Platform::HostToGuest[(getcsr() & RoundMaskVal) >> RoundShift]) {
      case 0x02:  // toward +infinity
        return !std::signbit(result);
      case 0x03:  // toward -infinity
        return std::signbit(result);
      default:
        return false;
    }
  }

  inline void setArithmeticResult(double result, bool single, bool inexact,
                                  int rounded_away) noexcept {
    result_bits = (fractionRounded(result, inexact, rounded_away) ? kFR : 0) |
                  (inexact ? kFI : 0) | (classifyResult(result, single) << 12);
  }

  // fctiw, fctid and their z forms: FR and FI; FPRF is undefined, kept.
  inline void setConversionResult(double result, bool inexact, int rounded_away) noexcept {
    result_bits = (result_bits & kFPRF) |
                  (fractionRounded(result, inexact, rounded_away) ? kFR : 0) |
                  (inexact ? kFI : 0);
  }

  // fcmpu, fcmpo: FPCC from the CR field the compare set; C, FR and FI kept.
  inline void setCompareResult(bool lt, bool gt, bool eq, bool un) noexcept {
    result_bits = (result_bits & ~kFPCC) | (uint32_t(lt) << 15) | (uint32_t(gt) << 14) |
                  (uint32_t(eq) << 13) | (uint32_t(un) << 12);
  }

  inline uint32_t loadExceptionsFromHost() const noexcept {
    fenceGuestState();
    const uint32_t status = Platform::getstatus();
    uint32_t bits = 0;
    if (status & Platform::StatusInvalid) bits |= kVX | kVXSOFT;
    if (status & Platform::StatusOverflow) bits |= kOX;
    if (status & Platform::StatusUnderflow) bits |= kUX;
    if (status & Platform::StatusDivideByZero) bits |= kZX;
    if (status & Platform::StatusInexact) bits |= kXX;
    return bits ? bits | kFX : 0;
  }

  // Writes the exception bits of `value` that `mask` selects. FX, FEX and VX
  // are derived, not written. An invalid operation is cleared when the write
  // covers VXSOFT (how it's reported) or every VX cause.
  inline void storeExceptionsFromGuest(uint32_t value, uint32_t mask) noexcept {
    mask &= kExceptionBits;
    if (!mask) {
      return;
    }
    fenceGuestState();
    uint32_t status = Platform::getstatus();
    auto apply = [&](uint32_t bits, uint32_t flag) {
      if (!(mask & bits)) {
        return;
      }
      if (value & mask & bits) {
        status |= flag;
      } else if (bits != kVXCauses || (mask & kVXSOFT) || (mask & kVXCauses) == kVXCauses) {
        status &= ~flag;
      }
    };
    apply(kVXCauses, Platform::StatusInvalid);
    apply(kOX, Platform::StatusOverflow);
    apply(kUX, Platform::StatusUnderflow);
    apply(kZX, Platform::StatusDivideByZero);
    apply(kXX, Platform::StatusInexact);
    Platform::setstatus(status);
    fenceGuestState();
  }

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }
  inline void setcsr(uint32_t csr) noexcept {
    fenceGuestState();
    // Where the status flags share the register (x86 MXCSR), a cached value
    // must not bring back older flags or drop newer ones.
    if constexpr (Platform::CsrStatusMask != 0) {
      csr = (csr & ~Platform::CsrStatusMask) | (Platform::getcsr() & Platform::CsrStatusMask);
    }
    // Keep host FP exceptions masked on every write, not just in InitHost.
    // Recompiled guest code reaches setcsr via storeFromGuest /
    // enableFlushMode / etc., and a csr value that still has the exception
    // bits in their trapping state (e.g. a context whose fpscr.csr field was
    // zero-initialized before InitHost ran) would otherwise re-arm host FP
    // traps and raise SIGFPE on the next FP op. InitHostExceptions applies the
    // per-arch polarity (x86: 1 = masked, ARM: 1 = trap enabled), so it is used
    // here rather than open-coding a mask that is correct on only one of them.
    Platform::InitHostExceptions(csr);
    Platform::setcsr(csr);
    fenceGuestState();
  }

  // Restoring the whole word would unmask every FP exception when csr is 0.
  inline void restoreGuestBits(uint32_t saved) noexcept {
    csr = (getcsr() & ~GuestMask) | (saved & GuestMask);
    setcsr(csr);
  }

  // RN, NI and the result bits.
  inline uint32_t loadFromHost() noexcept {
    csr = getcsr();
    return Platform::HostToGuest[(csr & RoundMaskVal) >> RoundShift] | (ni ? kNI : 0) |
           result_bits;
  }

  // Field 7: RN and NI. The flush mode for NI is applied by the next
  // enableFlushMode or disableFlushMode (the codegen forgets its mode after a
  // write of NI).
  inline void storeFromGuest(uint32_t value) noexcept {
    ni = (value & kNI) ? 1 : 0;
    csr &= ~RoundMaskVal;
    csr |= Platform::GuestToHost[value & kRoundMask];
    setcsr(csr);
  }

  inline void enableFlushModeUnconditional() noexcept {
    csr |= FlushMask;
    setcsr(csr);
  }

  inline void disableFlushModeUnconditional() noexcept {
    if (ni) [[unlikely]] {
      enableFlushModeUnconditional();
      return;
    }
    csr &= ~FlushMask;
    setcsr(csr);
  }

  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if (ni) [[unlikely]] {
      enableFlushMode();
      return;
    }
    if ((csr & FlushMask) != 0) [[unlikely]] {
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }

  inline void InitHost() noexcept {
    csr = getcsr();
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
};

}  // namespace rex::ppc

using PPCRegister = rex::ppc::Register;
using PPCXERRegister = rex::ppc::XERRegister;
using PPCCRRegister = rex::ppc::CRRegister;
using PPCVRegister = rex::ppc::VRegister;
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

//=============================================================================
// PPCContext Structure
//=============================================================================

struct alignas(0x40) PPCContext {
  PPCRegister r3;
  PPCRegister r0;
  PPCRegister r1;
  PPCRegister r2;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;

  uint64_t lr;
  PPCRegister ctr;
  PPCXERRegister xer;
  PPCRegister reserved;
  uint32_t msr = 0x200A000;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;  // VSCR saturation flag (for vector ops)

  /**
   * Last indirect call target address. Set by REX_CALL_INDIRECT_FUNC before
   * dispatch. Used by the invalid-function trap to report the faulting address.
   * Unconditional (not guarded by config flags) because ctr may be optimized
   * to a local variable via REX_CONFIG_CTR_AS_LOCAL.
   */
  uint32_t last_indirect_target = 0;

  PPCRegister f0;
  PPCRegister f1;
  PPCRegister f2;
  PPCRegister f3;
  PPCRegister f4;
  PPCRegister f5;
  PPCRegister f6;
  PPCRegister f7;
  PPCRegister f8;
  PPCRegister f9;
  PPCRegister f10;
  PPCRegister f11;
  PPCRegister f12;
  PPCRegister f13;
  PPCRegister f14;
  PPCRegister f15;
  PPCRegister f16;
  PPCRegister f17;
  PPCRegister f18;
  PPCRegister f19;
  PPCRegister f20;
  PPCRegister f21;
  PPCRegister f22;
  PPCRegister f23;
  PPCRegister f24;
  PPCRegister f25;
  PPCRegister f26;
  PPCRegister f27;
  PPCRegister f28;
  PPCRegister f29;
  PPCRegister f30;
  PPCRegister f31;

  PPCVRegister v0;
  PPCVRegister v1;
  PPCVRegister v2;
  PPCVRegister v3;
  PPCVRegister v4;
  PPCVRegister v5;
  PPCVRegister v6;
  PPCVRegister v7;
  PPCVRegister v8;
  PPCVRegister v9;
  PPCVRegister v10;
  PPCVRegister v11;
  PPCVRegister v12;
  PPCVRegister v13;
  PPCVRegister v14;
  PPCVRegister v15;
  PPCVRegister v16;
  PPCVRegister v17;
  PPCVRegister v18;
  PPCVRegister v19;
  PPCVRegister v20;
  PPCVRegister v21;
  PPCVRegister v22;
  PPCVRegister v23;
  PPCVRegister v24;
  PPCVRegister v25;
  PPCVRegister v26;
  PPCVRegister v27;
  PPCVRegister v28;
  PPCVRegister v29;
  PPCVRegister v30;
  PPCVRegister v31;
  PPCVRegister v32;
  PPCVRegister v33;
  PPCVRegister v34;
  PPCVRegister v35;
  PPCVRegister v36;
  PPCVRegister v37;
  PPCVRegister v38;
  PPCVRegister v39;
  PPCVRegister v40;
  PPCVRegister v41;
  PPCVRegister v42;
  PPCVRegister v43;
  PPCVRegister v44;
  PPCVRegister v45;
  PPCVRegister v46;
  PPCVRegister v47;
  PPCVRegister v48;
  PPCVRegister v49;
  PPCVRegister v50;
  PPCVRegister v51;
  PPCVRegister v52;
  PPCVRegister v53;
  PPCVRegister v54;
  PPCVRegister v55;
  PPCVRegister v56;
  PPCVRegister v57;
  PPCVRegister v58;
  PPCVRegister v59;
  PPCVRegister v60;
  PPCVRegister v61;
  PPCVRegister v62;
  PPCVRegister v63;
  PPCVRegister v64;
  PPCVRegister v65;
  PPCVRegister v66;
  PPCVRegister v67;
  PPCVRegister v68;
  PPCVRegister v69;
  PPCVRegister v70;
  PPCVRegister v71;
  PPCVRegister v72;
  PPCVRegister v73;
  PPCVRegister v74;
  PPCVRegister v75;
  PPCVRegister v76;
  PPCVRegister v77;
  PPCVRegister v78;
  PPCVRegister v79;
  PPCVRegister v80;
  PPCVRegister v81;
  PPCVRegister v82;
  PPCVRegister v83;
  PPCVRegister v84;
  PPCVRegister v85;
  PPCVRegister v86;
  PPCVRegister v87;
  PPCVRegister v88;
  PPCVRegister v89;
  PPCVRegister v90;
  PPCVRegister v91;
  PPCVRegister v92;
  PPCVRegister v93;
  PPCVRegister v94;
  PPCVRegister v95;
  PPCVRegister v96;
  PPCVRegister v97;
  PPCVRegister v98;
  PPCVRegister v99;
  PPCVRegister v100;
  PPCVRegister v101;
  PPCVRegister v102;
  PPCVRegister v103;
  PPCVRegister v104;
  PPCVRegister v105;
  PPCVRegister v106;
  PPCVRegister v107;
  PPCVRegister v108;
  PPCVRegister v109;
  PPCVRegister v110;
  PPCVRegister v111;
  PPCVRegister v112;
  PPCVRegister v113;
  PPCVRegister v114;
  PPCVRegister v115;
  PPCVRegister v116;
  PPCVRegister v117;
  PPCVRegister v118;
  PPCVRegister v119;
  PPCVRegister v120;
  PPCVRegister v121;
  PPCVRegister v122;
  PPCVRegister v123;
  PPCVRegister v124;
  PPCVRegister v125;
  PPCVRegister v126;
  PPCVRegister v127;

  // VRSAVE (SPR 256): a software-only mask of the vector registers in use.
  // Kept last so that adding it moved no other field.
  uint32_t vrsave = 0;

  //--- Non-volatile register save/restore --------
  // Layout: r14-r31 (144) | f14-f31 (144) | v14-v31 (288) | v64-v127 (1024)
  //       | cr2-cr4 (12) | fpscr (16).  Total: 1628 bytes.
  static constexpr size_t kNonVolatileSaveSize =
      18 * sizeof(PPCRegister) + 18 * sizeof(PPCRegister) + 18 * sizeof(PPCVRegister) +
      64 * sizeof(PPCVRegister) + 3 * sizeof(PPCCRRegister) + sizeof(PPCFPSCRRegister);

  inline void SaveNonVolatiles(uint8_t* dst) const {
    std::memcpy(dst, &r14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &f14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &v14, 18 * sizeof(PPCVRegister));
    dst += 18 * sizeof(PPCVRegister);
    std::memcpy(dst, &v64, 64 * sizeof(PPCVRegister));
    dst += 64 * sizeof(PPCVRegister);
    std::memcpy(dst, &cr2, 3 * sizeof(PPCCRRegister));
    dst += 3 * sizeof(PPCCRRegister);
    std::memcpy(dst, &fpscr, sizeof(PPCFPSCRRegister));
  }

  inline void RestoreNonVolatiles(const uint8_t* src) {
    std::memcpy(&r14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&f14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&v14, src, 18 * sizeof(PPCVRegister));
    src += 18 * sizeof(PPCVRegister);
    std::memcpy(&v64, src, 64 * sizeof(PPCVRegister));
    src += 64 * sizeof(PPCVRegister);
    std::memcpy(&cr2, src, 3 * sizeof(PPCCRRegister));
    src += 3 * sizeof(PPCCRRegister);
    PPCFPSCRRegister saved_fpscr;
    std::memcpy(&saved_fpscr, src, sizeof(PPCFPSCRRegister));
    fpscr.ni = saved_fpscr.ni;
    fpscr.result_bits = saved_fpscr.result_bits;
    fpscr.restoreGuestBits(saved_fpscr.csr);
  }
};
