/**
 * @file        graphics/edram_tile_count_test.cpp
 * @brief       Tests for the EDRAM tile count: 13-bit bases and the resolve constants' layout
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;

// xenia-canary 4ae7c9f53.
TEST_CASE("RB_COLOR_INFO and RB_DEPTH_INFO bases are 13 bits", "[graphics][edram]") {
  reg::RB_COLOR_INFO color_info;
  color_info.value = 0x1ABC | (uint32_t(xenos::ColorRenderTargetFormat::k_16_16) << 16);
  CHECK(color_info.color_base == 0x1ABC);
  CHECK(color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16);
  reg::RB_DEPTH_INFO depth_info;
  depth_info.value = 0x1FFF | (uint32_t(1) << 16);
  CHECK(depth_info.depth_base == 0x1FFF);
  CHECK(depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8);
}

// xenia-canary 4ae7c9f53.
TEST_CASE("EDRAM bases with a window offset wrap at the tile count", "[graphics][edram]") {
  // Standard: periodic 11-bit addressing.
  CHECK(draw_util::AddWindowOffsetToEdramBase(2040, 16, false, xenos::kEdramTileCount) == 8);
  CHECK(draw_util::AddWindowOffsetToEdramBase(2040, 16, true, xenos::kEdramTileCount) == 24);
  CHECK(draw_util::AddWindowOffsetToEdramBase(10, -16, false, xenos::kEdramTileCount) == 2042);
  // Extended: past 2048 is a separate tile.
  CHECK(draw_util::AddWindowOffsetToEdramBase(2040, 16, false, xenos::kEdramTileCountMax) == 2056);
  CHECK(draw_util::AddWindowOffsetToEdramBase(8190, 4, false, xenos::kEdramTileCountMax) == 2);
}

// The resolve shaders (resolve.xesli) unpack these by hand.
TEST_CASE("Resolve EDRAM info matches the resolve shader's bit layout", "[graphics][edram]") {
  draw_util::ResolveEdramInfo info;
  info.pitch_tiles = 0x3FF;
  info.msaa_samples = xenos::MsaaSamples::k4X;
  info.is_depth = 1;
  info.base_tiles = 0x1ABC;
  info.format = 0xA;
  info.format_is_64bpp = 1;
  info.decode_pwl_gamma = 1;
  uint32_t packed = info.packed;
  CHECK((packed & ((1u << 10) - 1)) == 0x3FF);
  CHECK(((packed >> 10) & 3u) == uint32_t(xenos::MsaaSamples::k4X));
  CHECK((packed & (1u << 12)) != 0);
  CHECK(((packed >> 13) & ((1u << 13) - 1)) == 0x1ABC);
  CHECK(((packed >> 26) & 15u) == 0xA);
  CHECK(((packed >> 30) & 1u) == 1);
  CHECK((packed & (1u << 31)) != 0);
}

TEST_CASE("Resolve coordinate info matches the resolve shader's bit layout",
          "[graphics][edram]") {
  draw_util::ResolveCoordinateInfo info;
  info.draw_resolution_scale_x = 3;
  info.draw_resolution_scale_y = 2;
  info.fill_half_pixel_offset = 1;
  info.edram_tile_count_log2 = 13;
  uint32_t packed = info.packed;
  CHECK(((packed >> 16) & 7u) == 3);
  CHECK(((packed >> 19) & 7u) == 2);
  CHECK((packed & (1u << 22)) != 0);
  CHECK((1u << ((packed >> 23) & 15u)) == xenos::kEdramTileCountMax);
}
