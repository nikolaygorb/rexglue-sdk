/**
 * @file        ppc/intrinsics.h
 * @brief       SIMD intrinsic helpers and vector lookup tables for PPC AltiVec emulation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp SIMD patterns and simde
 */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <simde/x86/avx.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include <rex/ppc/estimates.h>
#include <rex/types.h>

namespace rex::ppc {

//=============================================================================
// Vector Load/Store Mask Tables
//=============================================================================
// These tables are used for lvlx/lvrx (load vector left/right) and
// stvlx/stvrx (store vector left/right) instructions.

inline uint8_t VectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

//=============================================================================
// stvlx / stvrx
//=============================================================================
// A vector register holds its bytes reversed (u8[15 - i] is guest byte i).
// These write the part of it the instruction stores with at most two
// overlapping stores instead of a byte loop (upstream rexglue 55dd9b0b, U11),
// never touching a byte outside it. Like stvx, the stores are not volatile.

// Writes the first count (0-16) bytes of the 16 at bytes to host.
inline void StoreVectorBytes(uint8_t* host, const uint8_t* bytes, uint32_t count) {
  if (count == 16) {
    std::memcpy(host, bytes, 16);
  } else if (count >= 8) {
    std::memcpy(host, bytes, 8);
    std::memcpy(host + count - 8, bytes + count - 8, 8);
  } else if (count >= 4) {
    std::memcpy(host, bytes, 4);
    std::memcpy(host + count - 4, bytes + count - 4, 4);
  } else if (count >= 2) {
    std::memcpy(host, bytes, 2);
    std::memcpy(host + count - 2, bytes + count - 2, 2);
  } else if (count == 1) {
    host[0] = bytes[0];
  }
}

// The register's bytes in guest order.
inline void VectorGuestBytes(const uint8_t* reg, uint8_t* bytes) {
  simde_mm_storeu_si128(
      reinterpret_cast<simde__m128i*>(bytes),
      simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(reg)),
                            simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

// stvlx: guest bytes 0 .. 15 - (ea & 0xF) at ea (host: its host address).
inline void StoreVectorLeft(uint8_t* host, uint32_t ea, const uint8_t* reg) {
  alignas(16) uint8_t bytes[16];
  VectorGuestBytes(reg, bytes);
  StoreVectorBytes(host, bytes, 16 - (ea & 0xF));
}

// stvrx: the last ea & 0xF guest bytes at the start of ea's quadword
// (host_quadword: the host address of ea & ~0xF). Nothing when ea is aligned.
inline void StoreVectorRight(uint8_t* host_quadword, uint32_t ea, const uint8_t* reg) {
  alignas(16) uint8_t bytes[16];
  VectorGuestBytes(reg, bytes);
  const uint32_t count = ea & 0xF;
  StoreVectorBytes(host_quadword, bytes + 16 - count, count);
}

//=============================================================================
// SIMD Helper Functions
//=============================================================================

// Unsigned 32-bit saturating add
inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

// Signed 32-bit saturating subtract (vsubsws)
inline simde__m128i simde_mm_subs_epi32(simde__m128i a, simde__m128i b) {
  const simde__m128i diff = simde_mm_sub_epi32(a, b);
  // Overflow: a and b differ in sign and the difference's sign is not a's.
  const simde__m128i overflow =
      simde_mm_and_si128(simde_mm_xor_si128(a, b), simde_mm_xor_si128(a, diff));
  const simde__m128i saturated =
      simde_mm_xor_si128(simde_mm_srai_epi32(a, 31), simde_mm_set1_epi32(0x7FFFFFFF));
  return simde_mm_castps_si128(simde_mm_blendv_ps(simde_mm_castsi128_ps(diff),
                                                  simde_mm_castsi128_ps(saturated),
                                                  simde_mm_castsi128_ps(overflow)));
}

// Unsigned halfwords to bytes, saturated (vpkuhus); packus alone reads them as signed
inline simde__m128i simde_mm_packus_epu16(simde__m128i lo, simde__m128i hi) {
  const simde__m128i max = simde_mm_set1_epi16(0xFF);
  return simde_mm_packus_epi16(simde_mm_min_epu16(lo, max), simde_mm_min_epu16(hi, max));
}

// Unsigned words to halfwords, saturated (vpkuwus)
inline simde__m128i simde_mm_packus_epu32(simde__m128i lo, simde__m128i hi) {
  const simde__m128i max = simde_mm_set1_epi32(0xFFFF);
  return simde_mm_packus_epi32(simde_mm_min_epu32(lo, max), simde_mm_min_epu32(hi, max));
}

// Each word rotated left by the low 5 bits of its count (vrlw)
inline simde__m128i simde_mm_rolv_epi32(simde__m128i a, simde__m128i count) {
  count = simde_mm_and_si128(count, simde_mm_set1_epi32(31));
#if defined(__AVX2__) || !(defined(__x86_64__) || defined(_M_X64))
  return simde_mm_or_si128(
      simde_mm_sllv_epi32(a, count),
      simde_mm_srlv_epi32(a, simde_mm_sub_epi32(simde_mm_set1_epi32(32), count)));
#else
  // 16 swaps the halves; the rest is a multiply by 2^n (exact conversion, no
  // FP flag), whose 64-bit product has the rotation as high half | low half.
  const simde__m128i swap = simde_mm_cmpgt_epi32(simde_mm_and_si128(count, simde_mm_set1_epi32(16)),
                                                 simde_mm_setzero_si128());
  a = simde_mm_blendv_epi8(
      a, simde_mm_or_si128(simde_mm_slli_epi32(a, 16), simde_mm_srli_epi32(a, 16)), swap);
  const simde__m128i factor = simde_mm_cvttps_epi32(simde_mm_castsi128_ps(
      simde_mm_slli_epi32(simde_mm_add_epi32(simde_mm_and_si128(count, simde_mm_set1_epi32(15)),
                                             simde_mm_set1_epi32(127)),
                          23)));
  const simde__m128i even = simde_mm_mul_epu32(a, factor);
  const simde__m128i odd =
      simde_mm_mul_epu32(simde_mm_srli_epi64(a, 32), simde_mm_srli_epi64(factor, 32));
  return simde_mm_blend_epi16(simde_mm_or_si128(even, simde_mm_srli_epi64(even, 32)),
                              simde_mm_or_si128(odd, simde_mm_slli_epi64(odd, 32)), 0xCC);
#endif
}

// Signed 8-bit average (rounds towards zero)
inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Signed 16-bit average
inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Signed 32-bit average
inline simde__m128i simde_mm_avg_epi32(simde__m128i a, simde__m128i b) {
  simde__m128i sum = simde_mm_add_epi32(simde_mm_srai_epi32(a, 1), simde_mm_srai_epi32(b, 1));
  return simde_mm_add_epi32(sum,
                            simde_mm_and_si128(simde_mm_or_si128(a, b), simde_mm_set1_epi32(1)));
}

// Convert unsigned 32-bit integers to floats
inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1) {
  simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
  simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
  xmm0 = simde_mm_srli_epi32(xmm0, 31);
  xmm0 = simde_mm_add_epi32(xmm0, xmm1);
  xmm0 = simde_mm_srai_epi32(xmm0, 8);
  xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
  simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
  return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

// Permute bytes from two vectors based on control vector
inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c) {
  simde__m128i d = simde_mm_set1_epi8(0xF);
  simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e),
                              simde_mm_slli_epi32(c, 3));
}

// Unsigned 8-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Unsigned 16-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Vector Convert To Signed Fixed-Point Word Saturate
inline simde__m128i simde_mm_vctsxs(simde__m128 src1) {
  simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
  simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
  simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
  xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
  simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
                                        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)),
                                        simde_mm_castsi128_ps(xmm1));
  return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

// Vector Convert To Unsigned Fixed-Point Word Saturate
// Convert float to unsigned int with saturation to [0, UINT_MAX]
// NaN -> 0, negative -> 0, > UINT_MAX -> UINT_MAX
inline simde__m128i simde_mm_vctuxs(simde__m128 src1) {
  simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
  simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
  simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);  // UINT_MAX as float
  simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);

  // Clamp to [0, UINT_MAX]
  simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
  clamped = simde_mm_min_ps(clamped, max_val);

  // Convert to signed int first (will handle values up to INT_MAX correctly)
  // For values > INT_MAX, we need special handling
  simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);  // 2^31
  simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);

  // For values >= 2^31, subtract 2^31 before conversion and add it back after
  simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
  simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
  simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
                                             simde_mm_set1_epi32(int(0x80000000u)));
  simde__m128i result = simde_mm_or_si128(low_bits, high_bit);

  // Apply saturation: NaN -> 0, overflow -> UINT_MAX
  result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
  result = simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
      simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));

  return result;
}

// Vector Shift Right
inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
  b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
  return simde_mm_castps_si128(simde_mm_insert_ps(
      simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)),
      simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

