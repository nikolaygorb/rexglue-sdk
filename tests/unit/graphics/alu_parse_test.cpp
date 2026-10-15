/**
 * @file        graphics/alu_parse_test.cpp
 * @brief       Tests for Xenos ALU instruction operand parsing
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

// Builds an ALU instruction with identity swizzles, the scalar operation reading temporary r0
// through source 3.
ucode::AluInstruction MakeAluInstruction(ucode::AluVectorOpcode vector_opcode,
                                         ucode::AluScalarOpcode scalar_opcode) {
  uint32_t dwords[3] = {};
  // Scalar opcode in bits 26:31, scalar write mask X in bits 20:23.
  dwords[0] = (uint32_t(scalar_opcode) << 26) | (0b0001u << 20);
  // All swizzles 0 (identity).
  dwords[1] = 0;
  // Vector opcode in bits 24:28; sources 1-3 are temporary registers (bits 29:31).
  dwords[2] = (uint32_t(vector_opcode) << 24) | (0b111u << 29);
  ucode::AluInstruction instr;
  std::memcpy(&instr, dwords, sizeof(instr));
  return instr;
}

ParsedAluInstruction Parse(ucode::AluVectorOpcode vector_opcode,
                           ucode::AluScalarOpcode scalar_opcode) {
  ParsedAluInstruction parsed;
  ParseAluInstruction(MakeAluInstruction(vector_opcode, scalar_opcode), xenos::ShaderType::kPixel,
                      parsed);
  return parsed;
}

}  // namespace

// xenia-canary 92ada8ebc: a single-source two-component scalar operation takes `b` from X,
// or from Z when the co-issued vector operation uses source 3.
TEST_CASE("Two-component scalar operand uses WX with a two-source vector op", "[graphics][alu]") {
  ParsedAluInstruction parsed = Parse(ucode::AluVectorOpcode::kAdd, ucode::AluScalarOpcode::kAdds);
  REQUIRE(parsed.scalar_operand_count == 1);
  const InstructionOperand& operand = parsed.scalar_operands[0];
  REQUIRE(operand.component_count == 2);
  CHECK(operand.components[0] == SwizzleSource::kW);
  CHECK(operand.components[1] == SwizzleSource::kX);
}

TEST_CASE("Two-component scalar operand uses WZ with a three-source vector op",
          "[graphics][alu]") {
  ParsedAluInstruction parsed = Parse(ucode::AluVectorOpcode::kMad, ucode::AluScalarOpcode::kAdds);
  REQUIRE(parsed.scalar_operand_count == 1);
  const InstructionOperand& operand = parsed.scalar_operands[0];
  REQUIRE(operand.component_count == 2);
  CHECK(operand.components[0] == SwizzleSource::kW);
  CHECK(operand.components[1] == SwizzleSource::kZ);
}

TEST_CASE("Single-component scalar operand stays W with a three-source vector op",
          "[graphics][alu]") {
  ParsedAluInstruction parsed = Parse(ucode::AluVectorOpcode::kMad, ucode::AluScalarOpcode::kExp);
  REQUIRE(parsed.scalar_operand_count == 1);
  REQUIRE(parsed.scalar_operands[0].component_count == 1);
  CHECK(parsed.scalar_operands[0].components[0] == SwizzleSource::kW);
}
