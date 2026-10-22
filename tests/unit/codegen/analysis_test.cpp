/**
 * @file        codegen/analysis_test.cpp
 * @brief       Tests for function discovery on a synthetic image
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

// Assembled from (labels as in the comments below):
//
//   main:   bl a; bl h; bl caller; b f        config entry, 16 bytes
//   a:      li r3,0; blr
//   t:      addi r3,r3,4; b g                 thunk to code no function owns yet
//   f:      li r3,1; blr                      only main's `b` reaches it
//   g:      li r3,2; blr
//   p0:     addi r3,r3,-4                     this-adjusting thunk...
//   p1:     b a                               ...falling into the plain one
//   h:      li r4,3
//   h_loop: addic. r4,r4,-1; bne h_loop; blr
//   w1:     cmplwi cr6,r4,2; mr r30,r4; bgt w1_default; (table1 lookup by r30)
//   w2:     mr r30,r4; cmplwi cr6,r4,1; bgt w2_default; (table2 lookup by r30)
//   caller: bl w1; bl w2; bl sj; bl lj; blr   the only PDATA function
//   sj:     setjmp of the XDK CRT (mflr; mfcr; stfd f14-f31; std r13,152(r3))
//   lj:     longjmp of the XDK CRT (mr r7,r3; lfd f14-f31; ld r13,152(r7))
//   x:      li r3,0; b x                      first gap fill pass keeps y with it
//   y:      subfe r3,r3,r4; blr               an instruction ppc/opcode.h lacks
//   w3:     cmplwi cr6,r4,1; ble cr6,w3_dispatch; b w3_default
//   w3_dispatch: (table3 lookup by r4)
//   o:      mtctr r4                          with t, overlapping when both are
//   t:      addi r3,r3,1; bdnz t; blr         config entries
//
//   .rodata vtable: p0, p1, h_loop
//           table1: w1_c0, w1_c1, w1_c2, w1_c0, w1_c1, 0
//           table2: w2_c0, w2_c1, w2_c0, w2_c1, 0
//           table3: w3_c0, w3_c1, w3_c0, w3_c1, 0
constexpr uint32_t kText = 0x82000000;
constexpr uint32_t kRodata = 0x82010000;
constexpr uint32_t kPdata = 0x82018000;

constexpr uint32_t kMain = 0x82000000, kT = 0x82000018, kF = 0x82000020, kP0 = 0x82000030,
                   kP1 = 0x82000034, kHLoop = 0x8200003C, kW1 = 0x82000048, kW2 = 0x8200008C,
                   kCaller = 0x820000C8, kSetjmp = 0x820000DC, kLongjmp = 0x8200013C,
                   kX = 0x8200019C, kY = 0x820001A4, kW3 = 0x820001AC,
                   kO = 0x820001E8, kTl = 0x820001EC;

const std::vector<uint32_t> kTextWords = {
    // main
    0x48000011, 0x48000035, 0x480000C1, 0x48000014,
    // a
    0x38600000, 0x4E800020,
    // t
    0x38630004, 0x4800000C,
    // f
    0x38600001, 0x4E800020,
    // g
    0x38600002, 0x4E800020,
    // p0, p1
    0x3863FFFC, 0x4BFFFFDC,
    // h, h_loop
    0x38800003, 0x3484FFFF, 0x4082FFFC, 0x4E800020,
    // w1
    0x2B040002, 0x7C9E2378, 0x41990034, 0x3D808201, 0x398C000C, 0x57C0103A, 0x7C0C002E,
    0x7C0903A6, 0x4E800420,
    // w1_c0, w1_c1, w1_c2, w1_default
    0x38600000, 0x4E800020, 0x38600001, 0x4E800020, 0x38600002, 0x4E800020, 0x3860FFFF,
    0x4E800020,
    // w2
    0x7C9E2378, 0x2B040001, 0x4199002C, 0x3D808201, 0x398C0024, 0x57C0103A, 0x7C0C002E,
    0x7C0903A6, 0x4E800420,
    // w2_c0, w2_c1, w2_default
    0x3860000A, 0x4E800020, 0x3860000B, 0x4E800020, 0x3860FFFF, 0x4E800020,
    // caller
    0x4BFFFF81, 0x4BFFFFC1, 0x4800000D, 0x48000069, 0x4E800020,
    // sj
    0x7C0802A6, 0x7C800026, 0xD9C30000, 0xD9E30008, 0xDA030010, 0xDA230018, 0xDA430020,
    0xDA630028, 0xDA830030, 0xDAA30038, 0xDAC30040, 0xDAE30048, 0xDB030050, 0xDB230058,
    0xDB430060, 0xDB630068, 0xDB830070, 0xDBA30078, 0xDBC30080, 0xDBE30088, 0xF9A30098,
    0x90030134, 0x38600000, 0x4E800020,
    // lj
    0x7C671B78, 0xC9C70000, 0xC9E70008, 0xCA070010, 0xCA270018, 0xCA470020, 0xCA670028,
    0xCA870030, 0xCAA70038, 0xCAC70040, 0xCAE70048, 0xCB070050, 0xCB270058, 0xCB470060,
    0xCB670068, 0xCB870070, 0xCBA70078, 0xCBC70080, 0xCBE70088, 0xE9A70098, 0x80A70134,
    0x7CA803A6, 0x7C832378, 0x4E800020,
    // x, y
    0x38600000, 0x4BFFFFFC, 0x7C632110, 0x4E800020,
    // w3, w3_dispatch
    0x2B040001, 0x40990008, 0x4800002C, 0x3D808201, 0x398C0038, 0x5480103A, 0x7C0C002E,
    0x7C0903A6, 0x4E800420,
    // w3_c0, w3_c1, w3_default
    0x38600014, 0x4E800020, 0x38600015, 0x4E800020, 0x3860FFFF, 0x4E800020,
    // o, t
    0x7C8903A6, 0x38630001, 0x4200FFFC, 0x4E800020,
    // r1: li r4,0; li r3,0; bl a      a catch funclet ending in a rethrow
    // r2: li r3,7; blr                the next funclet, right after it
    0x38800000, 0x38600000, 0x4BFFFE11, 0x38600007, 0x4E800020,
};

const std::vector<uint32_t> kRodataWords = {
    // vtable
    0x82000030, 0x82000034, 0x8200003C,
    // table1
    0x8200006C, 0x82000074, 0x8200007C, 0x8200006C, 0x82000074, 0x00000000,
    // table2
    0x820000B0, 0x820000B8, 0x820000B0, 0x820000B8, 0x00000000,
    // table3
    0x820001D0, 0x820001D8, 0x820001D0, 0x820001D8, 0x00000000,
};

// caller: 5 instructions, 32-bit, no prolog, no handler
const std::vector<uint32_t> kPdataWords = {kCaller, (5u << 8) | (1u << 30)};

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

class SyntheticModule : public rex::runtime::Module {
 public:
  SyntheticModule() : Module(nullptr) {
    text_ = BigEndian(kTextWords);
    rodata_ = BigEndian(kRodataWords);
    pdata_ = BigEndian(kPdataWords);
    binary_sections_ = {
        {".text", kText, static_cast<uint32_t>(text_.size()), text_.data(), true, false},
        {".rdata", kRodata, static_cast<uint32_t>(rodata_.size()), rodata_.data(), false, false},
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
  std::string name_{"synthetic"};
  std::vector<uint8_t> text_, rodata_, pdata_;
};

struct Analyzed {
  CodegenContext ctx;
  std::string error;  // empty on success
};

Analyzed AnalyzeSynthetic(RecompilerConfig config = {}) {
  SyntheticModule module;
  FunctionConfig main;
  main.size = 16;
  config.functions[kMain] = main;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  auto result = Analyze(ctx);
  std::string error;
  if (!result) {
    error = result.error().what();
    for (const auto& entry : ctx.errors.Entries())
      error += "\n  " + entry.message;
  }
  return {std::move(ctx), error};
}

std::string EmitFunction(const CodegenContext& ctx, uint32_t function) {
  const auto* node = ctx.graph.getFunction(function);
  if (!node)
    return {};
  return node->emitCpp(EmitContext{ctx.binary(), ctx.Config(), ctx.graph});
}

size_t JumpTableSize(const CodegenContext& ctx, uint32_t function) {
  const auto* node = ctx.graph.getFunction(function);
  if (!node || node->jumpTables().size() != 1)
    return 0;
  return node->jumpTables().front().targets.size();
}

}  // namespace

TEST_CASE("Analysis registers a branch target no function covers", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  // f is reached only by main's `b`; gap fill had glued it to the thunk t.
  CHECK(ctx.graph.isEntryPoint(kF));
  CHECK(ctx.graph.isEntryPoint(kT));
}

TEST_CASE("Analysis registers code pointers that are not labels", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  // p1 is only reached by falling through from p0, but the vtable holds it.
  CHECK(ctx.graph.isEntryPoint(kP0));
  CHECK(ctx.graph.isEntryPoint(kP1));
  // h_loop is in the vtable data too, but it is a loop label inside h.
  CHECK_FALSE(ctx.graph.isEntryPoint(kHLoop));
}

TEST_CASE("Analysis bounds a jump table through a copy of the index", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  // Without the bound both tables would run on into the next words.
  CHECK(JumpTableSize(ctx, kW1) == 3);
  CHECK(JumpTableSize(ctx, kW2) == 2);
}

TEST_CASE("Analysis bounds a jump table checked before a branch to it", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  // The bounds check sits before `ble w3_dispatch; b w3_default`; without
  // following that branch the table runs on to its terminating 0.
  CHECK(JumpTableSize(ctx, kW3) == 2);
}

TEST_CASE("Analysis finds the CRT setjmp and longjmp", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  CHECK(ctx.Config().setJmpAddress == kSetjmp);
  CHECK(ctx.Config().longJmpAddress == kLongjmp);
}

TEST_CASE("Analysis takes code the analysis decoder does not know", "[codegen][analysis]") {
  auto [ctx, error] = AnalyzeSynthetic();
  INFO(error);
  REQUIRE(error.empty());
  // x's gap fill ends at y's blr, but x's blocks stop at `b x`; a later pass
  // finds y. `subfe` is not in ppc/opcode.h, the emitter translates it.
  CHECK(ctx.graph.isEntryPoint(kX));
  CHECK(ctx.graph.isEntryPoint(kY));
}

TEST_CASE("A bdnz to the entry of an overlapping function is a tail call", "[codegen][analysis]") {
  // o and t overlap; o's discovery sees t as a function, so t is no label of
  // o. The address range said "inside o" and emitted a goto to a label o
  // never has (Forza Horizon, upstream PR #434).
  RecompilerConfig config;
  FunctionConfig o, t;
  o.size = 0x10;
  t.size = 0x0C;
  config.functions[kO] = o;
  config.functions[kTl] = t;
  auto [ctx, error] = AnalyzeSynthetic(std::move(config));
  INFO(error);
  REQUIRE(error.empty());
  const std::string code = EmitFunction(ctx, kO);
  INFO(code);
  REQUIRE_FALSE(code.empty());
  CHECK(code.find("goto loc_820001EC;") == std::string::npos);
  CHECK(code.find("sub_820001EC(ctx, base);") != std::string::npos);
}

TEST_CASE("A call followed by another function ends the function", "[codegen][analysis]") {
  // A catch funclet that rethrows ends in bl _CxxThrowException, and the next
  // funclet follows; neither has a .pdata size. Running on, the first took the
  // next one's code and branched to labels it doesn't own (issue #511).
  constexpr uint32_t kR1 = 0x820001F8, kR2 = 0x82000204;
  RecompilerConfig config;
  config.functions[kR1] = FunctionConfig{};
  config.functions[kR2] = FunctionConfig{};
  auto [ctx, error] = AnalyzeSynthetic(std::move(config));
  INFO(error);
  REQUIRE(error.empty());
  const std::string first = EmitFunction(ctx, kR1);
  const std::string second = EmitFunction(ctx, kR2);
  INFO(first);
  REQUIRE_FALSE(first.empty());
  CHECK(first.find("s64 = 7;") == std::string::npos);
  CHECK(second.find("s64 = 7;") != std::string::npos);
}

TEST_CASE("Analysis keeps setjmp and longjmp from the config", "[codegen][analysis]") {
  RecompilerConfig config;
  config.setJmpAddress = kCaller;
  auto [ctx, error] = AnalyzeSynthetic(std::move(config));
  INFO(error);
  REQUIRE(error.empty());
  CHECK(ctx.Config().setJmpAddress == kCaller);
  CHECK(ctx.Config().longJmpAddress == kLongjmp);
}

TEST_CASE("A branch is classified by the function being emitted", "[codegen][analysis]") {
  // outer covers 0x00-0x40 and inner, an entry inside it, 0x10-0x20. A branch
  // of outer at 0x14 lies in both; its target 0x30 only in outer. The function
  // containing 0x14 is inner, so without the caller the label looks foreign
  // (PDZ 0x82664058).
  constexpr uint32_t kOuter = 0x82000000, kInner = 0x82000010;
  constexpr uint32_t kSite = 0x82000014, kTarget = 0x82000030;
  FunctionGraph graph;
  const auto* outer = graph.addFunction(kOuter, 0x40, FunctionAuthority::DISCOVERED);
  const auto* inner = graph.addFunction(kInner, 0x10, FunctionAuthority::DISCOVERED);
  REQUIRE(graph.getFunctionContaining(kSite) == inner);

  CHECK(graph.classifyTarget(kTarget, kSite, false) == TargetKind::Unknown);
  CHECK(graph.classifyTarget(kTarget, kSite, false, outer) == TargetKind::InternalLabel);
  // inner's own code still resolves against inner.
  CHECK(graph.classifyTarget(kInner, kSite, false, inner) == TargetKind::InternalLabel);
  CHECK(graph.classifyTarget(kInner, kSite, false, outer) == TargetKind::Function);
}

TEST_CASE("A tail call follows a function registered again", "[codegen][analysis]") {
  // Gap fill removes a function and registers it again to discover it anew.
  // The callers' edges go back to Unresolved on removal; left that way, a
  // conditional tail call would emit nothing.
  constexpr uint32_t kCaller = 0x82000000, kCallee = 0x82000020, kSite = 0x82000008;
  FunctionGraph graph;
  graph.addFunction(kCaller, 0x10, FunctionAuthority::DISCOVERED);
  auto* callee = graph.addFunction(kCallee, 0x10, FunctionAuthority::DISCOVERED);
  graph.addTailCallToFunction(kCaller, kSite, CallTarget::function(callee));

  graph.removeFunction(kCallee);
  const auto& edge = graph.getFunction(kCaller)->tailCalls().front();
  REQUIRE_FALSE(edge.target.isResolved());

  auto* again = graph.addFunction(kCallee, 4, FunctionAuthority::DISCOVERED);
  CHECK(edge.target.asFunction() == again);
}