// Vector Shift Left - shift entire 128-bit vector left by bits in low 3 bits of b
inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
  // On hardware each byte shifts by its own count, taking the bits of the next byte; the 128-bit
  // shift below is the usual case, where all the counts agree (a vspltisb count).
  const simde__m128i counts = simde_mm_and_si128(b, simde_mm_set1_epi8(0x7));
  if (simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(
          counts, simde_mm_shuffle_epi8(counts, simde_mm_setzero_si128()))) != 0xFFFF) {
    alignas(16) uint8_t src[16], count[16], dst[16];
    simde_mm_store_si128((simde__m128i*)src, a);
    simde_mm_store_si128((simde__m128i*)count, counts);
    // Host byte j is guest byte 15 - j, so the next guest byte is host j - 1.
    for (int j = 0; j < 16; ++j) {
      dst[j] = uint8_t((src[j] << count[j]) | (j ? src[j - 1] >> (8 - count[j]) : 0));
    }
    return simde_mm_load_si128((simde__m128i*)dst);
  }
  int shift = simde_mm_extract_epi8(counts, 0);
  if (shift == 0)
    return a;

#if defined(__x86_64__) || defined(_M_X64)
  // Split into high and low 64-bit parts
  simde__m128i low_shifted = simde_mm_slli_epi64(a, shift);
  simde__m128i high_carry = simde_mm_srli_epi64(a, 64 - shift);
  // Shift the carry from low qword to high qword position
  high_carry = simde_mm_slli_si128(high_carry, 8);
  return simde_mm_or_si128(low_shifted, high_carry);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using vld1/vst1 for conversion
  uint64_t vals[2];
  uint64_t res[2] = {0, 0};

  // Store simde__m128i to memory
  simde_mm_store_si128((simde__m128i*)vals, a);

  // Load as NEON vector
  uint64x2_t va = vld1q_u64(vals);

  // vshlq_u64 accepts variable shift per lane
  int64x2_t shift_vector = vdupq_n_s64(shift);
  uint64x2_t low_shifted = vshlq_u64(va, shift_vector);

  // NEON vshl uses negative counts for right shifts.
  int64x2_t rshift_vector = vdupq_n_s64(shift - 64);
  uint64x2_t high_carry = vshlq_u64(va, rshift_vector);

  // Combine results
  uint64x2_t result_vec = vdupq_n_u64(0);
  result_vec = vsetq_lane_u64(vgetq_lane_u64(low_shifted, 0), result_vec, 0);
  result_vec =
      vsetq_lane_u64(vgetq_lane_u64(low_shifted, 1) | vgetq_lane_u64(high_carry, 0), result_vec, 1);

  // Store back to memory and reload as simde__m128i
  vst1q_u64(res, result_vec);
  return simde_mm_load_si128((simde__m128i*)res);
#else
#error "Unsupported architecture for simde_mm_vsl (only x86_64 and ARM64 supported)"
#endif
}

// Vector Shift Left by Octet - shift entire vector left by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC left shift = shift towards MSB (lower PPC addresses) = shift towards higher x86 addresses
inline simde__m128i simde_mm_vslo(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vslo (only x86_64 and ARM64 supported)"
#endif
}

// Vector Shift Right by Octet - shift entire vector right by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC right shift = shift towards LSB (higher PPC addresses) = shift towards lower x86 addresses
inline simde__m128i simde_mm_vsro(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vsro (only x86_64 and ARM64 supported)"
#endif
}

// Variable 16-bit shift left: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi32(a_hi, s_hi);
  simde__m128i mask16 = simde_mm_set1_epi32(0xFFFF);
  r_lo = simde_mm_and_si128(r_lo, mask16);
  r_hi = simde_mm_and_si128(r_hi, mask16);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

// Variable 16-bit logical right shift: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srlv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srlv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srlv_epi32(a_hi, s_hi);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

// Variable 16-bit arithmetic right shift: sign-extend to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srav_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  // Sign-extend a: duplicate each 16-bit lane, then arithmetic shift right by 16
  simde__m128i a_lo = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(a, a), 16);
  simde__m128i a_hi = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(a, a), 16);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srav_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srav_epi32(a_hi, s_hi);
  return simde_mm_packs_epi32(r_lo, r_hi);
}

// Variable 8-bit shift left: widen to 16-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi8(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi8(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi8(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi8(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi8(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi16(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi16(a_hi, s_hi);
  simde__m128i mask8 = simde_mm_set1_epi16(0xFF);
  r_lo = simde_mm_and_si128(r_lo, mask8);
  r_hi = simde_mm_and_si128(r_hi, mask8);
  return simde_mm_packus_epi16(r_lo, r_hi);
}

//=============================================================================
// Single -> double load
//=============================================================================
// lfs widens a single to a double. cvtss2sd reads a denormal single as zero
// when DAZ is set (the VMX flush mode), so a denormal is rebuilt from its
// integer mantissa instead: the value is a normal double, exact whatever the
// MXCSR. The result never depends on the flush mode, so lfs needs no switch.
// A NaN is widened bit for bit: cvtss2sd would quiet a signalling one, which
// lfs does not, so a 32-bit value copied through a float register changed.
inline double fp_load_single(uint32_t bits) {
  const uint32_t exponent = bits & 0x7F800000u;
  const uint32_t mantissa = bits & 0x007FFFFFu;
  if ((exponent == 0 || exponent == 0x7F800000u) && mantissa != 0) [[unlikely]] {
    if (exponent) {
      return std::bit_cast<double>((uint64_t(bits & 0x80000000u) << 32) |
                                   UINT64_C(0x7FF0000000000000) | (uint64_t(mantissa) << 29));
    }
    double v = double(mantissa) * 0x1p-149;
    return (bits & 0x80000000u) ? -v : v;
  }
  return double(std::bit_cast<float>(bits));
}

