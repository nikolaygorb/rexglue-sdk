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
// SIMD Helper Functions
//=============================================================================

// Unsigned 32-bit saturating add
inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
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
// Multiply-add NaN rules
//=============================================================================
// fmadd/fmsub/fnmadd/fnmsub (and their single forms), vmaddfp and vnmsubfp
// compute D = +-(A * C +- B). When an operand is a NaN, the hardware returns
// the first NaN operand in A, B, C order, quieted, with its sign left alone;
// that is not the host's order. An invalid operation with no NaN operand
// (infinity times zero) gives the default QNaN, which is positive on PPC and
// negative on x86. The negated forms never negate a NaN result.

// Fixes up the result `r` of a scalar multiply-add with operands a, b, c.
inline double fp_madd_result(double r, double a, double b, double c, bool negate) {
  if (!std::isnan(r)) [[likely]] {
    return negate ? -r : r;
  }
  constexpr uint64_t kQuietBit = 0x0008000000000000ull;
  for (double x : {a, b, c}) {
    if (std::isnan(x)) {
      return std::bit_cast<double>(std::bit_cast<uint64_t>(x) | kQuietBit);
    }
  }
  return std::bit_cast<double>(0x7FF8000000000000ull);
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
template <typename... Operands>
inline bool fp_single_denormal_operand(Operands... operands) {
  bool any_denormal = false;
  bool any_nonfinite = false;
  for (double x : {double(operands)...}) {
    const uint64_t magnitude = std::bit_cast<uint64_t>(x) & 0x7FFFFFFFFFFFFFFFull;
    any_denormal |= magnitude - 1 < 0x000FFFFFFFFFFFFFull;
    any_nonfinite |= magnitude >= 0x7FF0000000000000ull;
  }
  return any_denormal && !any_nonfinite;
}

inline constexpr double kFpDefaultQNaN = std::bit_cast<double>(0x7FF8000000000000ull);

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

// vD = vA * vC + vB.
inline simde__m128 vec_madd(simde__m128 a, simde__m128 c, simde__m128 b) {
  simde__m128 r = vec_fma(a, c, b);
  if (vec_any_nan(r)) [[unlikely]] {
    r = vec_nan3(r, a, b, c);
  }
  return r;
}

// vD = -(vA * vC - vB). An exact zero comes out as -0; a NaN keeps its sign.
inline simde__m128 vec_nmsub(simde__m128 a, simde__m128 c, simde__m128 b) {
  simde__m128 r = simde_mm_xor_ps(vec_fms(a, c, b),
                                  simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
  if (vec_any_nan(r)) [[unlikely]] {
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

// vmsum3fp128 and vmsum4fp128: each product is exact in float64, summed in the
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

inline simde__m128 vec_msum3(simde__m128 a, simde__m128 b) {
  simde__m128d yx;
  const simde__m128d pairs = vec_msum_pairs(a, b, yx);
  return vec_msum_result(simde_mm_add_sd(simde_mm_unpackhi_pd(pairs, pairs), yx), a, b, false);
}

inline simde__m128 vec_msum4(simde__m128 a, simde__m128 b) {
  simde__m128d yx;
  const simde__m128d pairs = vec_msum_pairs(a, b, yx);
  return vec_msum_result(simde_mm_add_sd(simde_mm_unpackhi_pd(pairs, pairs), pairs), a, b, true);
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
