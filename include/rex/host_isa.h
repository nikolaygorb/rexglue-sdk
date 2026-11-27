/**
 * @file        rex/host_isa.h
 * @brief       Whether the host CPU runs code built for a wider instruction set
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::host_isa {

#if defined(__x86_64__) || defined(_M_X64)

struct CpuidResult {
  unsigned int eax, ebx, ecx, edx;
};

inline CpuidResult Cpuid(unsigned int leaf, unsigned int subleaf = 0) {
  CpuidResult r;
  __asm__("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
  return r;
}

// The OS saves the SSE and AVX registers (OSXSAVE, then XCR0 bits 1-2).
inline bool OsSavesAvx() {
  if ((Cpuid(1).ecx & (1u << 27)) == 0) {
    return false;
  }
  unsigned int xcr0_low, xcr0_high;
  __asm__("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0u));
  return (xcr0_low & 6u) == 6u;
}

// AVX and FMA (what -mfma enables).
inline bool HasFma() {
  const unsigned int ecx = Cpuid(1).ecx;
  return (ecx & (1u << 28)) && (ecx & (1u << 12)) && OsSavesAvx();
}

// x86-64-v3: AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE (and v2 below it).
inline bool HasX86_64_V3() {
  if (Cpuid(0).eax < 7) {
    return false;
  }
  const unsigned int ecx1 = Cpuid(1).ecx;
  const unsigned int ebx7 = Cpuid(7).ebx;
  const unsigned int ecx_ext = Cpuid(0x80000001u).ecx;
  const bool leaf1 = (ecx1 & (1u << 12)) && (ecx1 & (1u << 22)) && (ecx1 & (1u << 28)) &&
                     (ecx1 & (1u << 29)) && (ecx1 & (1u << 20)) && (ecx1 & (1u << 23));
  const bool leaf7 = (ebx7 & (1u << 3)) && (ebx7 & (1u << 5)) && (ebx7 & (1u << 8));
  const bool lzcnt = (ecx_ext & (1u << 5)) != 0;
  return leaf1 && leaf7 && lzcnt && OsSavesAvx();
}

#else

inline bool HasFma() {
  return true;
}
inline bool HasX86_64_V3() {
  return true;
}

#endif

// What the generated code was built for (REXGLUE_RECOMP_ISA, set on the host
// target by rexglue_setup_target), and whether this CPU runs it. An empty
// name: nothing beyond the baseline.
inline const char* RequiredName() {
#if defined(REXGLUE_RECOMP_ISA_X86_64_V3)
  return "x86-64-v3 (AVX2, FMA, BMI2, MOVBE)";
#elif defined(REXGLUE_RECOMP_ISA_FMA)
  return "AVX and FMA";
#else
  return "";
#endif
}

inline bool HostRunsRequired() {
#if defined(REXGLUE_RECOMP_ISA_X86_64_V3)
  return HasX86_64_V3();
#elif defined(REXGLUE_RECOMP_ISA_FMA)
  return HasFma();
#else
  return true;
#endif
}

}  // namespace rex::host_isa
