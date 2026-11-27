/**
 * @file        codegen/delay_hint_test.cpp
 * @brief       Spin hints compile to a pause: db16cyc and cctpl (or r1,r1,r1);
 *              the shorter delay hints stay empty
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

class WordsModule : public rex::runtime::Module {
 public:
  explicit WordsModule(const std::vector<uint32_t>& words) : Module(nullptr) {
    text_ = BigEndian(words);
    pdata_ = BigEndian({kText, (static_cast<uint32_t>(words.size()) << 8) | (1u << 30)});
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
  std::string name_{"delay_hints"};
  std::vector<uint8_t> text_, pdata_;
};

std::string Emit(const std::vector<uint32_t>& words) {
  WordsModule module(words);
  RecompilerConfig config;
  FunctionConfig fn;
  fn.size = static_cast<uint32_t>(words.size() * 4);
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

constexpr uint32_t kDb16cyc = 0x7FFFFB78;  // or r31,r31,r31
constexpr uint32_t kCctpl = 0x7C210B78;    // or r1,r1,r1
constexpr uint32_t kDb8cyc = 0x7F9CE378;   // or r28,r28,r28
constexpr uint32_t kBlr = 0x4E800020;

}  // namespace

TEST_CASE("db16cyc and cctpl pause, the shorter delay hints emit nothing", "[codegen][ppc]") {
  const std::string spin = Emit({kDb16cyc, kCctpl, kBlr});
  INFO(spin);
  CHECK(Count(spin, "simde_mm_pause();") == 2);

  const std::string short_hint = Emit({kDb8cyc, kBlr});
  INFO(short_hint);
  CHECK(Count(short_hint, "simde_mm_pause();") == 0);
}
