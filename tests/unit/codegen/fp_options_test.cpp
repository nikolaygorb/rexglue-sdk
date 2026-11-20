/**
 * @file        codegen/fp_options_test.cpp
 * @brief       Codegen options for scalar floating point
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <rex/codegen/analyze.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_graph.h>
#include <rex/system/module.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kText = 0x82000000;
constexpr uint32_t kPdata = 0x82018000;

// fadds f1,f2,f3; fmuls f1,f2,f3; fdivs f1,f2,f3; fmadds f1,f2,f3,f4;
// fsqrts f1,f2; blr
const std::vector<uint32_t> kWords = {0xEC22182A, 0xEC2200F2, 0xEC221824,
                                      0xEC2220FA, 0xEC20102C, 0x4E800020};

std::vector<uint8_t> BigEndian(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  for (uint32_t w : words) {
    bytes.push_back(static_cast<uint8_t>(w >> 24));
    bytes.push_back(static_cast<uint8_t>(w >> 16));
    bytes.push_back(static_cast<uint8_t>(w >> 8));
    bytes.push_back(static_cast<uint8_t>(w));
  }
  return bytes;
}

class SingleFpModule : public rex::runtime::Module {
 public:
  SingleFpModule() : Module(nullptr) {
    text_ = BigEndian(kWords);
    pdata_ = BigEndian({kText, (static_cast<uint32_t>(kWords.size()) << 8) | (1u << 30)});
    binary_sections_ = {
        {".text", kText, static_cast<uint32_t>(text_.size()), text_.data(), true, false},
        {".pdata", kPdata, static_cast<uint32_t>(pdata_.size()), pdata_.data(), false, false},
    };
  }
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  uint32_t base_address() const override { return kText; }
  uint32_t image_size() const override { return 0x20000; }
  uint32_t entry_point() const override { return kText; }
  uint32_t exception_directory_address() const override { return kPdata; }
  uint32_t exception_directory_size() const override {
    return static_cast<uint32_t>(pdata_.size());
  }

 private:
  std::string name_{"single_fp"};
  std::vector<uint8_t> text_, pdata_;
};

std::string Emit(RecompilerConfig config) {
  SingleFpModule module;
  FunctionConfig fn;
  fn.size = static_cast<uint32_t>(kWords.size() * 4);
  config.functions[kText] = fn;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  REQUIRE(Analyze(ctx).has_value());
  const auto* node = ctx.graph.getFunction(kText);
  REQUIRE(node != nullptr);
  return node->emitCpp(EmitContext{ctx.binary(), ctx.Config(), ctx.graph});
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
    ++n;
  return n;
}

}  // namespace

TEST_CASE("single_denormal_operand_quirk decides the denormal operand checks",
          "[codegen][fp]") {
  RecompilerConfig quirk;
  const std::string checked = Emit(quirk);
  INFO(checked);
  CHECK(Count(checked, "fp_single_denormal_operand(") == 5);

  RecompilerConfig plain;
  plain.singleDenormalOperandQuirk = false;
  const std::string unchecked = Emit(plain);
  INFO(unchecked);
  CHECK(Count(unchecked, "fp_single_denormal_operand(") == 0);
  CHECK(Count(unchecked, "fp_opaque(") == 0);
  // Every result is still rounded to single.
  CHECK(Count(unchecked, "double(float(") == 5);
}