//=============================================================================
// Double -> single store
//=============================================================================
// stfs narrows a double to a single. A NaN keeps its sign and the high 23 bits
// of its fraction, as the hardware does: cvtsd2ss would quiet a signalling one.
// Callers use the scalar flush mode (off) for denormal results.
inline uint32_t fp_store_single(double value) {
  const uint64_t bits = std::bit_cast<uint64_t>(value);
  if ((bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000) &&
      (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0) [[unlikely]] {
    return (uint32_t(bits >> 32) & 0x80000000u) | 0x7F800000u |
           (uint32_t(bits >> 29) & 0x007FFFFFu);
  }
  return std::bit_cast<uint32_t>(float(value));
}

//=============================================================================
// Scalar multiply-add
//=============================================================================
// fmadd, fmsub, fnmadd, fnmsub and their single forms call fp_fma. Built with
// FMA (-mfma, ARM64) it is the fused instruction, inline. Otherwise std::fma is
// a library call: a few nanoseconds on a CPU with FMA, but on one without
// (Intel before Haswell, AMD before Piledriver) a software emulation of the
// single rounding, about 30 ns a call in the Windows CRT, and games make tens
// of millions of these calls a second (slowed sound, stalled frames). On such
// CPUs fp_fma multiplies, rounds and adds instead: the result may differ from
// the fused one in the last bit. The environment variable REX_PPC_FMA=0 forces
// the multiply-add, REX_PPC_FMA=1 std::fma (read once at startup).

// Two roundings. The empty asm makes the rounded product an opaque value, so
// the compiler can't contract a * b + c into an fma, not even under
// -ffp-contract=fast.
inline double fp_mul_add(double a, double b, double c) {
  double product = a * b;
#if defined(__x86_64__) || defined(_M_X64)
  __asm__("" : "+x"(product));
#endif
  return product + c;
}

#if defined(__FMA__) || defined(__aarch64__) || defined(_M_ARM64)

inline double fp_fma(double a, double b, double c) { return __builtin_fma(a, b, c); }

#else

// CPUID leaf 1: FMA (ECX bit 12), and OSXSAVE (bit 27) for XGETBV, whose XCR0
// bits 1-2 say the OS saves the SSE and AVX registers.
inline bool fp_detect_host_fma() {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int eax, ebx, ecx, edx;
  __asm__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
  if ((ecx & (1u << 12)) == 0 || (ecx & (1u << 27)) == 0) {
    return false;
  }
  unsigned int xcr0_low, xcr0_high;
  __asm__("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0u));
  return (xcr0_low & 6u) == 6u;
#else
  return true;
#endif
}

inline bool fp_init_host_fma() {
  char value[8] = {};
#if defined(_WIN32)
  size_t length = 0;
  if (getenv_s(&length, value, sizeof(value), "REX_PPC_FMA") != 0) {
    value[0] = '\0';
  }
#else
  if (const char* env = std::getenv("REX_PPC_FMA")) {
    value[0] = env[0];
  }
#endif
  if (value[0] == '0') {
    return false;
  }
  if (value[0] == '1') {
    return true;
  }
  return fp_detect_host_fma();
}

inline const bool fp_host_has_fma = fp_init_host_fma();

inline double fp_fma(double a, double b, double c) {
  if (fp_host_has_fma) [[likely]] {
    return std::fma(a, b, c);
  }
  return fp_mul_add(a, b, c);
}

#endif

//=============================================================================
// Scalar arithmetic: NaN results, host flags
//=============================================================================
// fadd, fsub, fmul, fdiv, fsqrt and their single forms: a NaN operand is the
// result, the first in A, B order, quieted, and an invalid operation with no
// NaN operand (infinity minus infinity, zero over zero, the root of a negative
// value) gives the default QNaN, which is positive on PPC, negative on x86.
// The FPSCR exception bits live in the host's sticky status flags
// (FPSCRRegister), so the host arithmetic must raise the flags the hardware's
// does, no more, no fewer.

inline constexpr uint64_t kFpQuietBit = 0x0008000000000000ull;
inline constexpr double kFpDefaultQNaN = std::bit_cast<double>(0x7FF8000000000000ull);

// Fixes up the result `r` of an arithmetic on `operands`, in A, B order.
template <typename... Operands>
inline double fp_nan_result(double r, Operands... operands) {
  if (!std::isnan(r)) [[likely]] {
    return r;
  }
  for (double x : {double(operands)...}) {
    if (std::isnan(x)) {
      return std::bit_cast<double>(std::bit_cast<uint64_t>(x) | kFpQuietBit);
    }
  }
  return kFpDefaultQNaN;
}

// A value the compiler can't see through. Arithmetic on it stays in the branch
// it is written in: computed ahead of the branch (a select), it would raise
// the host flags of the path not taken.
inline double fp_opaque(double x) {
#if defined(__x86_64__) || defined(_M_X64)
  __asm__("" : "+x"(x));
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__("" : "+w"(x));
#endif
  return x;
}

// Raises the host's invalid flag (an FPSCR VX cause the host arithmetic
// doesn't raise itself).
inline void fp_raise_invalid() {
  volatile double zero = 0.0;
  volatile double product = zero * HUGE_VAL;
  (void)product;
}

// The host's square root instruction. The C library's sqrt may answer a NaN
// operand in software, without the invalid flag of an SNaN (VXSNAN).
inline double fp_sqrt(double x) {
  const simde__m128d v = simde_mm_set_sd(x);
  return simde_mm_cvtsd_f64(simde_mm_sqrt_sd(v, v));
}

// Raise the host's divide-by-zero flag (ZX), and overflow with inexact (OX,
// XX).
inline void fp_raise_divide_by_zero() {
  volatile double one = 1.0;
  volatile double zero = 0.0;
  volatile double quotient = one / zero;
  (void)quotient;
}

inline void fp_raise_overflow() {
  volatile double largest = 0x1.FFFFFFFFFFFFFp1023;
  volatile double product = largest * 2.0;
  (void)product;
}

inline bool fp_is_snan(double x) {
  const uint64_t bits = std::bit_cast<uint64_t>(x);
  return (bits & 0x7FF8000000000000ull) == 0x7FF0000000000000ull &&
         (bits & 0x0007FFFFFFFFFFFFull) != 0;
}

// fres and frsqrte: the console's estimates (rex/ppc/estimates.h) and the
// flags its FPU raises with them, from the hardware captures: ZX for a zero,
// VX for an SNaN, and for frsqrte a negative operand (VXSQRT), OX and XX for
// fres of a denormal (the reciprocal overflows). An estimate in range raises
// nothing, not even XX.
inline double fp_fres(double b) {
  const double r = estimate_reciprocal_single(b);
  const uint64_t magnitude = std::bit_cast<uint64_t>(b) & 0x7FFFFFFFFFFFFFFFull;
  if (magnitude - 1 >= 0x7FEFFFFFFFFFFFFFull) [[unlikely]] {
    // Zero, infinity or a NaN.
    if (magnitude == 0) {
      fp_raise_divide_by_zero();
    } else if (fp_is_snan(b)) {
      fp_raise_invalid();
    }
  } else if (std::isinf(r)) [[unlikely]] {
    fp_raise_overflow();
  }
  return r;
}

inline double fp_frsqrte(double b) {
  const double r = estimate_reciprocal_sqrt_double(b);
  const uint64_t bits = std::bit_cast<uint64_t>(b);
  const uint64_t magnitude = bits & 0x7FFFFFFFFFFFFFFFull;
  if (magnitude == 0) [[unlikely]] {
    fp_raise_divide_by_zero();
  } else if (fp_is_snan(b) || (magnitude <= 0x7FF0000000000000ull && (bits >> 63))) [[unlikely]] {
    fp_raise_invalid();
  }
  return r;
}

// vrefp and vrsqrtefp as the host's exact 1/x and 1/sqrt(x) instead of the
// console's estimates: faster, not bit-exact (see estimates.h for the risk).
// Per module (the title's code lives in the executable, which sets it).
inline std::atomic<bool> vec_estimate_host{false};

// The lane-parallel estimates' tables, one 32-bit entry per segment for a
// single read per lane: two int16 for pmaddwd, which interpolates in one step.
// vrsqrtefp: the slope in the high half; the base's 16 bits less 0x8000 in the
// low half (signed), the 0x8000 * 1024 added back after.
inline constexpr std::array<uint32_t, 32> kVecEstimateRsqrtTable = [] {
  std::array<uint32_t, 32> table{};
  for (size_t i = 0; i < table.size(); ++i) {
    const uint32_t entry = kEstimateReciprocalSqrtTable[i];
    table[i] = (entry & 0xFFFF0000u) | ((entry - 0x8000u) & 0xFFFFu);
  }
  return table;
}();
// vrefp: the slope in the high half, the base / 512 (all are multiples) in the low.
inline constexpr std::array<uint32_t, 32> kVecEstimateRecipTable = [] {
  std::array<uint32_t, 32> table{};
  for (size_t i = 0; i < table.size(); ++i) {
    table[i] = (uint32_t(kEstimateReciprocalSlope[i]) << 16) | (kEstimateReciprocalBase[i] >> 9);
  }
  return table;
}();

inline simde__m128i vec_table_lookup(const std::array<uint32_t, 32>& table, simde__m128i index) {
#if defined(__AVX2__) && !defined(REX_PPC_NO_GATHER)
  return simde_mm_i32gather_epi32(reinterpret_cast<const int32_t*>(table.data()), index, 4);
#else
  return simde_mm_setr_epi32(int32_t(table[uint32_t(simde_mm_cvtsi128_si32(index))]),
                             int32_t(table[uint32_t(simde_mm_extract_epi32(index, 1))]),
                             int32_t(table[uint32_t(simde_mm_extract_epi32(index, 2))]),
                             int32_t(table[uint32_t(simde_mm_extract_epi32(index, 3))]));
#endif
}

// pmaddwd's second operand: `low` in the low half, -step in the high.
inline simde__m128i vec_estimate_weights(simde__m128i step, int32_t low) {
  return simde_mm_or_si128(simde_mm_slli_epi32(simde_mm_sub_epi32(simde_mm_setzero_si128(), step), 16),
                           simde_mm_set1_epi32(low));
}

// estimate_reciprocal_sqrt in all lanes when each is a positive normal; false
// otherwise (the scalar handles zeros, denormals, negatives, infinities, NaNs).
// Checked against it for every input (each positive normal takes this path).
inline bool vec_estimate_rsqrt_normal(simde__m128 x, simde__m128& out) {
  const simde__m128i bits = simde_mm_castps_si128(x);
  const simde__m128i normal =
      simde_mm_andnot_si128(simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x00800000), bits),
                            simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x7F800000), bits));
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(normal)) != 0xF) [[unlikely]] {
    return false;
  }
  // The exponent's parity and the top 4 mantissa bits pick the entry.
  const simde__m128i index =
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 19), simde_mm_set1_epi32(31));
  const simde__m128i interp =
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 9), simde_mm_set1_epi32(1023));
  simde__m128i raw = simde_mm_add_epi32(
      simde_mm_madd_epi16(vec_table_lookup(kVecEstimateRsqrtTable, index),
                          vec_estimate_weights(interp, 1024)),
      simde_mm_set1_epi32(0x2000000));
  // An odd exponent's half of the table lacks bit 25, never bit 24: one shift
  // normalizes it, (380 - exponent) / 2 is the result's exponent either way.
  const simde__m128i odd = simde_mm_srai_epi32(simde_mm_slli_epi32(bits, 8), 31);
  raw = simde_mm_add_epi32(raw, simde_mm_and_si128(raw, odd));
  // Two bits dropped, to nearest even.
  raw = simde_mm_add_epi32(
      simde_mm_add_epi32(raw, simde_mm_set1_epi32(1)),
      simde_mm_and_si128(simde_mm_srli_epi32(raw, 2), simde_mm_set1_epi32(1)));
  const simde__m128i exponent = simde_mm_slli_epi32(
      simde_mm_srli_epi32(simde_mm_sub_epi32(simde_mm_set1_epi32(380), simde_mm_srli_epi32(bits, 23)),
                          1),
      23);
  out = simde_mm_castsi128_ps(simde_mm_or_si128(
      exponent, simde_mm_and_si128(simde_mm_srli_epi32(raw, 2), simde_mm_set1_epi32(0x007FFFFF))));
  return true;
}

