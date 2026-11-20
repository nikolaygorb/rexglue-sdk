/**
 * @file        graphics/render_area_test.cpp
 * @brief       Tests for the tight render area of Vulkan render passes
 *
 * @copyright   Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp
 *              contributors. All rights reserved.
 *              Ported for the ReXGlue fork from gears-of-war-2-pc-port
 *              sdk/patches/0089 (tests/unit/graphics/render_area_test.cpp).
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/vulkan/render_area.h>

namespace ra = rex::graphics::vulkan::render_area;

namespace {

VkRect2D Full() {
  VkRect2D r = {};
  r.offset.x = 0;
  r.offset.y = 0;
  r.extent.width = 1520;
  r.extent.height = 3456;
  return r;
}

VkRect2D Rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
  VkRect2D r = {};
  r.offset.x = x;
  r.offset.y = y;
  r.extent.width = w;
  r.extent.height = h;
  return r;
}

void AddScissoredDraw(ra::PassArea& area, VkRect2D scissor) {
  ra::AddDraw(area, &scissor);
}

}  // namespace

TEST_CASE("nothing added yields 1x1 at full offset", "[render_area]") {
  ra::PassArea area;
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 0);
  CHECK(result.offset.y == 0);
  CHECK(result.extent.width == 1);
  CHECK(result.extent.height == 1);
}

TEST_CASE("one draw with scissor yields that scissor", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(0, 0, 1280, 720));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 0);
  CHECK(result.offset.y == 0);
  CHECK(result.extent.width == 1280);
  CHECK(result.extent.height == 720);
}

TEST_CASE("two draws union their scissors", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(0, 0, 640, 360));
  AddScissoredDraw(area, Rect(640, 360, 640, 360));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 0);
  CHECK(result.offset.y == 0);
  CHECK(result.extent.width == 1280);
  CHECK(result.extent.height == 720);
}

TEST_CASE("only a clear rect yields that rect", "[render_area]") {
  ra::PassArea area;
  ra::AddClearRect(area, Rect(100, 100, 50, 50));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 100);
  CHECK(result.offset.y == 100);
  CHECK(result.extent.width == 50);
  CHECK(result.extent.height == 50);
}

TEST_CASE("draw with unknown scissor makes render area full", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(0, 0, 1280, 720));
  ra::AddDraw(area, nullptr);
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.extent.width == 1520);
  CHECK(result.extent.height == 3456);
}

TEST_CASE("scissor clipped to full render area", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(1400, 3400, 500, 500));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 1400);
  CHECK(result.offset.y == 3400);
  CHECK(result.extent.width == 120);
  CHECK(result.extent.height == 56);
}

TEST_CASE("scissor outside full yields 1x1 at full offset", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(2000, 4000, 10, 10));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 0);
  CHECK(result.offset.y == 0);
  CHECK(result.extent.width == 1);
  CHECK(result.extent.height == 1);
}

TEST_CASE("empty scissor alone yields 1x1 at full offset", "[render_area]") {
  ra::PassArea area;
  AddScissoredDraw(area, Rect(0, 0, 0, 720));
  VkRect2D result = ra::TightRenderArea(area, Full());
  CHECK(result.offset.x == 0);
  CHECK(result.offset.y == 0);
  CHECK(result.extent.width == 1);
  CHECK(result.extent.height == 1);
}

TEST_CASE("non-zero full offset with nothing added", "[render_area]") {
  ra::PassArea area;
  VkRect2D full = Rect(16, 32, 100, 100);
  VkRect2D result = ra::TightRenderArea(area, full);
  CHECK(result.offset.x == 16);
  CHECK(result.offset.y == 32);
  CHECK(result.extent.width == 1);
  CHECK(result.extent.height == 1);
}

TEST_CASE("UnionRect ignores empty rect in both orders", "[render_area]") {
  VkRect2D empty = {};
  VkRect2D nonEmpty = Rect(5, 6, 7, 8);

  VkRect2D r1 = ra::UnionRect(empty, nonEmpty);
  CHECK(r1.offset.x == 5);
  CHECK(r1.offset.y == 6);
  CHECK(r1.extent.width == 7);
  CHECK(r1.extent.height == 8);

  VkRect2D r2 = ra::UnionRect(nonEmpty, empty);
  CHECK(r2.offset.x == 5);
  CHECK(r2.offset.y == 6);
  CHECK(r2.extent.width == 7);
  CHECK(r2.extent.height == 8);
}
