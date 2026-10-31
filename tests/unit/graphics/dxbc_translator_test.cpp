/**
 * @file        graphics/dxbc_translator_test.cpp
 * @brief       Xenos microcode analysis and the DXBC translator on malformed
 *              microcode
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <iterator>
#include <string>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

// Found by shader_dxbc_fuzzer: the instruction range of an exec was read
// without checking it against the microcode size.
TEST_CASE("An exec past the end of the microcode is not read", "[graphics][shader]") {
  // One control flow pair, so one instruction slot: exece at slot 1, six ALU
  // instructions, then a nop.
  const uint32_t ucode[] = {
      1 | (6 << 12),
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12,
      0,
  };
  DxbcShader shader(xenos::ShaderType::kVertex, 0, ucode, std::size(ucode), std::endian::native);
  rex::string::StringBuffer ucode_disasm_buffer;
  shader.AnalyzeUcode(ucode_disasm_buffer);

  DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, false);
  Shader::Translation* translation = shader.GetOrCreateTranslation(
      translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(0)));
  CHECK_FALSE(translator.TranslateAnalyzedShader(*translation));
  CHECK_FALSE(translation->is_valid());
}

namespace {

// A vertex shader of one exece running a single fetch instruction at slot 1.
std::string DisassembleSingleFetch(uint32_t fetch_dword_0, uint32_t fetch_dword_1) {
  const uint32_t ucode[] = {
      1 | (1 << 12) | (0b01 << 16),
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12,
      0,
      fetch_dword_0,
      fetch_dword_1,
      0,
  };
  DxbcShader shader(xenos::ShaderType::kVertex, 0, ucode, std::size(ucode), std::endian::native);
  rex::string::StringBuffer ucode_disasm_buffer;
  shader.AnalyzeUcode(ucode_disasm_buffer);
  return shader.ucode_disassembly();
}

}  // namespace

// Found by shader_dxbc_fuzzer: the disassembly formatted a null name.
TEST_CASE("A vertex fetch of an unnamed data format disassembles", "[graphics][shader]") {
  // vfetch_full (must_be_one set) with data format 1, which has no name.
  std::string disassembly = DisassembleSingleFetch(1 << 19, 1 << 16);
  CHECK(disassembly.find("DataFormat=1") != std::string::npos);
}

// Found by shader_dxbc_fuzzer: the memory export analysis followed control
// flow past the last control flow instruction.
TEST_CASE("Memory export analysis stops at the end of the control flow", "[graphics][shader]") {
  // exec (not the end) of one ALU instruction at slot 1, then a nop: control
  // flow falls through the nop past the program. The instruction writes eM0
  // (vector_dest 33, export_data, vector write mask xyzw).
  const uint32_t ucode[] = {
      1 | (1 << 12),
      uint32_t(ucode::ControlFlowOpcode::kExec) << 12,
      0,
      33 | (1 << 15) | (0b1111 << 16),
      0,
      0,
  };
  DxbcShader shader(xenos::ShaderType::kVertex, 0, ucode, std::size(ucode), std::endian::native);
  rex::string::StringBuffer ucode_disasm_buffer;
  shader.AnalyzeUcode(ucode_disasm_buffer);
  CHECK(shader.memexport_eM_written() == 0b1);
  CHECK(shader.memexport_eM_potentially_written_before_end() == 0b1);
}

TEST_CASE("A fetch of an unknown opcode disassembles", "[graphics][shader]") {
  // Fetch opcode 2 is not defined.
  std::string disassembly = DisassembleSingleFetch(2, 0);
  CHECK(disassembly.find("tfetch_unknown") != std::string::npos);
}