// estimate_reciprocal in all lanes when each has a biased exponent 1..252
// (the result normal); false otherwise.
inline bool vec_estimate_recip_normal(simde__m128 x, simde__m128& out) {
  const simde__m128i bits = simde_mm_castps_si128(x);
  const simde__m128i magnitude = simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7FFFFFFF));
  const simde__m128i normal =
      simde_mm_andnot_si128(simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x00800000), magnitude),
                            simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x7E800000), magnitude));
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(normal)) != 0xF) [[unlikely]] {
    return false;
  }
  const simde__m128i segment =
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 18), simde_mm_set1_epi32(31));
  const simde__m128i step =
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 9), simde_mm_set1_epi32(0x1FF));
  const simde__m128i result = simde_mm_madd_epi16(vec_table_lookup(kVecEstimateRecipTable, segment),
                                                  vec_estimate_weights(step, 512));
  // (253 - exponent) << 23, the sign kept.
  const simde__m128i exponent = simde_mm_sub_epi32(
      simde_mm_set1_epi32(253 << 23), simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7F800000)));
  out = simde_mm_castsi128_ps(simde_mm_or_si128(
      simde_mm_or_si128(simde_mm_andnot_si128(magnitude, bits), exponent),
      simde_mm_and_si128(result, simde_mm_set1_epi32(0x007FFFFF))));
  return true;
}

inline simde__m128i vec_table_lookup16(const uint16_t* table, simde__m128i index) {
  return simde_mm_setr_epi32(table[uint32_t(simde_mm_cvtsi128_si32(index))],
                             table[uint32_t(simde_mm_extract_epi32(index, 1))],
                             table[uint32_t(simde_mm_extract_epi32(index, 2))],
                             table[uint32_t(simde_mm_extract_epi32(index, 3))]);
}

// The high 32 bits of each lane's 32 x 32-bit unsigned product.
inline simde__m128i vec_mulhi_epu32(simde__m128i a, simde__m128i b) {
  const simde__m128i even = simde_mm_srli_epi64(simde_mm_mul_epu32(a, b), 32);
  const simde__m128i odd =
      simde_mm_mul_epu32(simde_mm_srli_epi64(a, 32), simde_mm_srli_epi64(b, 32));
  return simde_mm_blend_epi16(even, odd, 0xCC);
}

// 2^e as an integer for e in 0..30. Exact conversions raise no host FP flag.
inline simde__m128i vec_pow2_epi32(simde__m128i e) {
  return simde_mm_cvttps_epi32(simde_mm_castsi128_ps(
      simde_mm_slli_epi32(simde_mm_add_epi32(e, simde_mm_set1_epi32(127)), 23)));
}

// estimate_log2 in all lanes when each is a positive normal; false otherwise.
// The sum is exact, as in the scalar code.
inline bool vec_estimate_log2_normal(simde__m128 x, simde__m128& out) {
  const simde__m128i bits = simde_mm_castps_si128(x);
  const simde__m128i normal =
      simde_mm_andnot_si128(simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x00800000), bits),
                            simde_mm_cmpgt_epi32(simde_mm_set1_epi32(0x7F800000), bits));
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(normal)) != 0xF) [[unlikely]] {
    return false;
  }
  const simde__m128i exponent =
      simde_mm_sub_epi32(simde_mm_srli_epi32(bits, 23), simde_mm_set1_epi32(127));
  const simde__m128i fraction = vec_table_lookup16(
      kEstimateLog2Table,
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 12), simde_mm_set1_epi32(0x7FF)));
  out = simde_mm_add_ps(
      simde_mm_cvtepi32_ps(exponent),
      simde_mm_mul_ps(simde_mm_cvtepi32_ps(fraction), simde_mm_set1_ps(1.0f / 2048.0f)));
  return true;
}

// estimate_pow2 in all lanes when each is below 128 in magnitude (no NaN or
// infinity) and the result is normal or one; false otherwise.
inline bool vec_estimate_pow2_normal(simde__m128 x, simde__m128& out) {
  const simde__m128i bits = simde_mm_castps_si128(x);
  const simde__m128i exponent =
      simde_mm_and_si128(simde_mm_srli_epi32(bits, 23), simde_mm_set1_epi32(0xFF));
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(
          simde_mm_cmpgt_epi32(simde_mm_set1_epi32(134), exponent))) != 0xF) [[unlikely]] {
    return false;
  }
  // The magnitude in fixed point with 15 fraction bits, truncated: the
  // mantissa shifted right by 135 - exponent, as a multiply's high half by
  // 2^(32 - shift). Shifts above 32 (denormals and zeros included) give 0.
  const simde__m128i mantissa = simde_mm_or_si128(
      simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7FFFFF)), simde_mm_set1_epi32(0x800000));
  const simde__m128i shift = simde_mm_sub_epi32(simde_mm_set1_epi32(135), exponent);
  const simde__m128i in_range = simde_mm_cmpgt_epi32(simde_mm_set1_epi32(33), shift);
  const simde__m128i factor = simde_mm_and_si128(
      vec_pow2_epi32(simde_mm_max_epi32(simde_mm_sub_epi32(simde_mm_set1_epi32(32), shift),
                                        simde_mm_setzero_si128())),
      in_range);
  const simde__m128i sign = simde_mm_srai_epi32(bits, 31);
  const simde__m128i units = simde_mm_srai_epi32(
      simde_mm_sub_epi32(simde_mm_xor_si128(vec_mulhi_epu32(mantissa, factor), sign), sign), 4);
  const simde__m128i integer = simde_mm_srai_epi32(units, 11);
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(
          simde_mm_cmpgt_epi32(integer, simde_mm_set1_epi32(-127)))) != 0xF) [[unlikely]] {
    return false;
  }
  const simde__m128i fraction =
      vec_table_lookup16(kEstimatePow2Table, simde_mm_and_si128(units, simde_mm_set1_epi32(0x7FF)));
  out = simde_mm_castsi128_ps(simde_mm_add_epi32(
      simde_mm_slli_epi32(simde_mm_add_epi32(integer, simde_mm_set1_epi32(126)), 23),
      simde_mm_slli_epi32(fraction, 12)));
  return true;
}

// vrefp, vrsqrtefp, vlogefp, vexptefp: the console's estimate in each lane.
template <uint32_t (*Estimate)(uint32_t)>
inline simde__m128 vec_estimate(simde__m128 x) {
  if constexpr (Estimate == &estimate_reciprocal_sqrt) {
    if (vec_estimate_host.load(std::memory_order_relaxed)) [[unlikely]] {
      return simde_mm_div_ps(simde_mm_set1_ps(1.0f), simde_mm_sqrt_ps(x));
    }
    simde__m128 r;
    if (vec_estimate_rsqrt_normal(x, r)) [[likely]] {
      return r;
    }
  } else if constexpr (Estimate == &estimate_reciprocal) {
    if (vec_estimate_host.load(std::memory_order_relaxed)) [[unlikely]] {
      return simde_mm_div_ps(simde_mm_set1_ps(1.0f), x);
    }
    simde__m128 r;
    if (vec_estimate_recip_normal(x, r)) [[likely]] {
      return r;
    }
  } else if constexpr (Estimate == &estimate_log2) {
    simde__m128 r;
    if (vec_estimate_log2_normal(x, r)) [[likely]] {
      return r;
    }
  } else if constexpr (Estimate == &estimate_pow2) {
    simde__m128 r;
    if (vec_estimate_pow2_normal(x, r)) [[likely]] {
      return r;
    }
  }
  alignas(16) uint32_t lanes[4];
  simde_mm_store_ps(reinterpret_cast<float*>(lanes), x);
  for (uint32_t& lane : lanes) {
    lane = Estimate(lane);
  }
  return simde_mm_load_ps(reinterpret_cast<const float*>(lanes));
}

// fsel's A >= 0 without a compare, which raises the invalid flag for an SNaN
// (fsel raises nothing): not a NaN, and positive or a zero of either sign.
inline bool fp_select_nonnegative(double a) {
  const uint64_t bits = std::bit_cast<uint64_t>(a);
  const uint64_t magnitude = bits & 0x7FFFFFFFFFFFFFFFull;
  return magnitude <= 0x7FF0000000000000ull && (bits == magnitude || magnitude == 0);
}

// fctiw, fctiwz, fctid, fctidz. The host conversion always runs: it raises the
// invalid flag (VXCVI) for a NaN or a value out of range, and answers those
// with the smallest integer, the hardware's result for a NaN and a negative
// value; a positive one saturates to the largest. fctiw and fctiwz put the
// sign extension of the word in the high word, as the hardware does.
inline int64_t fp_to_int32(double x) {
  int32_t r = simde_mm_cvtsd_si32(simde_mm_set_sd(x));
  if (r == INT32_MIN && x > 0.0) {
    r = INT32_MAX;
  }
  return r;
}

