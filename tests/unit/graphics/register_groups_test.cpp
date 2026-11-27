/**
 * @file        graphics/register_groups_test.cpp
 * @brief       gpu_register_group_survey: the group map covers every register
 *              once, and the census counts which groups each draw changed
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/register_groups.h>

namespace groups = rex::graphics::register_groups;

TEST_CASE("Register groups cover the register file in order, without gaps",
          "[graphics][register_groups]") {
  static_assert(groups::kRegisterEnd == rex::graphics::RegisterFile::kRegisterCount);
  uint32_t next = 0;
  for (const groups::Range& range : groups::kRanges) {
    INFO(range.first);
    CHECK(range.first == next);
    CHECK(range.last >= range.first);
    next = range.last + 1;
  }
  CHECK(next == groups::kRegisterEnd);
  // A few known registers.
  CHECK(groups::GroupOf(0x2000) == groups::kRenderTargets);   // RB_SURFACE_INFO
  CHECK(groups::GroupOf(0x2180) == groups::kPipelineState);   // SQ_PROGRAM_CNTL
  CHECK(groups::GroupOf(0x210F) == groups::kDynamicState);    // PA_CL_VPORT_XSCALE
  CHECK(groups::GroupOf(0x21FC) == groups::kDrawSetup);       // VGT_DRAW_INITIATOR
  CHECK(groups::GroupOf(0x2318) == groups::kResolve);         // RB_COPY_CONTROL
  CHECK(groups::GroupOf(0x4400) == groups::kPixelConstants);
  CHECK(groups::GroupOf(0x4927) == groups::kBoolLoopConstants);
  for (uint32_t i = 0; i < groups::kRegisterEnd; ++i) {
    if (groups::GroupTable()[i] != groups::GroupOf(i)) {
      FAIL("GroupTable differs from GroupOf at " << i);
    }
  }
}

TEST_CASE("A fetch slot's change is a texture's, a sampler's or a vertex buffer's",
          "[graphics][register_groups]") {
  const uint32_t texture[6] = {0x00000002, 0x1000, 0x2000, 0, 0, 0};
  uint32_t changed[6] = {0x00000002, 0x1000, 0x2000, 0, 0, 0};
  CHECK(groups::FetchSlotChange(texture, changed) == 0);
  changed[1] = 0x1100;  // the address
  CHECK(groups::FetchSlotChange(texture, changed) == 1u << groups::kTextures);
  changed[1] = 0x1000;
  changed[0] |= 0x00000400;  // a clamp mode
  CHECK(groups::FetchSlotChange(texture, changed) == 1u << groups::kSamplers);
  changed[4] = 1;  // word 4: all sampler bits
  changed[2] = 0x2001;
  CHECK(groups::FetchSlotChange(texture, changed) ==
        ((1u << groups::kSamplers) | (1u << groups::kTextures)));
  const uint32_t vertex[6] = {0x00000003, 0x10, 0x00000003, 0x20, 0x00000003, 0x30};
  uint32_t vertex_changed[6] = {0x00000003, 0x10, 0x00000003, 0x24, 0x00000003, 0x30};
  CHECK(groups::FetchSlotChange(vertex, vertex_changed) == 1u << groups::kVertexFetch);
}

TEST_CASE("The register group census counts unchanged groups draw by draw",
          "[graphics][register_groups]") {
  std::vector<uint32_t> registers(groups::kRegisterEnd, 0);
  groups::RegisterGroupSurvey survey;
  CHECK(survey.Observe(registers.data()) == 0);  // the first draw only takes the copy
  CHECK(survey.compared() == 0);
  CHECK(survey.Format(1).empty());

  // Draw 2: the same state.
  CHECK(survey.Observe(registers.data()) == 0);
  // Draw 3: a pixel constant and the draw initiator.
  registers[0x4400] = 1;
  registers[0x21FC] = 7;
  CHECK(survey.Observe(registers.data()) ==
        ((1u << groups::kPixelConstants) | (1u << groups::kDrawSetup)));
  // Draw 4: the viewport only (dynamic state is not a binding).
  registers[0x210F] = 0x3F800000;
  CHECK(survey.Observe(registers.data()) == 1u << groups::kDynamicState);
  // A system register outside the compared ranges is never seen.
  registers[0x0100] = 5;
  CHECK(survey.Observe(registers.data()) == 0);

  CHECK(survey.compared() == 4);
  CHECK(survey.changed(groups::kPixelConstants) == 1);
  CHECK(survey.changed(groups::kDynamicState) == 1);
  CHECK(survey.changed(groups::kRenderTargets) == 0);
  CHECK(survey.binding_unchanged() == 3);  // all but draw 3
  const std::string line = survey.Format(2);
  INFO(line);
  CHECK(line.find("2 draws a frame") == 0);
  CHECK(line.find("pixel_constants 75.0%") != std::string::npos);
  CHECK(line.find("all bindings 75.0%") != std::string::npos);

  survey.ResetCounts();
  CHECK(survey.compared() == 0);
  CHECK(survey.Observe(registers.data()) == 0);  // compared with the draw before the reset
  CHECK(survey.compared() == 1);
}
