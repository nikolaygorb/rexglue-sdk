/**
 * @file        platform/fpscr.h
 * @brief       Platform-specific FPSCR constants and intrinsics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

#include <rex/types.h>

#include <simde/x86/sse.h>

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::platform {

// simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)

struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK;
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr u32 ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                       (1 << 8) |   // DM - Denormal operand
                                       (1 << 9) |   // ZM - Zero divide
                                       (1 << 10) |  // OM - Overflow
                                       (1 << 11) |  // UM - Underflow
                                       (1 << 12);   // PM - Precision (Inexact)
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};

  // Sticky exception status flags (IE, ZE, OE, UE, PE; DE has no FPSCR bit).
  // They are in MXCSR itself (CsrStatusMask), so a write of a cached MXCSR
  // must keep the current ones.
  static constexpr u32 StatusInvalid = 1 << 0;
  static constexpr u32 StatusDivideByZero = 1 << 2;
  static constexpr u32 StatusOverflow = 1 << 3;
  static constexpr u32 StatusUnderflow = 1 << 4;
  static constexpr u32 StatusInexact = 1 << 5;
  static constexpr u32 StatusMask = 0x3F;
  static constexpr u32 CsrStatusMask = StatusMask;

  static inline u32 getcsr() noexcept { return simde_mm_getcsr(); }

  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }

  static inline u32 getstatus() noexcept { return simde_mm_getcsr() & StatusMask; }

  static inline void setstatus(u32 status) noexcept {
    simde_mm_setcsr((simde_mm_getcsr() & ~StatusMask) | (status & StatusMask));
  }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

struct FPSCRPlatform {
  // RMode
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = 3 << RoundShift;
  // FZ and FZ16
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  // Nearest, Zero, -Infinity, -Infinity
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  // Exception enable bits (0 = exception disabled, ARM defaults to 0)
  static constexpr u32 ExceptionMask = (1 << 8) |   // IOE - Invalid Operation
                                       (1 << 9) |   // DZE - Division by Zero
                                       (1 << 10) |  // OFE - Overflow
                                       (1 << 11) |  // UFE - Underflow
                                       (1 << 12) |  // IXE - Inexact
                                       (1 << 15);   // IDE - Input Denormal

  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(csr)); }

  // Sticky exception status flags, in FPSR (IOC, DZC, OFC, UFC, IXC; IDC has
  // no FPSCR bit), not in FPCR.
  static constexpr u32 StatusInvalid = 1 << 0;
  static constexpr u32 StatusDivideByZero = 1 << 1;
  static constexpr u32 StatusOverflow = 1 << 2;
  static constexpr u32 StatusUnderflow = 1 << 3;
  static constexpr u32 StatusInexact = 1 << 4;
  static constexpr u32 StatusMask = 0x9F;
  static constexpr u32 CsrStatusMask = 0;

  static inline u32 getstatus() noexcept {
    u64 fpsr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(fpsr));
    return u32(fpsr) & StatusMask;
  }

  static inline void setstatus(u32 status) noexcept {
    u64 fpsr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(fpsr));
    fpsr = (fpsr & ~u64(StatusMask)) | (status & StatusMask);
    __asm__ __volatile__("msr fpsr, %0" : : "r"(fpsr));
  }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr &= ~ExceptionMask;  // Clear enable bits to disable exceptions
  }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::platform