inline int64_t fp_to_int32_truncate(double x) {
  int32_t r = simde_mm_cvttsd_si32(simde_mm_set_sd(x));
  if (r == INT32_MIN && x > 0.0) {
    r = INT32_MAX;
  }
  return r;
}

inline int64_t fp_to_int64(double x) {
  int64_t r = simde_mm_cvtsd_si64(simde_mm_set_sd(x));
  if (r == INT64_MIN && x > 0.0) {
    r = INT64_MAX;
  }
  return r;
}

inline int64_t fp_to_int64_truncate(double x) {
  int64_t r = simde_mm_cvttsd_si64(simde_mm_set_sd(x));
  if (r == INT64_MIN && x > 0.0) {
    r = INT64_MAX;
  }
  return r;
}

//=============================================================================
// Multiply-add NaN rules
//=============================================================================
// fmadd/fmsub/fnmadd/fnmsub (and their single forms), vmaddfp and vnmsubfp
// compute D = +-(A * C +- B). When an operand is a NaN, the hardware returns
// the first NaN operand in A, B, C order, quieted, with its sign left alone;
// that is not the host's order. An invalid operation with no NaN operand
// (infinity times zero) gives the default QNaN, which is positive on PPC and
// negative on x86. The negated forms never negate a NaN result. Infinity times
// zero is invalid (VXIMZ) even when the addend is a QNaN, where x86's FMA
// raises nothing.

// Fixes up the result `r` of a scalar multiply-add with operands a, b, c
// (the product a * c, the addend b).
inline double fp_madd_result(double r, double a, double b, double c, bool negate) {
  if (!std::isnan(r)) [[likely]] {
    return negate ? -r : r;
  }
  constexpr uint64_t kMagnitude = 0x7FFFFFFFFFFFFFFFull;
  constexpr uint64_t kInfinity = 0x7FF0000000000000ull;
  const uint64_t ma = std::bit_cast<uint64_t>(a) & kMagnitude;
  const uint64_t mc = std::bit_cast<uint64_t>(c) & kMagnitude;
  if ((ma == kInfinity && mc == 0) || (ma == 0 && mc == kInfinity)) {
    fp_raise_invalid();
  }
  for (double x : {a, b, c}) {
    if (std::isnan(x)) {
      return std::bit_cast<double>(std::bit_cast<uint64_t>(x) | kFpQuietBit);
    }
  }
  return kFpDefaultQNaN;
}

//=============================================================================
// Single-precision denormal operand quirk
//=============================================================================
// The Xenon FPU's single-precision arithmetic treats a denormalized double
// operand specially (xenia-edge 28f38affe, 8b19ee756, from its tests made on
// hardware): fadds, fsubs, fmuls and the single multiply-adds answer with the
// default QNaN; fdivs and fsqrts keep the double result, not rounded to
// single. A NaN or infinite operand takes precedence over the quirk.

// Whether every operand is finite and at least one is denormal (a zero isn't:
// subtracting one wraps its magnitude to the top).
// One test of each operand on the way in (the compiler packs them into one
// SSE compare), and the finiteness of all only when one is denormal: the hot
// path of a two-operand check is 9 instructions instead of 14 (x86-64-v2).
// Moving the slow half out of line costs more (spills around the call).
template <typename... Operands>
inline bool fp_single_denormal_operand(Operands... operands) {
  constexpr uint64_t kMagnitude = 0x7FFFFFFFFFFFFFFFull;
  bool any_denormal = false;
  for (double x : {double(operands)...}) {
    any_denormal |= (std::bit_cast<uint64_t>(x) & kMagnitude) - 1 < 0x000FFFFFFFFFFFFFull;
  }
  if (!any_denormal) [[likely]] {
    return false;
  }
  bool all_finite = true;
  for (double x : {double(operands)...}) {
    all_finite &= (std::bit_cast<uint64_t>(x) & kMagnitude) < 0x7FF0000000000000ull;
  }
  return all_finite;
}

//=============================================================================
// FPSCR[FR]: whether a result was rounded away from zero
//=============================================================================
// For the codegen option fpscr_result_bits. 1 when the inexact result is
// larger in magnitude than the exact one, 0 when not, and -1 when the exact
// one isn't known. The double-precision forms take the exact error from an
// error-free transformation; the others compare with a double-precision
// reference (exact for frsp, fmuls and fcfid's integer, within double rounding
// otherwise).

inline int fp_rounded_away_error(double result, double error) {
  if (error == 0.0 || std::isnan(error) || !std::isfinite(result)) {
    return 0;
  }
  return std::signbit(error) != std::signbit(result) ? 1 : 0;
}

inline int fp_rounded_away_sum(double result, double a, double b) {
  const double b_virtual = result - a;
  const double error = (a - (result - b_virtual)) + (b - b_virtual);
  return fp_rounded_away_error(result, error);
}

inline int fp_rounded_away_product(double result, double a, double b) {
  return fp_rounded_away_error(result, std::fma(a, b, -result));
}

inline int fp_rounded_away_quotient(double result, double a, double b) {
  // a - q * b has the sign of the error times b's.
  const double remainder = std::fma(-result, b, a);
  return fp_rounded_away_error(result, std::signbit(b) ? -remainder : remainder);
}

inline int fp_rounded_away_root(double result, double a) {
  return fp_rounded_away_error(result, std::fma(-result, result, a));
}

// fcfid: compared with the integer exactly (a double at 2^63 or more is above
// every int64_t).
inline int fp_rounded_away_from_integer(double result, int64_t value) {
  if (result >= 9223372036854775808.0) {
    return 1;
  }
  if (result < -9223372036854775808.0) {
    return 1;
  }
  const int64_t back = int64_t(result);
  return (result >= 0.0 ? back > value : back < value) ? 1 : 0;
}

inline int fp_rounded_away_from(double result, double reference) {
  if (std::isnan(result) || std::isnan(reference)) {
    return 0;
  }
  return std::fabs(result) > std::fabs(reference) ? 1 : 0;
}

//=============================================================================
// Vector float results as on Xenon
//=============================================================================
// vmaddfp, vnmsubfp and their VMX128 forms round once: built with FMA that is
// the fused instruction. Otherwise it is in inline assembly on a CPU that has
// FMA, and on one without (or with REX_PPC_FMA=0) a multiply, then an add. That
// differs from the console by more than the last bit: a product that underflows
// is flushed before the add, a sum that cancels magnifies the product's
// rounding, and a product that overflows float32 gives an infinity or a NaN the
// single rounding does not.

// The lanes of r that are NaN: the first NaN of a, b, c, quieted, or the default
// QNaN when none is. Inlined into the [[unlikely]] branch of each site: an opaque
// call there would make the compiler reload the guest registers it keeps in host
// registers around every site, even when the branch is not taken.
[[gnu::always_inline]] inline simde__m128 vec_nan3(simde__m128 r, simde__m128 a, simde__m128 b,
                                                   simde__m128 c) {
  // Later blends win, so the operands go in reverse priority order.
  simde__m128 nan = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7FC00000));
  nan = simde_mm_blendv_ps(nan, c, simde_mm_cmpunord_ps(c, c));
  nan = simde_mm_blendv_ps(nan, b, simde_mm_cmpunord_ps(b, b));
  nan = simde_mm_blendv_ps(nan, a, simde_mm_cmpunord_ps(a, a));
  nan = simde_mm_or_ps(nan, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x00400000)));
  return simde_mm_blendv_ps(r, nan, simde_mm_cmpunord_ps(r, r));
}

inline bool vec_any_nan(simde__m128 r) {
  return simde_mm_movemask_ps(simde_mm_cmpunord_ps(r, r)) != 0;
}

// vaddfp, vsubfp, vmulfp128 and their VMX128 forms: the NaN lanes of r = a op b
// as the multiply-adds' (vA first). x86 gives the first source's NaN, and the
// compiler may swap the operands of an add or a multiply; its default NaN is
// negative.
inline simde__m128 vec_arith_result(simde__m128 r, simde__m128 a, simde__m128 b) {
  if (vec_any_nan(r)) [[unlikely]] {
    r = vec_nan3(r, a, b, b);
  }
  return r;
}

#if defined(__FMA__) || defined(__aarch64__) || defined(_M_ARM64)

inline simde__m128 vec_fma(simde__m128 a, simde__m128 c, simde__m128 b) {
  return simde_mm_fmadd_ps(a, c, b);
}

// a * c - b is a * c + (-b), exactly (simde's fmsub is not fused on ARM64).
inline simde__m128 vec_fms(simde__m128 a, simde__m128 c, simde__m128 b) {
  return simde_mm_fmadd_ps(
      a, c, simde_mm_xor_ps(b, simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));
}

#else

