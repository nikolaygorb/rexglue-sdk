/**
 * @file        ui/presenter_paint_config_test.cpp
 * @brief       Tests for the guest output paint configuration of the presenter
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/presenter.h>

namespace {

using GuestOutputPaintConfig = rex::ui::Presenter::GuestOutputPaintConfig;

}  // namespace

// CAS and FSR 1 only need the built-in shaders. They used to be compiled only
// with the optional FidelityFX SDK, so default builds offered just bilinear.
TEST_CASE("Paint config offers CAS and FSR without the FidelityFX SDK", "[ui][presenter]") {
  GuestOutputPaintConfig config;
  CHECK(config.GetEffect() == GuestOutputPaintConfig::Effect::kBilinear);
  config.SetEffect(GuestOutputPaintConfig::Effect::kCas);
  CHECK(config.GetEffect() == GuestOutputPaintConfig::Effect::kCas);
  config.SetEffect(GuestOutputPaintConfig::Effect::kFsr);
  CHECK(config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr);
}

TEST_CASE("Paint config has the Xenia defaults", "[ui][presenter]") {
  GuestOutputPaintConfig config;
  CHECK(config.GetCasAdditionalSharpness() ==
        GuestOutputPaintConfig::kCasAdditionalSharpnessDefault);
  CHECK(config.GetFsrMaxUpsamplingPasses() == GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax);
  CHECK(config.GetFsrSharpnessReduction() ==
        GuestOutputPaintConfig::kFsrSharpnessReductionDefault);
  CHECK(config.GetFsrQualityMode() == GuestOutputPaintConfig::FsrQualityMode::kAuto);
  CHECK_FALSE(config.GetDither());
}

TEST_CASE("Paint config clamps sharpness and pass count", "[ui][presenter]") {
  GuestOutputPaintConfig config;

  config.SetCasAdditionalSharpness(2.0f);
  CHECK(config.GetCasAdditionalSharpness() == GuestOutputPaintConfig::kCasAdditionalSharpnessMax);
  config.SetCasAdditionalSharpness(-1.0f);
  CHECK(config.GetCasAdditionalSharpness() == GuestOutputPaintConfig::kCasAdditionalSharpnessMin);
  config.SetCasAdditionalSharpness(std::numeric_limits<float>::quiet_NaN());
  CHECK_FALSE(std::isnan(config.GetCasAdditionalSharpness()));

  config.SetFsrSharpnessReduction(5.0f);
  CHECK(config.GetFsrSharpnessReduction() == GuestOutputPaintConfig::kFsrSharpnessReductionMax);
  config.SetFsrSharpnessReduction(std::numeric_limits<float>::quiet_NaN());
  CHECK_FALSE(std::isnan(config.GetFsrSharpnessReduction()));

  config.SetFsrMaxUpsamplingPasses(0);
  CHECK(config.GetFsrMaxUpsamplingPasses() == 1);
  config.SetFsrMaxUpsamplingPasses(100);
  CHECK(config.GetFsrMaxUpsamplingPasses() == GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax);
}
