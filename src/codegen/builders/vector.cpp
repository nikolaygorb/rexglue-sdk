/**
 * @file        rexcodegen/builders/vector.cpp
 * @brief       PPC vector instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <cmath>

#include <simde/x86/sse.h>

#include <rex/logging.h>

#include "../codegen_logging.h"

#include <ppc.h>

namespace rex::codegen {

//=============================================================================
// SIMD Constants Documentation
//=============================================================================
// This file uses several magic constants from Intel SSE/SSE4 intrinsics.
// Here are the key constants and their meanings:
//
// === Floating-Point Sign Bit ===
// 0x80000000: IEEE 754 sign bit mask for 32-bit float
//             Used for negation via XOR and sign extraction
//
// === IEEE 754 Single Precision Exponent ===
// 0x7f800000: Exponent field mask (bits 23-30) for float
// 0x7FFFFFFF: Magnitude mask (clears sign bit)
// 0x3F800000: IEEE 754 representation of 1.0f (used for OR-ing in exponent)
// 0x40400000: IEEE 754 representation of 3.0f (D3D NORMSHORT encoding bias)
//
// === Half-Float (FP16) Conversion ===
// 0x7C00: FP16 exponent mask
// 0x1C000: Exponent bias adjustment for FP16->FP32 conversion
// 0x8000: FP16 sign bit
// 0x03FF: FP16 mantissa mask
// 0x7FFF: FP16 positive infinity/max value
//
// === Packed Integer Masks ===
// 0x3FF: 10-bit mask for NORMPACKED32 format (10 bits per component)
// 0xFFFFF: 20-bit mask for NORMPACKED64 format
// 0x1F: 5-bit mask for shift amounts (shifts are mod 32)
// 0xFF: 8-bit mask for byte values
// 0xFFFF: 16-bit mask for halfword values
//
// === D3D Color Format ===
// 0x404000FF: D3D color packing constant (ARGB8888)
//=============================================================================

//=============================================================================
// Vector Floating Point Arithmetic
//=============================================================================

// vD = vA op vB, with the NaN lanes of rex::ppc::vec_arith_result.
static void emit_vec_fp_arith(BuilderContext& ctx, const char* simd_op) {
  ctx.emit_set_flush_mode(true);
  const auto a = fmt::format("simde_mm_load_ps({}.f32)", ctx.v(ctx.insn.operands[1]));
  const auto b = fmt::format("simde_mm_load_ps({}.f32)", ctx.v(ctx.insn.operands[2]));
  ctx.println(
      "\tsimde_mm_store_ps({0}.f32, rex::ppc::vec_arith_result(simde_mm_{1}_ps({2}, {3}), {2}, "
      "{3}));",
      ctx.v(ctx.insn.operands[0]), simd_op, a, b);
}

bool build_vaddfp(BuilderContext& ctx) {
  emit_vec_fp_arith(ctx, "add");
  return true;
}

bool build_vsubfp(BuilderContext& ctx) {
  emit_vec_fp_arith(ctx, "sub");
  return true;
}

bool build_vmulfp128(BuilderContext& ctx) {
  emit_vec_fp_arith(ctx, "mul");
  return true;
}

// vD = helper(vA, vC, vB), one of the multiply-adds of rex/ppc/intrinsics.h.
// Operands are (vD, vA, vC, vB) in encoding order; vmaddfp128, vmaddcfp128 and
// vnmsubfp128 come here with theirs in that order too.
static void emit_vec_madd(BuilderContext& ctx, const char* helper) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::{}(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32)));",
      ctx.v(ctx.insn.operands[0]), helper, ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      ctx.v(ctx.insn.operands[3]));
}

bool build_vmaddfp(BuilderContext& ctx) {
  emit_vec_madd(ctx, "vec_madd");
  return true;
}

bool build_vnmsubfp(BuilderContext& ctx) {
  emit_vec_madd(ctx, "vec_nmsub");
  return true;
}

// vD = helper(vA, vB), with a helper of rex/ppc/intrinsics.h.
static void emit_vec_fp_helper(BuilderContext& ctx, const char* helper) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::{}(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      ctx.v(ctx.insn.operands[0]), helper, ctx.v(ctx.insn.operands[1]),
      ctx.v(ctx.insn.operands[2]));
}

bool build_vmaxfp(BuilderContext& ctx) {
  emit_vec_fp_helper(ctx, "vec_max");
  return true;
}

bool build_vminfp(BuilderContext& ctx) {
  emit_vec_fp_helper(ctx, "vec_min");
  return true;
}

// vrefp, vrsqrtefp, vexptefp, vlogefp and their VMX128 forms: the console's
// estimates, lane by lane (rex/ppc/estimates.h). Integer arithmetic on the
// bits, so the result does not depend on the flush mode (a denormal input is
// a zero, as with VSCR[NJ] set).
static void emit_vec_estimate(BuilderContext& ctx, const char* estimate) {
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, "
      "rex::ppc::vec_estimate<rex::ppc::{}>(simde_mm_load_ps({}.f32)));",
      ctx.v(ctx.insn.operands[0]), estimate, ctx.v(ctx.insn.operands[1]));
}

bool build_vrefp(BuilderContext& ctx) {
  emit_vec_estimate(ctx, "estimate_reciprocal");
  return true;
}

bool build_vrsqrtefp(BuilderContext& ctx) {
  emit_vec_estimate(ctx, "estimate_reciprocal_sqrt");
  return true;
}

bool build_vexptefp(BuilderContext& ctx) {
  emit_vec_estimate(ctx, "estimate_pow2");
  return true;
}

bool build_vlogefp(BuilderContext& ctx) {
  emit_vec_estimate(ctx, "estimate_log2");
  return true;
}

//=============================================================================
// Vector Dot Products
//=============================================================================

// The console's fixed point sum by default; the float64 sum with
// fast_dot_products (rex/ppc/intrinsics.h).
bool build_vmsum3fp128(BuilderContext& ctx) {
  emit_vec_fp_helper(ctx, ctx.config().fastDotProducts ? "vec_msum3_fast" : "vec_msum3");
  return true;
}

bool build_vmsum4fp128(BuilderContext& ctx) {
  emit_vec_fp_helper(ctx, ctx.config().fastDotProducts ? "vec_msum4_fast" : "vec_msum4");
  return true;
}

//=============================================================================
// Vector Rounding
//=============================================================================

bool build_vrfim(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfin(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfip(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfiz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

//=============================================================================
// Vector Integer Arithmetic
//=============================================================================

bool build_vaddsbs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epi8", "s8");
  return true;
}

bool build_vaddshs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epi16", "s16");
  return true;
}

bool build_vaddsws(BuilderContext& ctx) {
  // vaddsws: Vector Add Signed Word Saturate
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);

  ctx.println("\t{{");
  // No direct SSE intrinsic, so use overflow detection and blend
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  ctx.println("\t\tsimde__m128i sum = simde_mm_add_epi32(a, b);");
  // Overflow if: (a ^ sum) & (b ^ sum) has MSB set (signs of a,b match but sum differs)
  ctx.println(
      "\t\tsimde__m128i overflow = simde_mm_and_si128(simde_mm_xor_si128(a, sum), "
      "simde_mm_xor_si128(b, sum));");
  // Saturation value: if a positive (MSB=0), use INT32_MAX;
  // if negative, use INT32_MIN (a >> 31) gives all 1s if negative, all 0s
  // if positive XOR with 0x7FFFFFFF: negative -> 0x80000000, positive -> 0x7FFFFFFF
  ctx.println(
      "\t\tsimde__m128i sat_val = simde_mm_xor_si128(simde_mm_srai_epi32(a, 31), "
      "simde_mm_set1_epi32(0x7FFFFFFF));");
  // Blend: select sat_val where overflow MSB is set, else sum
  ctx.println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_castps_si128(simde_mm_blendv_ps("
      "simde_mm_castsi128_ps(sum), "
      "simde_mm_castsi128_ps(sat_val), "
      "simde_mm_castsi128_ps(overflow))));",
      vD);
  ctx.println("\t}}");
  return true;
}

bool build_vaddubm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi8", "u8");
  return true;
}

bool build_vaddubs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epu8", "u8");
  return true;
}

bool build_vadduhm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi16", "u16");
  return true;
}

bool build_vadduwm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi32", "u32");
  return true;
}

bool build_vadduws(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simde_mm_adds_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vadduhs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epu16", "u16");
  return true;
}

bool build_vsubsws(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simde_mm_subs_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsububm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi8", "u8");
  return true;
}

bool build_vsububs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epu8", "u8");
  return true;
}

bool build_vsubuws(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*) {}.u32), "
      "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
      ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsubuhs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epu16", "u16");
  return true;
}

bool build_vsubuhm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi16", "u16");
  return true;
}

bool build_vsubuwm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi32", "u32");
  return true;
}

bool build_vmaxsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi32", "s32");
  return true;
}

bool build_vmaxsh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi16", "s16");
  return true;
}

bool build_vmaxsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi8", "s8");
  return true;
}

bool build_vminsh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi16", "s16");
  return true;
}

bool build_vminsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi8", "s8");
  return true;
}

bool build_vminsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi32", "s32");
  return true;
}

bool build_vmaxuh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu16", "u16");
  return true;
}

bool build_vminuh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu16", "u16");
  return true;
}

bool build_vminuw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu32", "u32");
  return true;
}

bool build_vsubsbs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epi8", "s8");
  return true;
}

bool build_vmaxub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu8", "u8");
  return true;
}

bool build_vminub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu8", "u8");
  return true;
}

bool build_vsubshs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epi16", "s16");
  return true;
}

//=============================================================================
// Vector Average
//=============================================================================

bool build_vavgsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_avg_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "rex::ppc::simde_mm_avg_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgsw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "rex::ppc::simde_mm_avg_epi32("
      "simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("avg_epu8", "u8");
  return true;
}

bool build_vavguh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("avg_epu16", "u16");
  return true;
}

//=============================================================================
// Vector Logical
//=============================================================================

bool build_vand(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("and_si128", "u8");
  return true;
}

bool build_vandc128(BuilderContext& ctx) {
  // vandc128: vD = vA & ~vB (simde_mm_andnot_si128 has reversed operand semantics)
  ctx.emit_vec_int_binary_swapped("andnot_si128", "u8");
  return true;
}

bool build_vandc(BuilderContext& ctx) {
  // vandc: vD = vA & ~vB (simde_mm_andnot_si128 has reversed operand semantics)
  ctx.emit_vec_int_binary_swapped("andnot_si128", "u8");
  return true;
}

bool build_vor(BuilderContext& ctx) {
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.v(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.println(
        "simde_mm_or_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  else
    ctx.println("simde_mm_load_si128((simde__m128i*){}.u8));", ctx.v(ctx.insn.operands[1]));

  return true;
}

bool build_vxor(BuilderContext& ctx) {
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.v(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.println(
        "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  else
    ctx.println("simde_mm_setzero_si128());");

  return true;
}

bool build_vnor(BuilderContext& ctx) {
  // vnor: vD = ~(vA | vB)
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_xor_si128("
      "simde_mm_or_si128("
      "simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_set1_epi32(-1)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsel(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[3]), ctx.v(ctx.insn.operands[1]),
      ctx.v(ctx.insn.operands[3]), ctx.v(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Vector Compare
//=============================================================================

bool build_vcmpbfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  // vcmpbfp: Vector Compare Bounds Floating Point
  // For each element i:
  //   bit 0 (0x80000000) = 1 if vA[i] > vB[i]
  //   bit 1 (0x40000000) = 1 if vA[i] < -vB[i]
  // A NaN in either operand is out of bounds both ways, so both bits are set.
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128 a = simde_mm_load_ps({}.f32);", ctx.v(ctx.insn.operands[1]));
  ctx.println("\t\tsimde__m128 b = simde_mm_load_ps({}.f32);", ctx.v(ctx.insn.operands[2]));
  ctx.println(
      "\t\tsimde__m128 sign = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)));");
  ctx.println("\t\tsimde__m128 unord = simde_mm_cmpunord_ps(a, b);");
  ctx.println("\t\tsimde__m128 gt = simde_mm_or_ps(simde_mm_cmpgt_ps(a, b), unord);");
  ctx.println(
      "\t\tsimde__m128 lt = simde_mm_or_ps(simde_mm_cmplt_ps(a, simde_mm_xor_ps(b, sign)), "
      "unord);");
  ctx.println(
      "\t\tsimde__m128 r = simde_mm_or_ps(simde_mm_and_ps(gt, sign), "
      "simde_mm_and_ps(lt, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x40000000))));");
  ctx.println("\t\tsimde_mm_store_ps({}.f32, r);", ctx.v(ctx.insn.operands[0]));
  // CR6[2] is set when every element is in bounds; vcmpbfp writes no other
  // bit. movemask_ps only sees bit 31, so bit 30 is shifted up into it first.
  if (isRecordForm(ctx.insn)) {
    ctx.println(
        "\t\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_or_si128(simde_mm_castps_si128(r), "
        "simde_mm_slli_epi32(simde_mm_castps_si128(r), 1))), 0xF);",
        ctx.cr(6));
    ctx.println("\t\t{}.lt = 0;", ctx.cr(6));
  }
  ctx.println("\t}}");
  return true;
}

bool build_vcmpeqfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpeq");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpeq_epi8", "u8");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgefp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpge");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpgt");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtub(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtuh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtuw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_cmpgt_epi32(simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((int32_t)0x80000000)), "
      "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((int32_t)0x80000000))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));

  if (isRecordForm(ctx.insn))
    ctx.println(
        "\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.cr(6), ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpgt_epi8", "u8");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpgt_epi32", "u32");
  if (isRecordForm(ctx.insn))
    ctx.println(
        "\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.cr(6), ctx.v(ctx.insn.operands[0]));
  return true;
}

//=============================================================================
// Vector Conversion
//=============================================================================

bool build_vctsxs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.s32, rex::ppc::simde_mm_vctsxs(",
            ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0)
    ctx.println("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))));",
                ctx.v(ctx.insn.operands[1]), 1u << ctx.insn.operands[2]);
  else
    ctx.println("simde_mm_load_ps({}.f32)));", ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vcfsx(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_ps({}.f32, ", ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const float value = std::ldexp(1.0f, -static_cast<int32_t>(ctx.insn.operands[2]));
    ctx.println(
        "simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.v(ctx.insn.operands[1]), *reinterpret_cast<const uint32_t*>(&value));
  } else {
    ctx.println("simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.v(ctx.insn.operands[1]));
  }
  return true;
}

bool build_vcfux(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_ps({}.f32, ", ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const float value = std::ldexp(1.0f, -static_cast<int32_t>(ctx.insn.operands[2]));
    ctx.println(
        "simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)"
        "), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.v(ctx.insn.operands[1]), *reinterpret_cast<const uint32_t*>(&value));
  } else {
    ctx.println("rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.v(ctx.insn.operands[1]));
  }
  return true;
}

bool build_vctuxs(BuilderContext& ctx) {
  // Vector Convert To Unsigned Fixed-Point Word Saturate
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u32, rex::ppc::simde_mm_vctuxs(",
            ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0)
    ctx.println("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))));",
                ctx.v(ctx.insn.operands[1]), 1u << ctx.insn.operands[2]);
  else
    ctx.println("simde_mm_load_ps({}.f32)));", ctx.v(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Vector Merge
//=============================================================================

bool build_vmrghb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi8", "u8");
  return true;
}

bool build_vmrghh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi16", "u16");
  return true;
}

bool build_vmrghw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi32", "u32");
  return true;
}

bool build_vmrglb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi8", "u8");
  return true;
}

bool build_vmrglh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi16", "u16");
  return true;
}

bool build_vmrglw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi32", "u32");
  return true;
}

//=============================================================================
// Vector Permute
//=============================================================================

bool build_vperm(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      ctx.v(ctx.insn.operands[3]));
  return true;
}

bool build_vpermwi128(BuilderContext& ctx) {
  // NOTE: accounting for full vector reversal here
  uint32_t x = 3 - (ctx.insn.operands[2] & 0x3);
  uint32_t y = 3 - ((ctx.insn.operands[2] >> 2) & 0x3);
  uint32_t z = 3 - ((ctx.insn.operands[2] >> 4) & 0x3);
  uint32_t w = 3 - ((ctx.insn.operands[2] >> 6) & 0x3);
  uint32_t perm = x | (y << 2) | (z << 4) | (w << 6);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vrlimi128(BuilderContext& ctx) {
  constexpr size_t shuffles[] = {SIMDE_MM_SHUFFLE(3, 2, 1, 0), SIMDE_MM_SHUFFLE(2, 1, 0, 3),
                                 SIMDE_MM_SHUFFLE(1, 0, 3, 2), SIMDE_MM_SHUFFLE(0, 3, 2, 1)};
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_blend_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_permute_ps(simde_mm_load_ps({}.f32), {}), {}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]),
      shuffles[ctx.insn.operands[3]], ctx.insn.operands[2]);
  return true;
}

//=============================================================================
// Vector Shift
//=============================================================================

bool build_vslb(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("sllv", "epi8", 0x7);
  return true;
}

bool build_vsldoi(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_alignr_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), {}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      16 - ctx.insn.operands[3]);
  return true;
}

bool build_vslh(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("sllv", "epi16", 0xF);
  return true;
}

bool build_vsrh(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("srlv", "epi16", 0xF);
  return true;
}

bool build_vsrb(BuilderContext& ctx) {
  // TODO(tomc): vectorize
  for (size_t i = 0; i < 16; i++)
    ctx.println("\t{}.u8[{}] = {}.u8[{}] >> ({}.u8[{}] & 0x7);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i);
  return true;
}

bool build_vsrab(BuilderContext& ctx) {
  // TODO(tomc): vectorize
  for (size_t i = 0; i < 16; i++)
    ctx.println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i);
  return true;
}

bool build_vsrah(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("srav", "epi16", 0xF);
  return true;
}

bool build_vrlh(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i sh = simde_mm_and_si128(");
  ctx.println("\t\t\tsimde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi16(0xF));", vB);
  ctx.println("\t\tsimde__m128i rsh = simde_mm_sub_epi16(simde_mm_set1_epi16(16), sh);");
  ctx.println("\t\tsimde__m128i result = simde_mm_or_si128(");
  ctx.println("\t\t\trex::ppc::simde_mm_sllv_epi16(a, sh),");
  ctx.println("\t\t\trex::ppc::simde_mm_srlv_epi16(a, rsh));");
  ctx.println("\t\tsimde_mm_store_si128((simde__m128i*){}.u8, result);", vD);
  ctx.println("\t}}");
  return true;
}

bool build_vrlw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simde_mm_rolv_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsl(BuilderContext& ctx) {
  // Vector Shift Left (128-bit) - shift entire vector left by bits specified in low 3 bits of vB
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsl(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vslo(BuilderContext& ctx) {
  // Vector Shift Left by Octet - shift entire vector left by bytes specified in bits 121:124 of vB
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vslo(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsro(BuilderContext& ctx) {
  // Vector Shift Right by Octet - shift entire vector right by bytes specified in bits 121:124 of
  // vB
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsro(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vslw(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  ctx.println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));");
  ctx.println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_sllv_epi32(a, shift));",
      vD);
  ctx.println("\t}}");
  return true;
}

bool build_vsr(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsr(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsraw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_srav_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0x1F))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsrw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_srlv_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0x1F))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Vector Splat
//=============================================================================

bool build_vspltb(BuilderContext& ctx) {
  // NOTE: accounting for full vector reversal here
  uint32_t perm = 15 - ctx.insn.operands[2];
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_set1_epi8(char(0x{:X}))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vsplth(BuilderContext& ctx) {
  // NOTE: accounting for full vector reversal here
  uint32_t perm = 7 - ctx.insn.operands[2];
  perm = (perm * 2) | ((perm * 2 + 1) << 8);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(short(0x{:X}))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vspltisb(BuilderContext& ctx) {
  // Sign-extend 5-bit immediate to 8-bit
  int8_t imm5 = static_cast<int8_t>(ctx.insn.operands[1] << 3) >> 3;
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_set1_epi8(char(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<uint8_t>(imm5));
  return true;
}

bool build_vspltisw(BuilderContext& ctx) {
  // Sign-extend 5-bit immediate to 32-bit
  int8_t imm5 = static_cast<int8_t>(ctx.insn.operands[1] << 3) >> 3;
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_set1_epi32(int(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<uint32_t>(static_cast<int32_t>(imm5)));
  return true;
}

bool build_vspltish(BuilderContext& ctx) {
  // Sign-extend 5-bit immediate to 16-bit
  int8_t imm5 = static_cast<int8_t>(ctx.insn.operands[1] << 3) >> 3;
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_set1_epi16(short(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<uint16_t>(static_cast<int16_t>(imm5)));
  return true;
}

bool build_vspltw(BuilderContext& ctx) {
  // NOTE: accounting for full vector reversal here
  uint32_t perm = 3 - ctx.insn.operands[2];
  perm |= (perm << 2) | (perm << 4) | (perm << 6);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

//=============================================================================
// Vector Pack
//=============================================================================

bool build_vpkshus(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuhum(BuilderContext& ctx) {
  // Vector Pack Unsigned Halfword Unsigned Modulo - pack low 8 bits from each halfword
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuhus(BuilderContext& ctx) {
  // Vector Pack Unsigned Halfword Unsigned Saturate: vA in the high half.
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_packus_epu16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuwum(BuilderContext& ctx) {
  // Vector Pack Unsigned Word Unsigned Modulo - pack low 16 bits from each word
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuwus(BuilderContext& ctx) {
  // Vector Pack Unsigned Word Unsigned Saturate: vA in the high half.
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "rex::ppc::simde_mm_packus_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkshss(BuilderContext& ctx) {
  // Vector Pack Signed Halfword Signed Saturate
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s8, "
      "simde_mm_packs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkswss(BuilderContext& ctx) {
  // Vector Pack Signed Word Signed Saturate
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkswus(BuilderContext& ctx) {
  // Vector Pack Signed Word Unsigned Saturate
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkd3d128(BuilderContext& ctx) {
  // TODO(tomc): vectorize
  // NOTE: handling vector reversal here too
  ctx.emit_set_flush_mode(true);
  switch (ctx.insn.operands[2]) {
    case 0:  // D3D color
    {
      uint32_t mask = ctx.insn.operands[3];
      uint32_t shift = ctx.insn.operands[4];

      // Pack the 4 floats to D3DCOLOR: 0 to 255 each (a NaN as 0), SAT for a
      // lane outside.
      for (size_t i = 0; i < 4; i++) {
        constexpr size_t indices[] = {3, 0, 1, 2};
        ctx.println(
            "\t{}.u32[{}] = rex::ppc::pack_d3d_field({}.u32[{}], 0x40400000u, 0x404000FFu, "
            "0x40400000u, ctx.vscr_sat);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i);
        ctx.println("\t{}.u32 {}= ({}.u32[{}] & 0xFF) << {};", ctx.temp(), i == 0 ? "" : "|",
                    ctx.v_temp(), i, indices[i] * 8);
      }

      // Handle mask operand:
      // mask=1: Write result to word[shift]
      // mask=2: Write result to word[shift], clear word[shift+1] to 0
      // mask=3: Same as mask=2, but for shift=3 only clear word[0] (don't write result)
      if (mask == 3 && shift == 3) {
        // Special case: mask=3, shift=3 - only clear word 0
        ctx.println("\t{}.u32[0] = 0;", ctx.v(ctx.insn.operands[0]));
      } else {
        // Write result to word[shift]
        ctx.println("\t{}.u32[{}] = {}.u32;", ctx.v(ctx.insn.operands[0]), shift, ctx.temp());
        // For mask=2 or mask=3, also clear the adjacent word
        if (mask >= 2 && shift < 3) {
          ctx.println("\t{}.u32[{}] = 0;", ctx.v(ctx.insn.operands[0]), shift + 1);
        }
      }
      break;
    }

    case 1:  // NORMSHORT2 - pack 2 floats (in 3.0+X form) to 2 signed shorts
    {
      // Extract signed 16-bit values from floats and pack into one 32-bit word
      // floats are in form: 3.0f + X (stored as integer add to 3.0f representation 0x40400000)
      // We need to saturate to signed 16-bit range [-32767, 32767] (NOT -32768, based on tests);
      // a NaN packs as 0x8000 and z and w have no field (no SAT from them).
      // NOTE: Guest element 0 is at array index 3, element 1 is at index 2 (reversed for host)
      // Element 0 (index 3) goes to HIGH word, element 1 (index 2) goes to LOW word
      for (size_t i = 0; i < 2; i++) {
        ctx.println(
            "\t{}.u32[{}] = rex::ppc::pack_d3d_field({}.u32[{}], 0x403F8001u, 0x40407FFFu, "
            "0x403F8000u, ctx.vscr_sat);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), 3 - i);
      }
      ctx.println("\t{0}.u32[0] = (({0}.u32[0] & 0xFFFF) << 16) | ({0}.u32[1] & 0xFFFF);",
                  ctx.v_temp());
      ctx.println("\t{}.u32[{}] = {}.u32[0];", ctx.v(ctx.insn.operands[0]), ctx.insn.operands[4],
                  ctx.v_temp());
      break;
    }

    case 2:  // NORMPACKED32 - pack 4 floats (in 3.0+X form) to 2:10:10:10 format (w:x:y:z)
    {
      // x, y, z: -511 to 511 (a NaN as 0x200), w: 0 to 3 (a NaN as 0); SAT for any lane
      // outside. Guest element 0 (x) is at host index 3, element 3 (w) at index 0.
      constexpr uint32_t kShift[] = {30, 20, 10, 0};
      for (size_t i = 0; i < 4; i++) {
        const bool w = i == 0;
        ctx.println(
            "\t{}.u32[{}] = rex::ppc::pack_d3d_field({}.u32[{}], 0x{:08X}u, 0x{:08X}u, "
            "0x{:08X}u, ctx.vscr_sat);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i, w ? 0x40400000u : 0x403FFE01u,
            w ? 0x40400003u : 0x404001FFu, w ? 0x40400000u : 0x403FFE00u);
      }
      ctx.println("\t{}.u32 = 0;", ctx.temp());
      for (size_t i = 0; i < 4; i++) {
        ctx.println("\t{}.u32 |= ({}.u32[{}] & 0x{:X}) << {};", ctx.temp(), ctx.v_temp(), i,
                    i == 0 ? 0x3u : 0x3FFu, kShift[i]);
      }
      ctx.println("\t{}.u32[{}] = {}.u32;", ctx.v(ctx.insn.operands[0]), ctx.insn.operands[4],
                  ctx.temp());
      break;
    }

    case 3:  // FLOAT16_2 - pack 2 floats to 2 float16s
    {
      // Pack 2 elements into 32 bits (1 word)
      // Guest element 0 goes to high 16 bits, element 1 to low 16 bits
      // Output u16 index = (1-i) + 2*shift for element i
      for (size_t i = 0; i < 2; i++) {
        size_t srcIdx = 3 - i;  // Guest element i is at host array index 3-i
        size_t dstIdx =
            (1 - i) + (2 * ctx.insn.operands[4]);  // Output reversed: elem 0 to high, elem 1 to low
        ctx.println("\t{}.u32 = ({}.u32[{}]&0x7FFFFFFF);", ctx.temp(), ctx.v(ctx.insn.operands[1]),
                    srcIdx);
        ctx.println(
            "\t{0}.u8[0] = ({1}.f32 != {1}.f32) || ({1}.f32 > 65504.0f) ? 0xFF : "
            "(({2}.u32[{3}]&0x7f800000)>>23);",
            ctx.v_temp(), ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println("\t{}.u16 = {}.u8[0] != 0xFF ? (({}.u32[{}]&0x7FE000)>>13) : 0x0;", ctx.temp(),
                    ctx.v_temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println(
            "\t{0}.u16[{1}] = {2}.u8[0] != 0xFF ? ({2}.u8[0] > 0x70 ? "
            "((({2}.u8[0]-0x70)<<10)+{3}.u16) : (0x71-{2}.u8[0] > 31 ? 0x0 : "
            "((0x400+{3}.u16)>>(0x71-{2}.u8[0])))) : 0x7FFF;",
            ctx.v(ctx.insn.operands[0]), dstIdx, ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u16[{}] |= (({}.u32[{}]&0x80000000)>>16);", ctx.v(ctx.insn.operands[0]),
                    dstIdx, ctx.v(ctx.insn.operands[1]), srcIdx);
      }
      break;
    }

    case 4:  // NORMSHORT4 - pack 4 floats (in 3.0+X form) to 4 signed shorts
    {
      // Pack 4 elements into 64 bits (2 words)
      // Guest element 0 goes to highest 16-bit position, element 3 to lowest
      // Output u16 index = (3-i) + 2*shift for element i
      // -32767 to 32767 each (a NaN as 0x8000), SAT for any lane outside. All four
      // fields are taken before the destination is written (it may be the source).
      for (size_t i = 0; i < 4; i++) {
        ctx.println(
            "\t{}.u32[{}] = rex::ppc::pack_d3d_field({}.u32[{}], 0x403F8001u, 0x40407FFFu, "
            "0x403F8000u, ctx.vscr_sat);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i);
      }
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;  // Guest element i is at host array index 3-i
        size_t dstIdx = (3 - i) + (2 * ctx.insn.operands[4]);  // Output also reversed
        ctx.println("\t{}.u16[{}] = uint16_t({}.u32[{}]);", ctx.v(ctx.insn.operands[0]), dstIdx,
                    ctx.v_temp(), srcIdx);
      }
      break;
    }

    case 5:  // float16_4
    {
      // Pack 4 elements into 64 bits (4 x 16-bit floats)
      // Guest element 0 goes to highest 16-bit position, element 3 to lowest
      // Output u16 index = (3-i) + 2*shift for element i
      if (ctx.insn.operands[3] != 2 || ctx.insn.operands[4] > 2)
        REXCODEGEN_WARN("Unexpected float16_4 pack instruction at {:X}", ctx.base);

      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;  // Guest element i is at host array index 3-i
        size_t dstIdx = (3 - i) + (2 * ctx.insn.operands[4]);  // Output also reversed
        ctx.println("\t{}.u32 = ({}.u32[{}]&0x7FFFFFFF);", ctx.temp(), ctx.v(ctx.insn.operands[1]),
                    srcIdx);
        ctx.println(
            "\t{0}.u8[0] = ({1}.f32 != {1}.f32) || ({1}.f32 > 65504.0f) ? 0xFF : "
            "(({2}.u32[{3}]&0x7f800000)>>23);",
            ctx.v_temp(), ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println("\t{}.u16 = {}.u8[0] != 0xFF ? (({}.u32[{}]&0x7FE000)>>13) : 0x0;", ctx.temp(),
                    ctx.v_temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println(
            "\t{0}.u16[{1}] = {2}.u8[0] != 0xFF ? ({2}.u8[0] > 0x70 ? "
            "((({2}.u8[0]-0x70)<<10)+{3}.u16) : (0x71-{2}.u8[0] > 31 ? 0x0 : "
            "((0x400+{3}.u16)>>(0x71-{2}.u8[0])))) : 0x7FFF;",
            ctx.v(ctx.insn.operands[0]), dstIdx, ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u16[{}] |= (({}.u32[{}]&0x80000000)>>16);", ctx.v(ctx.insn.operands[0]),
                    dstIdx, ctx.v(ctx.insn.operands[1]), srcIdx);
      }
      break;
    }

    case 6:  // NORMPACKED64 - pack 4 floats to 4:20:20:20 format
    {
      // Format: 4 bits for w, 20 bits each for x, y, z (packed into 64 bits), the
      // lanes in the 3.0 + X form like the other packs (the old code converted
      // the float to an integer and took guest element 3 for x). x, y, z:
      // -524287 to 524287 (a NaN as 0x80000), w: 0 to 15 (a NaN as 0); SAT for
      // any lane outside. Guest element 0 (x) is at host index 3.
      constexpr uint32_t kShift[] = {60, 40, 20, 0};
      for (size_t i = 0; i < 4; i++) {
        const bool w = i == 0;
        ctx.println(
            "\t{}.u32[{}] = rex::ppc::pack_d3d_field({}.u32[{}], 0x{:08X}u, 0x{:08X}u, "
            "0x{:08X}u, ctx.vscr_sat);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i, w ? 0x40400000u : 0x40380001u,
            w ? 0x4040000Fu : 0x4047FFFFu, w ? 0x40400000u : 0x40380000u);
      }
      ctx.println("\t{}.u64 = 0;", ctx.temp());
      for (size_t i = 0; i < 4; i++) {
        ctx.println("\t{}.u64 |= uint64_t({}.u32[{}] & 0x{:X}) << {};", ctx.temp(), ctx.v_temp(), i,
                    i == 0 ? 0xFu : 0xFFFFFu, kShift[i]);
      }
      ctx.println("\t{}.u64[{}] = {}.u64;", ctx.v(ctx.insn.operands[0]), ctx.insn.operands[4] >> 1,
                  ctx.temp());
      break;
    }

    default:
      ctx.println("\t__builtin_debugtrap();");
      break;
  }
  return true;
}

//=============================================================================
// Vector Unpack
//=============================================================================

bool build_vupkd3d128(BuilderContext& ctx) {
  // TODO(tomc): Vectorize
  // NOTE: handling vector reversal here too
  switch (ctx.insn.operands[2] >> 2) {
    case 0:  // D3D color
      for (size_t i = 0; i < 4; i++) {
        constexpr size_t indices[] = {3, 0, 1, 2};
        ctx.println("\t{}.u32[{}] = {}.u8[{}] | 0x3F800000;", ctx.v_temp(), i,
                    ctx.v(ctx.insn.operands[1]), indices[i]);
      }
      ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
      break;

    case 1:  // NORMSHORT2 - unpack 2 shorts to floats (3.0+X form)
      for (size_t i = 0; i < 2; i++) {
        ctx.println("\t{}.f32 = 3.0f;", ctx.temp());
        ctx.println("\t{}.s32 += {}.s16[{}];", ctx.temp(), ctx.v(ctx.insn.operands[1]), 1 - i);
        // -32768 (3.0 - 2^-7, 0x403F8000) is the negative overflow, a quiet NaN on
        // the console (xenia-canary's XMMUnpackSHORT_Overflow; test short2_3).
        ctx.println("\tif ({}.u32 == 0x403F8000) {}.u32 = 0x7FC00000;", ctx.temp(), ctx.temp());
        ctx.println("\t{}.f32[{}] = {}.f32;", ctx.v_temp(), 3 - i, ctx.temp());
      }
      ctx.println("\t{}.f32[1] = 0.0f;", ctx.v_temp());
      ctx.println("\t{}.f32[0] = 1.0f;", ctx.v_temp());
      ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
      break;

    case 2:  // NORMPACKED32 - unpack 2:10:10:10 to floats
    {
      // Format: w(2 bits):z(10 bits):y(10 bits):x(10 bits) in Guest element 3 (host s32[0])
      // x, y, z --> 3.0+X form (signed 10-bit), w --> 1.0+w form
      // Output: x --> Guest element 0 (host u32[3]), y --> Guest element 1 (host u32[2]),
      //         z --> Guest element 2 (host u32[1]), w --> Guest element 3 (host u32[0])
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      // x (bits 0-9) - sign extend from 10 bits --> Guest element 0 (host u32[3])
      ctx.println("\t{}.s32 = ({}.s32[0] << 22) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 == -512 ? 0x7FC00000 : ({}.s32 + 0x40400000);",
                  ctx.v_temp(), ctx.temp(), ctx.temp());
      ctx.println("\t{}.u32[3] = {}.u32[0];", vDst, ctx.v_temp());
      // y (bits 10-19) - sign extend from 10 bits --> Guest element 1 (host u32[2])
      ctx.println("\t{}.s32 = ({}.s32[0] << 12) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 + 0x40400000;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u32[2] = {}.u32[0];", vDst, ctx.v_temp());
      // z (bits 20-29) - sign extend from 10 bits --> Guest element 2 (host u32[1])
      ctx.println("\t{}.s32 = ({}.s32[0] << 2) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 + 0x40400000;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u32[1] = {}.u32[0];", vDst, ctx.v_temp());
      // w (bits 30-31) - 2 bits, convert to 1.0+w form --> Guest element 3 (host u32[0])
      ctx.println("\t{}.u32[0] = ({}.u32[0] >> 30) | 0x3F800000;", ctx.v_temp(), vSrc);
      ctx.println("\t{}.u32[0] = {}.u32[0];", vDst, ctx.v_temp());
      break;
    }

    case 3:  // FLOAT16_2 - unpack 2 float16 to floats
    {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      // Unpack 2 float16s from u16[0,1] to elements 0,1 (stored at u32[3,2])
      // Element 0 (at u32[3]) from u16[1] (high), element 1 (at u32[2]) from u16[0] (low)
      for (size_t i = 0; i < 2; i++) {
        size_t srcIdx = 1 - i;  // Read from u16[1], u16[0]
        size_t dstIdx = 3 - i;  // Write to u32[3], u32[2]
        ctx.println("\t{}.u32 = {}.u16[{}];", ctx.temp(), vSrc, srcIdx);
        // Extract sign, exponent, mantissa
        ctx.println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.v_temp(), ctx.temp(), ctx.temp(), ctx.temp());
        // Handle zero/denorm case
        ctx.println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.temp(), ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u32[{}] = {}.u32[0];", vDst, dstIdx, ctx.v_temp());
      }
      ctx.println("\t{}.f32[1] = 0.0f;", vDst);
      ctx.println("\t{}.f32[0] = 1.0f;", vDst);
      break;
    }

    case 4:  // NORMSHORT4 - unpack 4 shorts to floats (3.0+X form)
    {
      // Unpack 4 shorts from Guest elements 2-3 (host u16[0-3]) to 4 floats
      // Guest element order is reversed in host arrays
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;  // Read from u16 indices 3, 2, 1, 0 (guest shorts 0, 1, 2, 3)
        size_t dstIdx = 3 - i;  // Write to f32 indices 3, 2, 1, 0 (Guest elements 0, 1, 2, 3)
        ctx.println("\t{}.f32 = 3.0f;", ctx.temp());
        ctx.println("\t{}.s32 += {}.s16[{}];", ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        // -32768: a quiet NaN, as for NORMSHORT2.
        ctx.println("\tif ({}.u32 == 0x403F8000) {}.u32 = 0x7FC00000;", ctx.temp(), ctx.temp());
        ctx.println("\t{}.f32[{}] = {}.f32;", ctx.v(ctx.insn.operands[0]), dstIdx, ctx.temp());
      }
      break;
    }

    case 5:  // FLOAT16_4 - unpack 4 float16 to floats
    {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      // Unpack 4 float16s from Guest elements 2-3 (host u16[0-3]) to elements 0-3
      // Guest element order is reversed in host arrays
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;  // Read from u16 indices 3, 2, 1, 0 (guest shorts 0, 1, 2, 3)
        size_t dstIdx = 3 - i;  // Write to u32 indices 3, 2, 1, 0 (Guest elements 0, 1, 2, 3)
        ctx.println("\t{}.u32 = {}.u16[{}];", ctx.temp(), vSrc, srcIdx);
        // Extract sign, exponent, mantissa and convert to float32
        ctx.println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.v_temp(), ctx.temp(), ctx.temp(), ctx.temp());
        // Handle zero/denorm case
        ctx.println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.temp(), ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u32[{}] = {}.u32[0];", vDst, dstIdx, ctx.v_temp());
      }
      break;
    }

    case 6:  // NORMPACKED64 - unpack 4:20:20:20 to floats
    {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      // Format: w(4 bits):z(20 bits):y(20 bits):x(20 bits) in Guest elements 2-3 (host u64[0])
      // x, y, z --> 3.0+X form (signed 20-bit), w --> 1.0+w form; -0x80000 gives QNaN
      // Output: x --> Guest element 0 (host u32[3]), y --> Guest element 1 (host u32[2]),
      //         z --> Guest element 2 (host u32[1]), w --> Guest element 3 (host u32[0])
      ctx.println("\t{}.u64[0] = {}.u64[0];", ctx.v_temp(), vSrc);
      for (size_t i = 0; i < 3; i++) {
        // Field i at bits 20*i .. 20*i+19, sign extended from 20 bits
        ctx.println("\t{}.s32 = int32_t(int64_t({}.u64[0] << {}) >> 44);", ctx.temp(),
                    ctx.v_temp(), 44 - 20 * i);
        ctx.println(
            "\t{}.u32[{}] = {}.s32 == -0x80000 ? 0x7FC00000u : uint32_t({}.s32 + 0x40400000);",
            vDst, 3 - i, ctx.temp(), ctx.temp());
      }
      // w (bits 60-63) - 4 bits, 1.0+w form
      ctx.println("\t{}.u32[0] = uint32_t({}.u64[0] >> 60) | 0x3F800000;", vDst, ctx.v_temp());
      break;
    }

    default:
      ctx.println("\t__builtin_debugtrap();");
      break;
  }
  return true;
}

bool build_vupkhsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_cvtepi8_epi16(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s8), "
      "simde_mm_load_si128((simde__m128i*){}.s8))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupkhsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupklsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi8_epi16(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupklsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]));
  return true;
}

}  // namespace rex::codegen