// The instruction itself is needed, so REX_PPC_FMA=1 does not stand in for it.
inline const bool vec_host_has_fma = fp_host_has_fma && fp_detect_host_fma();

inline simde__m128 vec_fma(simde__m128 a, simde__m128 c, simde__m128 b) {
  if (vec_host_has_fma) {
    __asm__("vfmadd213ps %2, %1, %0" : "+x"(a) : "x"(c), "x"(b));
    return a;
  }
  simde__m128 product = simde_mm_mul_ps(a, c);
  __asm__("" : "+x"(product));
  return simde_mm_add_ps(product, b);
}

inline simde__m128 vec_fms(simde__m128 a, simde__m128 c, simde__m128 b) {
  if (vec_host_has_fma) {
    __asm__("vfmsub213ps %2, %1, %0" : "+x"(a) : "x"(c), "x"(b));
    return a;
  }
  simde__m128 product = simde_mm_mul_ps(a, c);
  __asm__("" : "+x"(product));
  return simde_mm_sub_ps(product, b);
}

#endif

inline simde__m128 vec_flush_denormals(simde__m128 x);

// The lanes of r that are a NaN or +-FLT_MIN: the multiply-adds' rare cases.
inline bool vec_any_nan_or_min(simde__m128 r) {
  const simde__m128i magnitude =
      simde_mm_and_si128(simde_mm_castps_si128(r), simde_mm_set1_epi32(0x7FFFFFFF));
  const simde__m128 min = simde_mm_castsi128_ps(
      simde_mm_cmpeq_epi32(magnitude, simde_mm_set1_epi32(0x00800000)));
  return simde_mm_movemask_ps(simde_mm_or_ps(simde_mm_cmpunord_ps(r, r), min)) != 0;
}

// The lanes (all bits) whose exact a * c + b is below FLT_MIN in magnitude. The
// product of two floats is exact in a double (24 + 24 bits, far from its limits),
// and TwoSum gives the sum's rounding error exactly: the exact value is below
// FLT_MIN when the rounded sum is, or when it is FLT_MIN and the error points to
// zero.
[[gnu::always_inline]] inline simde__m128d vec_madd_tiny_half(simde__m128d a, simde__m128d c,
                                                              simde__m128d b) {
  const simde__m128d zero = simde_mm_setzero_pd();
  const simde__m128d min = simde_mm_set1_pd(0x1p-126);
  const simde__m128d p = simde_mm_mul_pd(a, c);
  const simde__m128d s = simde_mm_add_pd(p, b);
  const simde__m128d b_rounded = simde_mm_sub_pd(s, p);
  const simde__m128d error = simde_mm_add_pd(simde_mm_sub_pd(p, simde_mm_sub_pd(s, b_rounded)),
                                             simde_mm_sub_pd(b, b_rounded));
  const simde__m128d magnitude = simde_mm_andnot_pd(simde_mm_set1_pd(-0.0), s);
  const simde__m128d toward_zero =
      simde_mm_and_pd(simde_mm_cmpneq_pd(error, zero),
                      simde_mm_xor_pd(simde_mm_cmplt_pd(error, zero), simde_mm_cmplt_pd(s, zero)));
  return simde_mm_or_pd(simde_mm_cmplt_pd(magnitude, min),
                        simde_mm_and_pd(simde_mm_cmpeq_pd(magnitude, min), toward_zero));
}

// vmaddfp, vnmsubfp: the console flushes a result whose exact value is below
// FLT_MIN (tiny before rounding), x86 only one below it after rounding, so an
// exact value just under FLT_MIN comes out of the host as +-FLT_MIN (upstream
// hardware samples, tests/unit/ppc/upstream_multiply_add_test.cpp). Those lanes
// of r = a * c + b become the zero of their sign. Denormal operands are zeros,
// as the host's DAZ made them for r.
[[gnu::always_inline]] inline simde__m128 vec_madd_flush_tiny(simde__m128 r, simde__m128 a,
                                                              simde__m128 c, simde__m128 b) {
  a = vec_flush_denormals(a);
  c = vec_flush_denormals(c);
  b = vec_flush_denormals(b);
  const simde__m128d low = vec_madd_tiny_half(simde_mm_cvtps_pd(a), simde_mm_cvtps_pd(c),
                                              simde_mm_cvtps_pd(b));
  const simde__m128d high = vec_madd_tiny_half(simde_mm_cvtps_pd(simde_mm_movehl_ps(a, a)),
                                               simde_mm_cvtps_pd(simde_mm_movehl_ps(c, c)),
                                               simde_mm_cvtps_pd(simde_mm_movehl_ps(b, b)));
  const simde__m128 tiny = simde_mm_shuffle_ps(simde_mm_castpd_ps(low), simde_mm_castpd_ps(high),
                                               SIMDE_MM_SHUFFLE(2, 0, 2, 0));
  const simde__m128 sign = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)));
  const simde__m128 min = simde_mm_castsi128_ps(simde_mm_cmpeq_epi32(
      simde_mm_castps_si128(simde_mm_andnot_ps(sign, r)), simde_mm_set1_epi32(0x00800000)));
  return simde_mm_blendv_ps(r, simde_mm_and_ps(r, sign), simde_mm_and_ps(tiny, min));
}

// vD = vA * vC + vB.
inline simde__m128 vec_madd(simde__m128 a, simde__m128 c, simde__m128 b) {
  simde__m128 r = vec_fma(a, c, b);
  if (vec_any_nan_or_min(r)) [[unlikely]] {
    r = vec_madd_flush_tiny(r, a, c, b);
    r = vec_nan3(r, a, b, c);
  }
  return r;
}

// vD = -(vA * vC - vB). An exact zero comes out as -0; a NaN keeps its sign.
inline simde__m128 vec_nmsub(simde__m128 a, simde__m128 c, simde__m128 b) {
  const simde__m128 sign = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)));
  simde__m128 r = simde_mm_xor_ps(vec_fms(a, c, b), sign);
  if (vec_any_nan_or_min(r)) [[unlikely]] {
    // r is -(a * c) + b.
    r = vec_madd_flush_tiny(r, simde_mm_xor_ps(a, sign), c, b);
    r = vec_nan3(r, a, b, c);
  }
  return r;
}

// vmaxfp and vminfp: a denormal is a signed zero, a NaN operand is the result
// (vA first, quieted), and between zeros the maximum is -0 only when both are,
// the minimum +0 only when both are. maxps and minps give the second operand for
// a NaN and for equal values, and a denormal's bits: DAZ only makes their compare
// see a zero. The AND (maximum) or OR (minimum) of equal values is the right zero.
inline simde__m128 vec_flush_denormals(simde__m128 x) {
  const simde__m128i bits = simde_mm_castps_si128(x);
  const simde__m128i tiny = simde_mm_cmpeq_epi32(
      simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7F800000)), simde_mm_setzero_si128());
  return simde_mm_castsi128_ps(
      simde_mm_andnot_si128(simde_mm_and_si128(tiny, simde_mm_set1_epi32(0x7FFFFFFF)), bits));
}

// The lanes where a or b is a NaN: a quieted if it is one, else b quieted.
[[gnu::always_inline]] inline simde__m128 vec_nan2(simde__m128 r, simde__m128 a, simde__m128 b) {
  const simde__m128 quiet = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x00400000));
  const simde__m128 nan = simde_mm_blendv_ps(simde_mm_or_ps(b, quiet), simde_mm_or_ps(a, quiet),
                                             simde_mm_cmpunord_ps(a, a));
  return simde_mm_blendv_ps(r, nan, simde_mm_cmpunord_ps(a, b));
}

inline simde__m128 vec_max(simde__m128 a, simde__m128 b) {
  simde__m128 r = vec_flush_denormals(
      simde_mm_blendv_ps(simde_mm_max_ps(a, b), simde_mm_and_ps(a, b), simde_mm_cmpeq_ps(a, b)));
  if (simde_mm_movemask_ps(simde_mm_cmpunord_ps(a, b)) != 0) [[unlikely]] {
    r = vec_nan2(r, a, b);
  }
  return r;
}

inline simde__m128 vec_min(simde__m128 a, simde__m128 b) {
  simde__m128 r = vec_flush_denormals(
      simde_mm_blendv_ps(simde_mm_min_ps(a, b), simde_mm_or_ps(a, b), simde_mm_cmpeq_ps(a, b)));
  if (simde_mm_movemask_ps(simde_mm_cmpunord_ps(a, b)) != 0) [[unlikely]] {
    r = vec_nan2(r, a, b);
  }
  return r;
}

