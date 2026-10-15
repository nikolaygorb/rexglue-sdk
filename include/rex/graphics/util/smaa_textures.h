#pragma once
/**
 * @file        rex/graphics/util/smaa_textures.h
 * @brief       Precomputed lookup textures of SMAA for the swap post effect
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::graphics::smaa {

// AreaTex: R8G8_UNORM, rows of kAreaTexturePitch bytes, top row first.
constexpr uint32_t kAreaTextureWidth = 160;
constexpr uint32_t kAreaTextureHeight = 560;
constexpr uint32_t kAreaTexturePitch = kAreaTextureWidth * 2;
// SearchTex: R8_UNORM, rows of kSearchTexturePitch bytes, top row first.
constexpr uint32_t kSearchTextureWidth = 64;
constexpr uint32_t kSearchTextureHeight = 16;
constexpr uint32_t kSearchTexturePitch = kSearchTextureWidth;

// kAreaTexturePitch * kAreaTextureHeight bytes.
const uint8_t* GetAreaTextureData();
// kSearchTexturePitch * kSearchTextureHeight bytes.
const uint8_t* GetSearchTextureData();

}  // namespace rex::graphics::smaa
