/**
 * @file        graphics/fetch_parse_test.cpp
 * @brief       Tests for Xenos texture fetch instruction operand parsing
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>

using namespace rex::graphics;

namespace {

// Parses a texture fetch from r0 with the given source swizzle (three 2-bit components).
ParsedTextureFetchInstruction Parse(ucode::FetchOpcode opcode, xenos::FetchOpDimension dimension,
                                    uint32_t src_swizzle) {
  uint32_t dwords[3] = {};
  // Opcode in bits 0:4, source swizzle in bits 26:31.
  dwords[0] = uint32_t(opcode) | (src_swizzle << 26);
  // Dimension in bits 14:15.
  dwords[2] = uint32_t(dimension) << 14;
  ucode::TextureFetchInstruction op;
  std::memcpy(&op, dwords, sizeof(op));
  ParsedTextureFetchInstruction parsed;
  ParseTextureFetchInstruction(op, parsed);
  return parsed;
}

}  // namespace

// xenia-canary fbdb1f281: tfetch1D with a nonreplicated source swizzle provides XY for a
// 2D fetch constant (545407D4's UI shader).
TEST_CASE("tfetch1D with a nonreplicated swizzle reads two components", "[graphics][fetch]") {
  // X from x, Y from y.
  ParsedTextureFetchInstruction parsed =
      Parse(ucode::FetchOpcode::kTextureFetch, xenos::FetchOpDimension::k1D, 0b00'01'00);
  REQUIRE(parsed.operand_count >= 1);
  REQUIRE(parsed.operands[0].component_count == 2);
  CHECK(parsed.operands[0].components[0] == SwizzleSource::kX);
  CHECK(parsed.operands[0].components[1] == SwizzleSource::kY);
}

TEST_CASE("tfetch1D with a replicated swizzle reads one component", "[graphics][fetch]") {
  // zzz.
  ParsedTextureFetchInstruction parsed =
      Parse(ucode::FetchOpcode::kTextureFetch, xenos::FetchOpDimension::k1D, 0b10'10'10);
  REQUIRE(parsed.operand_count >= 1);
  REQUIRE(parsed.operands[0].component_count == 1);
  CHECK(parsed.operands[0].components[0] == SwizzleSource::kZ);
}

TEST_CASE("Other 1D fetch opcodes keep one component", "[graphics][fetch]") {
  ParsedTextureFetchInstruction parsed = Parse(ucode::FetchOpcode::kGetTextureComputedLod,
                                               xenos::FetchOpDimension::k1D, 0b00'01'00);
  REQUIRE(parsed.operand_count >= 1);
  CHECK(parsed.operands[0].component_count == 1);
}

// xenia-edge 669b4266f: getBCF goes with a tfetch and reads the same coordinates.
TEST_CASE("getBCF1D with a nonreplicated swizzle reads two components like tfetch1D",
          "[graphics][fetch]") {
  ParsedTextureFetchInstruction parsed = Parse(ucode::FetchOpcode::kGetTextureBorderColorFrac,
                                               xenos::FetchOpDimension::k1D, 0b00'01'00);
  REQUIRE(parsed.operand_count >= 1);
  CHECK(parsed.operands[0].component_count == 2);
}

namespace {

// Parses a tfetch2D with the given extra bits in dwords 0 and 1.
ParsedTextureFetchInstruction ParseTfetch2D(uint32_t dword_0_bits, uint32_t dword_1_bits) {
  uint32_t dwords[3] = {};
  dwords[0] = uint32_t(ucode::FetchOpcode::kTextureFetch) | (0b00'01'00u << 26) | dword_0_bits;
  dwords[1] = dword_1_bits;
  dwords[2] = uint32_t(xenos::FetchOpDimension::k2D) << 14;
  ucode::TextureFetchInstruction op;
  std::memcpy(&op, dwords, sizeof(op));
  ParsedTextureFetchInstruction parsed;
  ParseTextureFetchInstruction(op, parsed);
  return parsed;
}

}  // namespace

// xenia-canary c3cd8617b: only point sampled 2D fetches with normalized coordinates snap to texel
// centers.
TEST_CASE("Point sampled normalized tfetch2D snaps to texel centers", "[graphics][fetch]") {
  CHECK(ParseTfetch2D(0, 0).CanSnapToTexelCenter(true));
  // Unnormalized coordinates (dword 0 bit 25).
  CHECK_FALSE(ParseTfetch2D(UINT32_C(1) << 25, 0).CanSnapToTexelCenter(true));
  // Linear mag filter override (dword 1 bits 12:13).
  CHECK_FALSE(
      ParseTfetch2D(0, uint32_t(xenos::TextureFilter::kLinear) << 12).CanSnapToTexelCenter(true));
  // 1D fetches don't snap.
  CHECK_FALSE(Parse(ucode::FetchOpcode::kTextureFetch, xenos::FetchOpDimension::k1D, 0b10'10'10)
                  .CanSnapToTexelCenter(true));
}

// xenia-edge 669b4266f: getBCF2D snaps like the tfetch2D it goes with.
TEST_CASE("Point sampled normalized getBCF2D snaps to texel centers", "[graphics][fetch]") {
  CHECK(Parse(ucode::FetchOpcode::kGetTextureBorderColorFrac, xenos::FetchOpDimension::k2D,
              0b00'01'00)
            .CanSnapToTexelCenter(true));
  CHECK_FALSE(Parse(ucode::FetchOpcode::kGetTextureComputedLod, xenos::FetchOpDimension::k2D,
                    0b00'01'00)
                  .CanSnapToTexelCenter(true));
}