// vmsum3fp128 and vmsum4fp128, fast form (codegen `fast_dot_products = true`;
// the default is the console's, vec_msum3 / vec_msum4 below). It is 1 ulp off
// the console (rounded to nearest where the console truncates) in about three
// of four sampled cases, and its NaN and overflow results differ.
//
// Each product is exact in float64, summed in the
// order Xenia's x64 backend matched to the console (DOT_PRODUCT_3/4),
// (x + z) + y or (x + z) + (y + w), and rounded once to float32, in all four
// lanes. Host lanes 3..0 hold the guest's x, y, z, w. Denormal inputs and results
// are zeros through the flush mode the generated code sets.
//
// A result that is not finite is redone in the order of that sequence, so that it
// doesn't depend on the order the compiler gives commutative operations: the
// first NaN operand of each step goes through, quieted; a NaN a step makes is
// 0x7FC00000; finite inputs that overflow float32 give 0x7FC00000 too, and an
// infinity from the inputs is kept.
[[gnu::noinline, gnu::cold, gnu::const]] inline uint32_t vec_msum_special(simde__m128 a,
                                                                          simde__m128 b,
                                                                          bool four) {
  alignas(16) uint32_t av[4], bv[4];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(av), simde_mm_castps_si128(a));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(bv), simde_mm_castps_si128(b));
  struct Term {
    double value;
    uint32_t nan;  // The result's bits when it is a NaN, else 0.
  };
  const auto is_nan = [](uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; };
  const auto value = [](uint32_t x) {
    if ((x & 0x7F800000u) == 0) {
      x &= 0x80000000u;  // A denormal input is a signed zero.
    }
    return double(std::bit_cast<float>(x));
  };
  const auto product = [&](int lane) -> Term {
    if (is_nan(av[lane])) {
      return {0.0, av[lane] | 0x00400000u};
    }
    if (is_nan(bv[lane])) {
      return {0.0, bv[lane] | 0x00400000u};
    }
    const double p = value(av[lane]) * value(bv[lane]);
    return p != p ? Term{0.0, 0x7FC00000u} : Term{p, 0};
  };
  const auto add = [](Term x, Term y) -> Term {
    if (x.nan) {
      return x;
    }
    if (y.nan) {
      return y;
    }
    const double s = x.value + y.value;
    return s != s ? Term{0.0, 0x7FC00000u} : Term{s, 0};
  };
  const Term xz = add(product(3), product(1));
  const Term total = four ? add(xz, add(product(2), product(0))) : add(xz, product(2));
  if (total.nan) {
    return total.nan;
  }
  const float narrow = float(total.value);
  if (std::isinf(narrow) && std::isfinite(total.value)) {
    return 0x7FC00000u;
  }
  return std::bit_cast<uint32_t>(narrow);
}

inline simde__m128d vec_msum_pairs(simde__m128 a, simde__m128 b, simde__m128d& yx) {
  const simde__m128d wz = simde_mm_mul_pd(simde_mm_cvtps_pd(a), simde_mm_cvtps_pd(b));
  yx = simde_mm_mul_pd(simde_mm_cvtps_pd(simde_mm_movehl_ps(a, a)),
                       simde_mm_cvtps_pd(simde_mm_movehl_ps(b, b)));
  return simde_mm_add_pd(yx, wz);  // {y + w, x + z}
}

// A plain conversion: simde's cvtsd_ss is fcvtxn on ARM64, which rounds to odd
// and never overflows.
inline simde__m128 vec_msum_result(simde__m128d sum, simde__m128 a, simde__m128 b, bool four) {
  const float narrow = float(simde_mm_cvtsd_f64(sum));
  if ((std::bit_cast<uint32_t>(narrow) << 1) >= 0xFF000000u) [[unlikely]] {
    return simde_mm_castsi128_ps(simde_mm_set1_epi32(int(vec_msum_special(a, b, four))));
  }
  return simde_mm_set1_ps(narrow);
}

inline simde__m128 vec_msum3_fast(simde__m128 a, simde__m128 b) {
  simde__m128d yx;
  const simde__m128d pairs = vec_msum_pairs(a, b, yx);
  return vec_msum_result(simde_mm_add_sd(simde_mm_unpackhi_pd(pairs, pairs), yx), a, b, false);
}

inline simde__m128 vec_msum4_fast(simde__m128 a, simde__m128 b) {
  simde__m128d yx;
  const simde__m128d pairs = vec_msum_pairs(a, b, yx);
  return vec_msum_result(simde_mm_add_sd(simde_mm_unpackhi_pd(pairs, pairs), pairs), a, b, true);
}

