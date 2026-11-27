/**
 * @file        graphics/upscaler_policy_test.cpp
 * @brief       Native temporal upscaler choices without a device: provider
 *              and FSR version selection, color encoding, jitter sequence
 *              length (vendor guides) and the upscale ratio limit
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/native/scene_history.h>
#include <rex/graphics/native/upscaler_policy.h>
#include <rex/ui/vulkan/fsr_versions.h>

namespace native = rex::graphics::native;
namespace scene = rex::graphics::native::scene;
using native::UpscalerColorSpace;
using native::UpscalerProvider;
using F = rex::graphics::xenos::ColorRenderTargetFormat;

TEST_CASE("Upscaler setting names the provider and FSR version", "[graphics][upscaler]") {
  CHECK_FALSE(native::UpscalerOnFor("off"));
  for (const char* value : {"fsr", "fsr3", "fsr4", "xess", "dlss"}) CHECK(native::UpscalerOnFor(value));
  // "fsr", the older value, is FSR 3.1; only fsr4 asks for FSR 4.
  for (const char* value : {"fsr", "fsr3"}) {
    CHECK(native::UpscalerProviderFor(value) == UpscalerProvider::kFsr);
    CHECK(native::UpscalerMajorFor(value) == 3);
  }
  CHECK(native::UpscalerProviderFor("fsr4") == UpscalerProvider::kFsr);
  CHECK(native::UpscalerMajorFor("fsr4") == 4);
  CHECK(native::UpscalerProviderFor("xess") == UpscalerProvider::kXess);
  CHECK(native::UpscalerProviderFor("dlss") == UpscalerProvider::kDlss);
  CHECK(std::string(native::UpscalerName(UpscalerProvider::kFsr)) == "AMD FSR");
  CHECK(std::string(native::UpscalerName(UpscalerProvider::kXess)) == "Intel XeSS");
  CHECK(std::string(native::UpscalerName(UpscalerProvider::kDlss)) == "NVIDIA DLSS");
}

TEST_CASE("Upscaler color space by setting and guest target format", "[graphics][upscaler]") {
  // An explicit setting wins over the format.
  CHECK(native::UpscalerColorSpaceFor("srgb", uint32_t(F::k_16_16_16_16_FLOAT)) == UpscalerColorSpace::kSrgb);
  CHECK(native::UpscalerColorSpaceFor("linear", uint32_t(F::k_8_8_8_8)) == UpscalerColorSpace::kLinear);
  CHECK(native::UpscalerColorSpaceFor("hdr", uint32_t(F::k_8_8_8_8)) == UpscalerColorSpace::kHdr);
  // auto: float and 16-bit fixed point (-32..32) targets hold values above 1.
  for (F format : {F::k_2_10_10_10_FLOAT, F::k_2_10_10_10_FLOAT_AS_16_16_16_16, F::k_16_16, F::k_16_16_16_16,
                   F::k_16_16_FLOAT, F::k_16_16_16_16_FLOAT, F::k_32_FLOAT, F::k_32_32_FLOAT}) {
    INFO(uint32_t(format));
    CHECK(native::UpscalerColorSpaceFor("auto", uint32_t(format)) == UpscalerColorSpace::kHdr);
  }
  for (F format : {F::k_8_8_8_8, F::k_8_8_8_8_GAMMA, F::k_2_10_10_10, F::k_2_10_10_10_AS_10_10_10_10}) {
    INFO(uint32_t(format));
    CHECK(native::UpscalerColorSpaceFor("auto", uint32_t(format)) == UpscalerColorSpace::kSrgb);
  }
  CHECK(std::string(native::UpscalerColorSpaceName(UpscalerColorSpace::kSrgb)) == "srgb");
  CHECK(std::string(native::UpscalerColorSpaceName(UpscalerColorSpace::kLinear)) == "linear");
  CHECK(std::string(native::UpscalerColorSpaceName(UpscalerColorSpace::kHdr)) == "hdr");
}

TEST_CASE("XeSS and DLSS jitter phases follow the vendor guides", "[graphics][upscaler]") {
  // NVIDIA DLSS programming guide 3.7.1.1, minimum phase count per mode
  // (display 1920 wide): DLAA 1.0, Quality 1.5, Balanced 1.724,
  // Performance 2.0, Ultra Performance 3.0.
  const std::array<std::pair<uint32_t, uint32_t>, 5> dlss{{{1920, 8}, {1280, 18}, {1114, 24}, {960, 32}, {640, 72}}};
  for (auto [render, phases] : dlss) {
    INFO(render);
    CHECK(native::UpscalerJitterPhases(render, 1920) == phases);
  }
  // Intel XeSS-SR guide: at least 8 * (scale)^2; Ultra Quality Plus 1.3 and
  // Quality 1.7 round up.
  CHECK(native::UpscalerJitterPhases(1000, 1300) == 14);
  CHECK(native::UpscalerJitterPhases(1000, 1700) == 24);
  // Never fewer than the guides ask for, at any width.
  for (uint32_t render = 640; render <= 1920; ++render) {
    const double ratio = 1920.0 / render;
    INFO(render);
    CHECK(double(native::UpscalerJitterPhases(render, 1920)) >= 8.0 * ratio * ratio - 1e-9);
  }
  // Native AA at or above the output size, no size, and past the 3x limit.
  CHECK(native::UpscalerJitterPhases(1920, 1280) == 8);
  CHECK(native::UpscalerJitterPhases(0, 1920) == 8);
  CHECK(native::UpscalerJitterPhases(480, 1920) == 72);

  // The Halton (2, 3) sequence of that length: distinct sub-pixel offsets in
  // [-0.5, 0.5) render pixels, covering the pixel evenly, then repeating.
  const uint32_t phases = native::UpscalerJitterPhases(640, 1920);
  std::set<std::pair<float, float>> seen;
  double sum_x = 0, sum_y = 0;
  for (uint32_t i = 0; i < phases; ++i) {
    const auto jitter = scene::Jitter(i, phases);
    CHECK((jitter[0] >= -.5f && jitter[0] < .5f));
    CHECK((jitter[1] >= -.5f && jitter[1] < .5f));
    seen.insert({jitter[0], jitter[1]});
    sum_x += jitter[0]; sum_y += jitter[1];
  }
  CHECK(seen.size() == phases);
  CHECK(std::abs(sum_x / phases) < .02);
  CHECK(std::abs(sum_y / phases) < .02);
  CHECK(scene::Jitter(phases, phases) == scene::Jitter(0, phases));
}

TEST_CASE("Upscale ratio limit of XeSS and DLSS", "[graphics][upscaler]") {
  // Ultra Performance, 3x per dimension: scale4 12 over a 1x scene.
  CHECK(native::UpscaleWithin(3.0, 12, 4));
  CHECK_FALSE(native::UpscaleWithin(3.0, 13, 4));
  CHECK(native::UpscaleWithin(3.0, 12, 0));  // No scene scale: 1x.
  CHECK_FALSE(native::UpscaleWithin(3.0, 16, 0));
  // A scene drawn at 2x reaches 6x.
  CHECK(native::UpscaleWithin(3.0, 24, 8));
  CHECK_FALSE(native::UpscaleWithin(3.0, 25, 8));
  // AMD FSR: no limit.
  CHECK(native::UpscaleWithin(0.0, 64, 4));
}

TEST_CASE("FSR version order falls back from FSR 4 to FSR 3.1", "[graphics][upscaler]") {
  using rex::ui::vulkan::FsrHasMajor;
  using rex::ui::vulkan::FsrMajor;
  using rex::ui::vulkan::FsrVersionOrder;
  CHECK(FsrMajor("3.1.4") == 3);
  CHECK(FsrMajor("4.0.2") == 4);
  CHECK(FsrMajor("FSR 3.1.0") == 3);
  CHECK(FsrMajor("10.2") == 10);
  CHECK(FsrMajor("") == 0);
  CHECK(FsrMajor("upscaler") == 0);

  // The loader lists the newest first; FSR 2 is never picked.
  const std::vector<std::string> amd{"4.0.2", "3.1.5", "3.1.4", "2.3.3"};
  CHECK(FsrHasMajor(amd, 4));
  CHECK_FALSE(FsrHasMajor(amd, 5));
  CHECK(FsrVersionOrder(amd, 4) == std::vector<size_t>{0, 1, 2});
  CHECK(FsrVersionOrder(amd, 3) == std::vector<size_t>{1, 2});
  // Without FSR 4 (older GPU or DLL set), fsr4 runs FSR 3.1.
  const std::vector<std::string> older{"3.1.5", "3.1.4"};
  CHECK_FALSE(FsrHasMajor(older, 4));
  CHECK(FsrVersionOrder(older, 4) == std::vector<size_t>{0, 1});
  // A list in another order still tries the wanted major first.
  CHECK(FsrVersionOrder({"3.1.4", "4.0.2"}, 4) == std::vector<size_t>{1, 0});
  CHECK(FsrVersionOrder({}, 4).empty());
  CHECK(FsrVersionOrder({"2.3.3"}, 3).empty());
}
