/**
 * @file        rexcodegen/builders/vector_integer.cpp
 * @brief       AltiVec integer instructions missing from vector.cpp: byte rotate,
 *              word carry/average/max, even/odd multiplies, multiply-sum,
 *              sum-across, pixel pack/unpack, VSCR moves
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <initializer_list>
#include <string>
#include <string_view>

#include <ppc.h>

namespace rex::codegen {

//=============================================================================
// Element order
//=============================================================================
// Vector registers are held byte-reversed: guest element k of an N-element
// vector is host element N-1-k. Element-wise operations do not care. For the
// rest:
//  - guest even elements (0, 2, ...) are the host odd ones, so the "even"
//    half of a host halfword/word is its high part;
//  - the bytes/halfwords that make up guest word k are the ones of host word
//    3-k, so per-word sums can work on host words directly;
//  - "high" (guest first) elements are the host upper ones.
// Results that mix elements go through a temporary, as vD may alias vA/vB/vC.
//
// Saturating instructions set ctx.vscr_sat (VSCR[SAT]), which is sticky.

namespace {

// Prints `lines`, each with a leading tab, after replacing $0..$3 with the
// instruction's vector operands 0..3. Operands are resolved on first use, so
// only the registers an instruction has are marked as used.
void emitVec(BuilderContext& ctx, std::initializer_list<std::string_view> lines) {
  std::string names[4];
  for (std::string_view line : lines) {
    std::string out;
    out.reserve(line.size() + 16);
    for (size_t i = 0; i < line.size(); i++) {
      if (line[i] == '$' && i + 1 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '3') {
        const size_t index = line[i + 1] - '0';
        if (names[index].empty()) {
          names[index] = ctx.v(ctx.insn.operands[index]);
        }
        out += names[index];
        i++;
      } else {
        out += line[i];
      }
    }
    ctx.println("\t{}", out);
  }
}

}  // namespace

//=============================================================================
// Rotate, carry, average, max
//=============================================================================

bool build_vrlb(BuilderContext& ctx) {
  // vD.b[i] = rotl8(vA.b[i], vB.b[i] & 7)
  emitVec(ctx, {
                   "for (int i = 0; i < 16; i++) {",
                   "\tconst uint32_t sh = $2.u8[i] & 7;",
                   "\t$0.u8[i] = uint8_t(($1.u8[i] << sh) | ($1.u8[i] >> ((8 - sh) & 7)));",
                   "}",
               });
  return true;
}

bool build_vaddcuw(BuilderContext& ctx) {
  // vD.w[i] = carry out of vA.w[i] + vB.w[i]
  emitVec(ctx, {
                   "for (int i = 0; i < 4; i++)",
                   "\t$0.u32[i] = uint32_t((uint64_t($1.u32[i]) + $2.u32[i]) >> 32);",
               });
  return true;
}

bool build_vsubcuw(BuilderContext& ctx) {
  // vD.w[i] = carry out of vA.w[i] + ~vB.w[i] + 1, i.e. no borrow
  emitVec(ctx, {
                   "for (int i = 0; i < 4; i++)",
                   "\t$0.u32[i] = $1.u32[i] >= $2.u32[i] ? 1u : 0u;",
               });
  return true;
}

bool build_vavguw(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int i = 0; i < 4; i++)",
                   "\t$0.u32[i] = uint32_t((uint64_t($1.u32[i]) + $2.u32[i] + 1) >> 1);",
               });
  return true;
}

bool build_vmaxuw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu32", "u32");
  return true;
}

//=============================================================================
// Even/odd multiplies
//=============================================================================
// Guest even elements are the host odd ones (index 2i+1), guest odd the host
// even ones (index 2i).

namespace {

void emitMulEvenOdd(BuilderContext& ctx, bool even, const char* dst_field, const char* src_field,
                    const char* product_type, int count) {
  const char* index = even ? "2 * i + 1" : "2 * i";
  const std::string loop = fmt::format("\tfor (int i = 0; i < {}; i++)", count);
  const std::string body =
      fmt::format("\t\tt.{}[i] = {}({}($1.{}[{}]) * {}($2.{}[{}]));", dst_field, product_type,
                  product_type, src_field, index, product_type, src_field, index);
  emitVec(ctx, {"{", "\tPPCVRegister t;", loop, body, "\t$0 = t;", "}"});
}

}  // namespace

bool build_vmuleub(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, true, "u16", "u8", "uint32_t", 8);
  return true;
}

bool build_vmuloub(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, false, "u16", "u8", "uint32_t", 8);
  return true;
}

bool build_vmulesb(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, true, "s16", "s8", "int32_t", 8);
  return true;
}

bool build_vmulosb(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, false, "s16", "s8", "int32_t", 8);
  return true;
}

bool build_vmuleuh(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, true, "u32", "u16", "uint32_t", 4);
  return true;
}

bool build_vmulouh(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, false, "u32", "u16", "uint32_t", 4);
  return true;
}

bool build_vmulesh(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, true, "s32", "s16", "int32_t", 4);
  return true;
}

bool build_vmulosh(BuilderContext& ctx) {
  emitMulEvenOdd(ctx, false, "s32", "s16", "int32_t", 4);
  return true;
}

//=============================================================================
// Multiply-add (halfword), element-wise
//=============================================================================

bool build_vmladduhm(BuilderContext& ctx) {
  // vD.h[i] = (vA.h[i] * vB.h[i] + vC.h[i]) mod 2^16
  emitVec(ctx, {
                   "for (int i = 0; i < 8; i++)",
                   "\t$0.u16[i] = uint16_t(uint32_t($1.u16[i]) * $2.u16[i] + $3.u16[i]);",
               });
  return true;
}

bool build_vmhaddshs(BuilderContext& ctx) {
  // vD.h[i] = sat16(((vA.h[i] * vB.h[i]) >> 15) + vC.h[i])
  emitVec(ctx, {
                   "for (int i = 0; i < 8; i++) {",
                   "\tint32_t r = ((int32_t($1.s16[i]) * $2.s16[i]) >> 15) + $3.s16[i];",
                   "\tif (r > 32767) { r = 32767; ctx.vscr_sat = 1; }",
                   "\telse if (r < -32768) { r = -32768; ctx.vscr_sat = 1; }",
                   "\t$0.s16[i] = int16_t(r);",
                   "}",
               });
  return true;
}

bool build_vmhraddshs(BuilderContext& ctx) {
  // vD.h[i] = sat16(((vA.h[i] * vB.h[i] + 0x4000) >> 15) + vC.h[i])
  emitVec(ctx, {
                   "for (int i = 0; i < 8; i++) {",
                   "\tint32_t r = ((int32_t($1.s16[i]) * $2.s16[i] + 0x4000) >> 15) + $3.s16[i];",
                   "\tif (r > 32767) { r = 32767; ctx.vscr_sat = 1; }",
                   "\telse if (r < -32768) { r = -32768; ctx.vscr_sat = 1; }",
                   "\t$0.s16[i] = int16_t(r);",
                   "}",
               });
  return true;
}

//=============================================================================
// Multiply-sum: per word, sum the products of its bytes/halfwords plus vC
//=============================================================================

bool build_vmsumubm(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint32_t sum = $3.u32[w];",
                   "\tfor (int j = 0; j < 4; j++)",
                   "\t\tsum += uint32_t($1.u8[4 * w + j]) * $2.u8[4 * w + j];",
                   "\t$0.u32[w] = sum;",
                   "}",
               });
  return true;
}

bool build_vmsummbm(BuilderContext& ctx) {
  // Signed bytes of vA times unsigned bytes of vB, modulo 2^32.
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint32_t sum = $3.u32[w];",
                   "\tfor (int j = 0; j < 4; j++)",
                   "\t\tsum += uint32_t(int32_t($1.s8[4 * w + j]) * int32_t($2.u8[4 * w + j]));",
                   "\t$0.u32[w] = sum;",
                   "}",
               });
  return true;
}

bool build_vmsumuhm(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint32_t sum = $3.u32[w];",
                   "\tfor (int j = 0; j < 2; j++)",
                   "\t\tsum += uint32_t($1.u16[2 * w + j]) * $2.u16[2 * w + j];",
                   "\t$0.u32[w] = sum;",
                   "}",
               });
  return true;
}

bool build_vmsumuhs(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint64_t sum = $3.u32[w];",
                   "\tfor (int j = 0; j < 2; j++)",
                   "\t\tsum += uint64_t($1.u16[2 * w + j]) * $2.u16[2 * w + j];",
                   "\tif (sum > 0xFFFFFFFFull) { sum = 0xFFFFFFFFull; ctx.vscr_sat = 1; }",
                   "\t$0.u32[w] = uint32_t(sum);",
                   "}",
               });
  return true;
}

bool build_vmsumshm(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint32_t sum = $3.u32[w];",
                   "\tfor (int j = 0; j < 2; j++)",
                   "\t\tsum += uint32_t(int32_t($1.s16[2 * w + j]) * $2.s16[2 * w + j]);",
                   "\t$0.u32[w] = sum;",
                   "}",
               });
  return true;
}

bool build_vmsumshs(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tint64_t sum = $3.s32[w];",
                   "\tfor (int j = 0; j < 2; j++)",
                   "\t\tsum += int64_t(int32_t($1.s16[2 * w + j]) * $2.s16[2 * w + j]);",
                   "\tif (sum > INT32_MAX) { sum = INT32_MAX; ctx.vscr_sat = 1; }",
                   "\telse if (sum < INT32_MIN) { sum = INT32_MIN; ctx.vscr_sat = 1; }",
                   "\t$0.s32[w] = int32_t(sum);",
                   "}",
               });
  return true;
}

//=============================================================================
// Sum across
//=============================================================================

bool build_vsum4sbs(BuilderContext& ctx) {
  // vD.w[i] = sat32(vB.w[i] + sum of the four signed bytes of vA.w[i])
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tint64_t sum = $2.s32[w];",
                   "\tfor (int j = 0; j < 4; j++)",
                   "\t\tsum += $1.s8[4 * w + j];",
                   "\tif (sum > INT32_MAX) { sum = INT32_MAX; ctx.vscr_sat = 1; }",
                   "\telse if (sum < INT32_MIN) { sum = INT32_MIN; ctx.vscr_sat = 1; }",
                   "\t$0.s32[w] = int32_t(sum);",
                   "}",
               });
  return true;
}

bool build_vsum4ubs(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tuint64_t sum = $2.u32[w];",
                   "\tfor (int j = 0; j < 4; j++)",
                   "\t\tsum += $1.u8[4 * w + j];",
                   "\tif (sum > 0xFFFFFFFFull) { sum = 0xFFFFFFFFull; ctx.vscr_sat = 1; }",
                   "\t$0.u32[w] = uint32_t(sum);",
                   "}",
               });
  return true;
}

bool build_vsum4shs(BuilderContext& ctx) {
  emitVec(ctx, {
                   "for (int w = 0; w < 4; w++) {",
                   "\tint64_t sum = int64_t($2.s32[w]) + $1.s16[2 * w] + $1.s16[2 * w + 1];",
                   "\tif (sum > INT32_MAX) { sum = INT32_MAX; ctx.vscr_sat = 1; }",
                   "\telse if (sum < INT32_MIN) { sum = INT32_MIN; ctx.vscr_sat = 1; }",
                   "\t$0.s32[w] = int32_t(sum);",
                   "}",
               });
  return true;
}

bool build_vsum2sws(BuilderContext& ctx) {
  // Guest word 1 = sat(vA.w0 + vA.w1 + vB.w1), guest word 3 = sat(vA.w2 + vA.w3 + vB.w3),
  // guest words 0 and 2 = 0. Guest words 0..3 are host words 3..0.
  emitVec(ctx, {
                   "{",
                   "\tPPCVRegister t;",
                   "\tt.u32[3] = 0;",
                   "\tt.u32[1] = 0;",
                   "\tfor (int h = 0; h < 4; h += 2) {",
                   "\t\tint64_t sum = int64_t($1.s32[h + 1]) + $1.s32[h] + $2.s32[h];",
                   "\t\tif (sum > INT32_MAX) { sum = INT32_MAX; ctx.vscr_sat = 1; }",
                   "\t\telse if (sum < INT32_MIN) { sum = INT32_MIN; ctx.vscr_sat = 1; }",
                   "\t\tt.s32[h] = int32_t(sum);",
                   "\t}",
                   "\t$0 = t;",
                   "}",
               });
  return true;
}

bool build_vsumsws(BuilderContext& ctx) {
  // Guest word 3 (host word 0) = sat(sum of the words of vA + vB.w3), the rest 0.
  emitVec(ctx, {
                   "{",
                   "\tint64_t sum = int64_t($1.s32[0]) + $1.s32[1] + $1.s32[2] + $1.s32[3] + "
                   "$2.s32[0];",
                   "\tif (sum > INT32_MAX) { sum = INT32_MAX; ctx.vscr_sat = 1; }",
                   "\telse if (sum < INT32_MIN) { sum = INT32_MIN; ctx.vscr_sat = 1; }",
                   "\tPPCVRegister t;",
                   "\tt.u32[1] = t.u32[2] = t.u32[3] = 0;",
                   "\tt.s32[0] = int32_t(sum);",
                   "\t$0 = t;",
                   "}",
               });
  return true;
}

//=============================================================================
// Pixel pack/unpack (1:5:5:5)
//=============================================================================

bool build_vpkpx(BuilderContext& ctx) {
  // Each 8:8:8:8 word becomes 1:5:5:5: bit 7 of byte 0, the top 5 bits of
  // bytes 1, 2 and 3. Guest halfwords 0..3 come from vA, 4..7 from vB, so
  // host halfwords 4..7 from host words 0..3 of vA, 0..3 from those of vB.
  emitVec(ctx, {
                   "{",
                   "\tPPCVRegister t;",
                   "\tfor (int h = 0; h < 8; h++) {",
                   "\t\tconst uint32_t x = h < 4 ? $2.u32[h] : $1.u32[h - 4];",
                   "\t\tt.u16[h] = uint16_t(((x >> 9) & 0xFC00) | ((x >> 6) & 0x03E0) | "
                   "((x >> 3) & 0x001F));",
                   "\t}",
                   "\t$0 = t;",
                   "}",
               });
  return true;
}

namespace {

// 1:5:5:5 to 8:8:8:8: the 1-bit field sign-extended, the 5-bit ones
// zero-extended. `first` is the first host halfword of vB to unpack.
void emitUnpackPixel(BuilderContext& ctx, int first) {
  const std::string source = fmt::format("\t\tconst uint32_t p = $1.u16[h + {}];", first);
  emitVec(ctx, {
                   "{",
                   "\tPPCVRegister t;",
                   "\tfor (int h = 0; h < 4; h++) {",
                   source,
                   "\t\tt.u32[h] = ((p & 0x8000) ? 0xFF000000u : 0u) | (((p >> 10) & 0x1F) << 16) | "
                   "(((p >> 5) & 0x1F) << 8) | (p & 0x1F);",
                   "\t}",
                   "\t$0 = t;",
                   "}",
               });
}

}  // namespace

bool build_vupkhpx(BuilderContext& ctx) {
  // Guest halfwords 0..3 are host halfwords 4..7.
  emitUnpackPixel(ctx, 4);
  return true;
}

bool build_vupklpx(BuilderContext& ctx) {
  emitUnpackPixel(ctx, 0);
  return true;
}

//=============================================================================
// VSCR
//=============================================================================
// Only SAT is tracked (ctx.vscr_sat). NJ is reported as set, the Xenon reset
// value; vector floating point does not change with it here.

bool build_mfvscr(BuilderContext& ctx) {
  emitVec(ctx, {
                   "$0.u32[3] = $0.u32[2] = $0.u32[1] = 0;",
                   "$0.u32[0] = 0x00010000u | (ctx.vscr_sat & 1u);",
               });
  return true;
}

bool build_mtvscr(BuilderContext& ctx) {
  // mtvscr vB: the VSCR is guest word 3 (host word 0).
  emitVec(ctx, {"ctx.vscr_sat = uint8_t($0.u32[0] & 1u);"});
  return true;
}

}  // namespace rex::codegen
