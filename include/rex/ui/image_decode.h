/**
 * @file        rex/ui/image_decode.h
 * @brief       Minimal host-side PNG decode helper (wraps stb_image).
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <cstdint>
#include <vector>

namespace rex::ui {

// Decodes a PNG (or any stb-supported image) byte buffer to tightly-packed
// R8G8B8A8 pixels. Returns an empty vector on failure; on success fills
// out_width/out_height and returns width*height*4 bytes.
std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& out_width,
                                     int& out_height);

// Encodes tightly-packed R8G8B8A8 pixels as PNG. Empty on failure.
std::vector<uint8_t> EncodeImagePNG(const uint8_t* rgba, uint32_t width, uint32_t height);

// Tightly packs rows of R8G8B8X8 pixels `stride` bytes apart, with alpha 255.
std::vector<uint8_t> PackOpaqueRGBA(const uint8_t* rgbx, uint32_t width, uint32_t height,
                                    size_t stride);

// Counts the pixels of two tightly-packed R8G8B8A8 images whose red, green or
// blue differ by more than `tolerance`. If diff_out is given, fills it with an
// image of the same size, white where pixels differ and a dim copy of `a`
// elsewhere.
uint64_t CompareImagesRGBA(const uint8_t* a, const uint8_t* b, uint32_t width, uint32_t height,
                           uint32_t tolerance, std::vector<uint8_t>* diff_out = nullptr);

}  // namespace rex::ui
