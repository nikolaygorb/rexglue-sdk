/**
 * @file        graphics/async_pipeline_wait_test.cpp
 * @brief       Tests which draws wait for a pipeline still being created (xenia-canary f9431eb95)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/pipeline_wait.h>
#include <rex/graphics/xenos.h>

namespace {

using namespace rex::graphics;

bool WaitNeeded(uint32_t pitch, xenos::MsaaSamples msaa_samples) {
  auto regs = std::make_unique<RegisterFile>();
  std::memset(regs->values, 0, sizeof(regs->values));
  reg::RB_SURFACE_INFO surface_info{};
  surface_info.surface_pitch = pitch;
  surface_info.msaa_samples = msaa_samples;
  (*regs)[XE_GPU_REG_RB_SURFACE_INFO] = surface_info.value;
  return draw_util::IsAsyncPipelineWaitNeededForDraw(*regs);
}

}  // namespace

TEST_CASE("Draws to surfaces at most 2 tiles wide wait for their pipeline",
          "[graphics][pipeline]") {
  // 1x1 eye adaptation history, 160 pixels = 2 tiles.
  CHECK(WaitNeeded(1, xenos::MsaaSamples::k1X));
  CHECK(WaitNeeded(160, xenos::MsaaSamples::k1X));
  CHECK_FALSE(WaitNeeded(161, xenos::MsaaSamples::k1X));
  CHECK_FALSE(WaitNeeded(1280, xenos::MsaaSamples::k1X));
  // 2x MSAA doubles the samples vertically only; 4x doubles the width.
  CHECK(WaitNeeded(160, xenos::MsaaSamples::k2X));
  CHECK(WaitNeeded(80, xenos::MsaaSamples::k4X));
  CHECK_FALSE(WaitNeeded(81, xenos::MsaaSamples::k4X));
  // No surface (memexport-only, pitch 0): not decided by the width.
  CHECK_FALSE(WaitNeeded(0, xenos::MsaaSamples::k1X));
}

TEST_CASE("A draw that must not be skipped waits until its pipeline is created",
          "[graphics][pipeline]") {
  CHECK(PipelineWaitBudgetMs(true, 0) == kPipelineWaitUntilCreated);
  CHECK(PipelineWaitBudgetMs(true, 5) == kPipelineWaitUntilCreated);
  CHECK(PipelineWaitBudgetMs(false, 5) == 5);
  CHECK(PipelineWaitBudgetMs(false, 0) == 0);
}
