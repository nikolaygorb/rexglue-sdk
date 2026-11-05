/**
 * @file        image_decode_test.cpp
 * @brief       PNG round trip and image comparison used by GPU trace replay.
 *
 * @license     BSD 3-Clause License
 */

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/image_decode.h>

using namespace rex::ui;

TEST_CASE("PNG encoding round-trips", "[image]") {
  // 3x2 R8G8B8X8 rows with padding, as the presenter captures them.
  const uint32_t width = 3, height = 2;
  const size_t stride = 16;
  std::vector<uint8_t> rgbx(stride * height, 0x55);
  for (uint32_t i = 0; i < width * height; ++i) {
    uint8_t* pixel = rgbx.data() + stride * (i / width) + 4 * (i % width);
    pixel[0] = uint8_t(i * 40);
    pixel[1] = uint8_t(255 - i);
    pixel[2] = uint8_t(i);
    pixel[3] = 0;
  }
  const std::vector<uint8_t> rgba = PackOpaqueRGBA(rgbx.data(), width, height, stride);
  REQUIRE(rgba.size() == width * height * 4);
  CHECK(rgba[3] == 0xFF);
  CHECK(rgba[4 * 4] == 4 * 40);

  const std::vector<uint8_t> png = EncodeImagePNG(rgba.data(), width, height);
  REQUIRE_FALSE(png.empty());
  int decoded_width = 0, decoded_height = 0;
  CHECK(DecodeImageRGBA(png.data(), png.size(), decoded_width, decoded_height) == rgba);
  CHECK(decoded_width == 3);
  CHECK(decoded_height == 2);
}

TEST_CASE("Image comparison counts pixels beyond the tolerance", "[image]") {
  std::vector<uint8_t> a(4 * 4, 100), b(4 * 4, 100);
  b[0] = 102;     // pixel 0: within a tolerance of 2
  b[4 + 1] = 90;  // pixel 1: beyond it
  b[8 + 3] = 0;   // pixel 2: alpha only, ignored
  std::vector<uint8_t> diff;
  CHECK(CompareImagesRGBA(a.data(), b.data(), 2, 2, 2, &diff) == 1);
  CHECK(diff[4] == 0xFF);
  CHECK(diff[0] == 25);
  CHECK(CompareImagesRGBA(a.data(), b.data(), 2, 2, 1) == 2);
  CHECK(CompareImagesRGBA(a.data(), a.data(), 2, 2, 0) == 0);
}
