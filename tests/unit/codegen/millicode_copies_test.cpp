/**
 * @file        codegen/millicode_copies_test.cpp
 * @brief       Every copy of the save/restore helpers is found and named
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

// main:   li r3,0; blr
// decoy:  std r14,-152(r1); blr          the head of __savegprlr_14 alone
// save1:  __savegprlr_14..31, stw r12,-8(r1); blr
// rest1:  __restgprlr_14..31, lwz r12,-8(r1); mtlr r12; blr
// save2, rest2: the same again (a second CRT)
constexpr uint32_t kText = 0x82000000;
constexpr uint32_t kPdata = 0x82018000;
constexpr uint32_t kMain = 0x82000000, kDecoy = 0x82000008, kSave1 = 0x82000010,
                   kRest1 = 0x82000060, kSave2 = 0x820000B4, kRest2 = 0x82000104;

void AppendGprRun(std::vector<uint32_t>& words, uint32_t head, bool save) {
  for (uint32_t i = 0; i < 18; ++i)
    words.push_back(head + (i << 21) + i * 8);
  if (save) {
    words.push_back(0x9181FFF8);  // stw r12,-8(r1)
  } else {
    words.push_back(0x8181FFF8);  // lwz r12,-8(r1)
    words.push_back(0x7D8803A6);  // mtlr r12
  }
  words.push_back(0x4E800020);
}

std::vector<uint32_t> TextWords() {
  std::vector<uint32_t> words = {0x38600000, 0x4E800020, 0xF9C1FF68, 0x4E800020};
  for (int copy = 0; copy < 2; ++copy) {
    AppendGprRun(words, 0xF9C1FF68, true);
    AppendGprRun(words, 0xE9C1FF68, false);
  }
  return words;
}

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

class TwoCrtModule : public rex::runtime::Module {
 public:
  TwoCrtModule() : Module(nullptr) {
    text_ = BigEndian(TextWords());
    // main: 2 instructions, 32-bit, no prolog, no handler
    pdata_ = BigEndian({kMain, (2u << 8) | (1u << 30)});
    binary_sections_ = {
        {".text", kText, static_cast<uint32_t>(text_.size()), text_.data(), true, false},
        {".pdata", kPdata, static_cast<uint32_t>(pdata_.size()), pdata_.data(), false, false},
    };
  }
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  uint32_t base_address() const override { return kText; }
  uint32_t image_size() const override { return 0x20000; }
  uint32_t entry_point() const override { return kMain; }
  uint32_t exception_directory_address() const override { return kPdata; }
  uint32_t exception_directory_size() const override {
    return static_cast<uint32_t>(pdata_.size());
  }

 private:
  std::string name_{"two_crt"};
  std::vector<uint8_t> text_, pdata_;
};

std::string NameAt(const CodegenContext& ctx, uint32_t address) {
  const auto* node = ctx.graph.getFunction(address);
  return node ? node->name() : std::string{};
}

}  // namespace

TEST_CASE("Analysis finds and names every copy of the save/restore helpers",
          "[codegen][analysis]") {
  REQUIRE(TextWords().size() * 4 == kRest2 + 21 * 4 - kText);
  TwoCrtModule module;
  RecompilerConfig config;
  FunctionConfig main;
  main.size = 8;
  config.functions[kMain] = main;
  FunctionConfig decoy;
  decoy.size = 8;
  decoy.name = "decoy";
  config.functions[kDecoy] = decoy;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  REQUIRE(Analyze(ctx).has_value());

  const auto& state = ctx.analysisState();
  CHECK(state.saveGpr14Addresses == std::vector<uint32_t>{kSave1, kSave2});
  CHECK(state.restGpr14Addresses == std::vector<uint32_t>{kRest1, kRest2});

  // The first copy keeps the plain names, the second carries its address.
  CHECK(NameAt(ctx, kSave1) == "__savegprlr_14");
  CHECK(NameAt(ctx, kSave1 + 17 * 4) == "__savegprlr_31");
  CHECK(NameAt(ctx, kRest1 + 3 * 4) == "__restgprlr_17");
  CHECK(NameAt(ctx, kSave2) == "__savegprlr_14_820000B4");
  CHECK(NameAt(ctx, kSave2 + 17 * 4) == "__savegprlr_31_820000F8");
  CHECK(NameAt(ctx, kRest2) == "__restgprlr_14_82000104");
  CHECK(IsSaveRestoreHelperName(NameAt(ctx, kSave2)));

  // A lone head is not a run: the decoy stays an ordinary function.
  CHECK(NameAt(ctx, kDecoy) == "decoy");
}
