/**
 * @file        graphics/resolve_clear_value_test.cpp
 * @brief       Tests for the color clear values passed to the resolve clear shaders
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/util/draw.h>

using namespace rex::graphics;

namespace {

draw_util::ResolveInfo MakeColorClear(bool format_is_64bpp) {
  draw_util::ResolveInfo info = {};
  info.rb_copy_control.copy_src_select = 0;
  info.rb_copy_control.color_clear_enable = 1;
  info.color_edram_info.format_is_64bpp = format_is_64bpp;
  info.rb_color_clear = 0xAAAABBBB;
  info.rb_color_clear_lo = 0xCCCCDDDD;
  return info;
}

}  // namespace

// xenia-canary 16e1eb8e2.
TEST_CASE("64bpp color clear takes the low dword from RB_COLOR_CLEAR_LO", "[graphics][resolve]") {
  draw_util::ResolveClearShaderConstants constants = {};
  MakeColorClear(true).GetColorClearShaderConstants(constants);
  CHECK(constants.rt_specific.clear_value[0] == 0xCCCCDDDD);
  CHECK(constants.rt_specific.clear_value[1] == 0xAAAABBBB);
  CHECK(constants.rt_specific.edram_info.format_is_64bpp == 1);
}

// xenia-canary 16e1eb8e2.
TEST_CASE("32bpp color clear uses only RB_COLOR_CLEAR", "[graphics][resolve]") {
  draw_util::ResolveClearShaderConstants constants = {};
  MakeColorClear(false).GetColorClearShaderConstants(constants);
  CHECK(constants.rt_specific.clear_value[0] == 0xAAAABBBB);
  CHECK(constants.rt_specific.clear_value[1] == 0xAAAABBBB);
}