// vmsum3fp128 and vmsum4fp128 as the console computes them. Ported from
// upstream rexglue (include/rex/ppc/fpu.h, DotProductReference and
// DotProductFinite, 9740cacf and later; Copyright (c) 2026 Tom Clay, BSD
// 3-Clause), which matches all 747 of its console samples (tests/unit/ppc/
// upstream_cases). The products are exact; they are added as truncated fixed
// point with 29 fractional bits below the largest product, in the sign most of
// the terms have, then normalized without rounding. A NaN operand or an
// infinity times zero gives the default QNaN; infinities of one sign give that
// infinity, of both signs the QNaN. The result goes to all four lanes. flush
// takes denormal operands as zeros (VSCR[NJ] set, which generated code assumes).
namespace vec_dot_detail {

struct Term {
  bool negative;
  uint64_t mantissa;
  int32_t exponent;
};

inline Term MakeTerm(uint32_t a, uint32_t b, bool flush) {
  const auto split = [flush](uint32_t bits, uint32_t& mantissa, int32_t& exponent) {
    const uint32_t biased = (bits >> 23) & 0xFFu;
    mantissa = biased == 0 ? 0 : ((bits & 0x7FFFFFu) | 0x800000u);
    exponent = static_cast<int32_t>(biased) - 127;
    if (biased == 0 && !flush && (bits & 0x7FFFFFu) != 0) {
      const int32_t shift = std::countl_zero(bits & 0x7FFFFFu) - 8;
      mantissa = (bits & 0x7FFFFFu) << shift;
      exponent = -126 - shift;
    }
  };
  uint32_t mantissa_a, mantissa_b;
  int32_t exponent_a, exponent_b;
  split(a, mantissa_a, exponent_a);
  split(b, mantissa_b, exponent_b);
  return {((a ^ b) & 0x80000000u) != 0, uint64_t{mantissa_a} * mantissa_b,
          exponent_a + exponent_b - 46};
}

// The fixed point sum (bits from `lowest` up) to the result bits.
inline uint32_t Finish(int64_t sum, bool majority_negative, int32_t lowest) {
  const int64_t flip = sum >> 63;
  const uint32_t sign =
      (majority_negative ? 0x80000000u : 0u) ^ (static_cast<uint32_t>(flip) & 0x80000000u);
  const uint64_t magnitude = static_cast<uint64_t>(sum ^ flip);
  if (magnitude == 0) {
    return 0;
  }
  const uint32_t width = static_cast<uint32_t>(std::bit_width(magnitude));
  const int32_t exponent = lowest + static_cast<int32_t>(width) - 1;
  const uint32_t top = static_cast<uint32_t>((magnitude << (64 - width)) >> 40);
  if (static_cast<uint32_t>(exponent + 126) < 254u) [[likely]] {
    return sign | (static_cast<uint32_t>(exponent + 127) << 23) | (top & 0x7FFFFFu);
  }
  if (exponent < -127) {
    return sign;
  }
  if (exponent == -127) {
    return sign | (top & 0x7FFFFFu);
  }
  if (exponent > 128) {
    return sign | 0x7FC00000u;
  }
  // Exponent 128: the console leaves the bits as they are (2 * FLT_MAX gives
  // 0x7FFFFFFF).
  return sign | (static_cast<uint32_t>(exponent + 127) << 23) | (top & 0x7FFFFFu);
}

// Any operands, in guest order (element i is host lane 3 - i).
template <uint32_t Terms>
inline uint32_t Reference(const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b,
                          bool flush) {
  bool any_nan = false;
  bool any_invalid = false;
  uint32_t infinities = 0;
  uint32_t negative_infinities = 0;
  std::array<Term, Terms> terms{};
  for (uint32_t i = 0; i < Terms; ++i) {
    const uint32_t x = flush && (a[i] & 0x7F800000u) == 0 ? (a[i] & 0x80000000u) : a[i];
    const uint32_t y = flush && (b[i] & 0x7F800000u) == 0 ? (b[i] & 0x80000000u) : b[i];
    const bool nan_x = (x & 0x7FFFFFFFu) > 0x7F800000u;
    const bool nan_y = (y & 0x7FFFFFFFu) > 0x7F800000u;
    const bool inf_x = (x & 0x7FFFFFFFu) == 0x7F800000u;
    const bool inf_y = (y & 0x7FFFFFFFu) == 0x7F800000u;
    const bool zero_x = (x & 0x7FFFFFFFu) == 0;
    const bool zero_y = (y & 0x7FFFFFFFu) == 0;
    any_nan |= nan_x || nan_y;
    any_invalid |= (inf_x && zero_y) || (inf_y && zero_x);
    if ((inf_x || inf_y) && !zero_x && !zero_y) {
      ++infinities;
      negative_infinities += ((x ^ y) & 0x80000000u) != 0 ? 1 : 0;
    }
    terms[i] = MakeTerm(x, y, flush);
  }
  if (any_nan || any_invalid) {
    return 0x7FC00000u;
  }
  if (infinities != 0) {
    if (negative_infinities == 0) {
      return 0x7F800000u;
    }
    return negative_infinities == infinities ? 0xFF800000u : 0x7FC00000u;
  }
  uint32_t negatives = 0;
  bool any_live = false;
  int32_t highest = 0;
  for (const Term& term : terms) {
    negatives += term.negative ? 1 : 0;
    if (term.mantissa != 0 && (!any_live || term.exponent > highest)) {
      highest = term.exponent;
      any_live = true;
    }
  }
  if (!any_live) {
    return negatives == Terms ? 0x80000000u : 0u;
  }
  const bool majority_negative = 2 * negatives > Terms;
  const int32_t lowest = highest + 47 - 29;
  int64_t sum = 0;
  for (const Term& term : terms) {
    uint64_t aligned = 0;
    if (term.mantissa != 0) {
      const int32_t shift = lowest - term.exponent;
      aligned = shift >= 64 ? 0 : (term.mantissa >> shift);
    }
    const int64_t value = static_cast<int64_t>(aligned);
    sum += term.negative == majority_negative ? value : ~value;
  }
  return Finish(sum, majority_negative, lowest);
}

// Finite, normal-or-zero operands (no infinity, NaN, or with flush clear, a
// denormal) in the highest Terms host lanes, as Reference. The products scaled
// to the fixed point are below 2^31, so they go through float64 and truncate
// to int32 exactly.
template <uint32_t Terms>
inline uint32_t Finite(simde__m128i words_a, simde__m128i words_b, simde__m128i biased_a,
                       simde__m128i biased_b) {
  constexpr int32_t kNone = -0x40000000;
  const auto set = [](uint32_t value) { return simde_mm_set1_epi32(static_cast<int32_t>(value)); };
  const simde__m128i used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const simde__m128i zero = simde_mm_setzero_si128();
  const simde__m128i live_a = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_a, zero), used);
  const simde__m128i live_b = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_b, zero), used);
  const simde__m128i mantissa_a = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_a, set(0x7FFFFFu)), set(0x800000u)), live_a);
  const simde__m128i mantissa_b = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_b, set(0x7FFFFFu)), set(0x800000u)), live_b);
  const simde__m128i exponent = simde_mm_sub_epi32(simde_mm_add_epi32(biased_a, biased_b), set(300));
  const simde__m128i live = simde_mm_and_si128(live_a, live_b);
  const simde__m128i candidate =
      simde_mm_blendv_epi8(set(static_cast<uint32_t>(kNone)), exponent, live);
  simde__m128i highest_lanes =
      simde_mm_max_epi32(candidate, simde_mm_shuffle_epi32(candidate, 0x4E));
  highest_lanes = simde_mm_max_epi32(highest_lanes, simde_mm_shuffle_epi32(highest_lanes, 0xB1));
  const int32_t highest = simde_mm_cvtsi128_si32(highest_lanes);
  const simde__m128i negative = simde_mm_srai_epi32(simde_mm_xor_si128(words_a, words_b), 31);
  const uint32_t negatives = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(
      simde_mm_movemask_ps(simde_mm_castsi128_ps(simde_mm_and_si128(negative, used))))));
  if (highest == kNone) {
    return negatives == Terms ? 0x80000000u : 0u;
  }
  const bool majority = 2 * negatives > Terms;
  const simde__m128i minority = simde_mm_and_si128(
      simde_mm_xor_si128(negative, simde_mm_set1_epi32(-static_cast<int32_t>(majority))), used);
  const int32_t lowest = highest + 47 - 29;
  // 2^(exponent - lowest) as float64 bits; the field stays within 1..2046 for
  // every exponent two finite floats give.
  const simde__m128i field = simde_mm_add_epi32(
      simde_mm_sub_epi32(exponent, simde_mm_set1_epi32(lowest)), simde_mm_set1_epi32(1023));
  const auto upper = [](simde__m128i v) { return simde_mm_shuffle_epi32(v, 0xEE); };
  const simde__m128d scale_low =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(field), 52));
  const simde__m128d scale_high =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(upper(field)), 52));
  const simde__m128d product_low =
      simde_mm_mul_pd(simde_mm_cvtepi32_pd(mantissa_a), simde_mm_cvtepi32_pd(mantissa_b));
  const simde__m128d product_high = simde_mm_mul_pd(simde_mm_cvtepi32_pd(upper(mantissa_a)),
                                                    simde_mm_cvtepi32_pd(upper(mantissa_b)));
  const simde__m128i aligned =
      simde_mm_unpacklo_epi64(simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_low, scale_low)),
                              simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_high, scale_high)));
  const simde__m128i terms = simde_mm_xor_si128(aligned, minority);
  const simde__m128i pair =
      simde_mm_add_epi64(simde_mm_cvtepi32_epi64(terms), simde_mm_cvtepi32_epi64(upper(terms)));
  const int64_t total = simde_mm_cvtsi128_si64(pair) + simde_mm_extract_epi64(pair, 1);
  return Finish(total, majority, lowest);
}

template <uint32_t Terms>
inline simde__m128 Dot(simde__m128 a, simde__m128 b, bool flush) {
  const simde__m128i words_a = simde_mm_castps_si128(a);
  const simde__m128i words_b = simde_mm_castps_si128(b);
  const simde__m128i used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const simde__m128i exponent_mask = simde_mm_set1_epi32(0x7F800000);
  const simde__m128i biased_a = simde_mm_srli_epi32(simde_mm_and_si128(words_a, exponent_mask), 23);
  const simde__m128i biased_b = simde_mm_srli_epi32(simde_mm_and_si128(words_b, exponent_mask), 23);
  const simde__m128i infinite = simde_mm_set1_epi32(0xFF);
  simde__m128i special = simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, infinite),
                                           simde_mm_cmpeq_epi32(biased_b, infinite));
  if (!flush) [[unlikely]] {
    const simde__m128i zero = simde_mm_setzero_si128();
    const simde__m128i fraction_mask = simde_mm_set1_epi32(0x7FFFFF);
    const simde__m128i denormal_a = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_a, fraction_mask), zero),
        simde_mm_cmpeq_epi32(biased_a, zero));
    const simde__m128i denormal_b = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_b, fraction_mask), zero),
        simde_mm_cmpeq_epi32(biased_b, zero));
    special = simde_mm_or_si128(special, simde_mm_or_si128(denormal_a, denormal_b));
  }
  special = simde_mm_and_si128(special, used);
  uint32_t result;
  if (simde_mm_movemask_epi8(special) == 0) [[likely]] {
    result = Finite<Terms>(words_a, words_b, biased_a, biased_b);
  } else {
    alignas(16) std::array<uint32_t, 4> host_a;
    alignas(16) std::array<uint32_t, 4> host_b;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_a.data()), words_a);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_b.data()), words_b);
    std::array<uint32_t, 4> guest_a{}, guest_b{};
    for (uint32_t i = 0; i < 4; ++i) {
      guest_a[i] = host_a[3 - i];
      guest_b[i] = host_b[3 - i];
    }
    result = Reference<Terms>(guest_a, guest_b, flush);
  }
  return simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<int32_t>(result)));
}

}  // namespace vec_dot_detail

inline simde__m128 vec_msum3(simde__m128 a, simde__m128 b, bool flush = true) {
  return vec_dot_detail::Dot<3>(a, b, flush);
}

inline simde__m128 vec_msum4(simde__m128 a, simde__m128 b, bool flush = true) {
  return vec_dot_detail::Dot<4>(a, b, flush);
}

// One field of vpkd3d128: a lane in the 3.0 + k * 2^-22 form (raw float bits)
// clamped to its field's range [lo, hi], floats in [2, 4) whose low bits are
// the field. A lane the clamp moves sets VSCR[SAT], even one the mask doesn't
// write. A NaN packs as nan_bits, the field's minimum (0x8000 for a short),
// which unpacks back to a NaN, or 0 in the unsigned W fields, and sets nothing
// (xenia-canary c0ea2c443, 6fc314d26; VSCR[SAT] measured on a console,
// xenia-edge 4bcbae774).
[[gnu::always_inline]] inline uint32_t pack_d3d_field(uint32_t bits, uint32_t lo, uint32_t hi,
                                                      uint32_t nan_bits, uint8_t& sat) {
  const float value = std::bit_cast<float>(bits);
  if (value != value) {
    return nan_bits;
  }
  if (value < std::bit_cast<float>(lo)) {
    sat = 1;
    return lo;
  }
  if (value > std::bit_cast<float>(hi)) {
    sat = 1;
    return hi;
  }
  return bits;
}

}  // namespace rex::ppc

//=============================================================================
// Global Aliases for Generated Code
//=============================================================================
// Vector mask tables accessible from global scope for generated code
using rex::ppc::VectorMaskL;
using rex::ppc::VectorMaskR;
using rex::ppc::VectorShiftTableL;
using rex::ppc::VectorShiftTableR;
