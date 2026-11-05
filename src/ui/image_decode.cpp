/**
 * @file        ui/image_decode.cpp
 * @brief       PNG decode via stb_image. See image_decode.h.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/image_decode.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cstdlib>
#include <cstring>

namespace rex::ui {

std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& out_width,
                                     int& out_height) {
  out_width = 0;
  out_height = 0;
  if (!data || size == 0) {
    return {};
  }
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &out_width, &out_height,
                                          &channels, 4 /* force RGBA */);
  if (!pixels) {
    out_width = 0;
    out_height = 0;
    return {};
  }
  const size_t byte_count = static_cast<size_t>(out_width) * out_height * 4;
  std::vector<uint8_t> result(pixels, pixels + byte_count);
  stbi_image_free(pixels);
  return result;
}

std::vector<uint8_t> EncodeImagePNG(const uint8_t* rgba, uint32_t width, uint32_t height) {
  std::vector<uint8_t> result;
  if (!rgba || !width || !height) {
    return result;
  }
  stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        auto& out = *static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
      },
      &result, int(width), int(height), 4, rgba, int(width * 4));
  return result;
}

std::vector<uint8_t> PackOpaqueRGBA(const uint8_t* rgbx, uint32_t width, uint32_t height,
                                    size_t stride) {
  std::vector<uint8_t> result(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = result.data() + size_t(width) * 4 * y;
    std::memcpy(row, rgbx + stride * y, size_t(width) * 4);
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 3] = 0xFF;
    }
  }
  return result;
}

uint64_t CompareImagesRGBA(const uint8_t* a, const uint8_t* b, uint32_t width, uint32_t height,
                           uint32_t tolerance, std::vector<uint8_t>* diff_out) {
  const size_t pixel_count = size_t(width) * height;
  if (diff_out) {
    diff_out->resize(pixel_count * 4);
  }
  uint64_t different = 0;
  for (size_t i = 0; i < pixel_count; ++i) {
    const uint8_t* pa = a + i * 4;
    const uint8_t* pb = b + i * 4;
    bool differs = false;
    for (int c = 0; c < 3; ++c) {
      differs |= uint32_t(std::abs(int(pa[c]) - int(pb[c]))) > tolerance;
    }
    different += differs;
    if (diff_out) {
      uint8_t* pd = diff_out->data() + i * 4;
      for (int c = 0; c < 3; ++c) {
        pd[c] = differs ? 0xFF : uint8_t(pa[c] / 4);
      }
      pd[3] = 0xFF;
    }
  }
  return different;
}

}  // namespace rex::ui
