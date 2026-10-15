/**
 * @file        graphics/pipeline_priority_test.cpp
 * @brief       Tests for the async pipeline compilation priorities
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline_util.h>

using namespace rex::graphics::pipeline_util;

// xenia-canary 5845f3437.
TEST_CASE("Bound RT mask has one bit per RT with any component written", "[graphics][pipeline]") {
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x0000) == 0b0000);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x000F) == 0b0001);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x0008) == 0b0001);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x0010) == 0b0010);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x0100) == 0b0100);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0x8000) == 0b1000);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0xF0F0) == 0b1010);
  CHECK(GetBoundRTMaskFromNormalizedColorMask(0xFFFF) == 0b1111);
}

// xenia-canary 5845f3437.
TEST_CASE("Pipelines writing visible RTs are compiled first", "[graphics][pipeline]") {
  // Writes RT0, which is bound.
  CHECK(CalculatePipelinePriority(0b0001, 0b0001, false) == kPriorityRT0);
  CHECK(CalculatePipelinePriority(0b0011, 0b0011, true) == kPriorityRT0);
  // Writes only a bound RT other than RT0.
  CHECK(CalculatePipelinePriority(0b0010, 0b0011, false) == kPriorityVisibleRT);
  // Writes only unbound RTs, but depth.
  CHECK(CalculatePipelinePriority(0b0100, 0b0011, true) == kPriorityDepthOnly);
  CHECK(CalculatePipelinePriority(0b0000, 0b0000, true) == kPriorityDepthOnly);
  // Writes nothing visible.
  CHECK(CalculatePipelinePriority(0b0100, 0b0011, false) == kPriorityLowest);
  CHECK(CalculatePipelinePriority(0b0000, 0b0000, false) == kPriorityLowest);

  CHECK(kPriorityRT0 > kPriorityVisibleRT);
  CHECK(kPriorityVisibleRT > kPriorityDepthOnly);
  CHECK(kPriorityDepthOnly > kPriorityLowest);
}
