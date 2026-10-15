/**
 * @file        graphics/util/smaa_textures.cpp
 * @brief       Precomputed lookup textures of SMAA for the swap post effect
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/util/smaa_textures.h>

// The textures from the SMAA repository (MIT), in one translation unit so both
// backends share a single copy.
#include "../../../thirdparty/smaa/AreaTex.h"
#include "../../../thirdparty/smaa/SearchTex.h"

namespace rex::graphics::smaa {

static_assert(AREATEX_WIDTH == kAreaTextureWidth && AREATEX_HEIGHT == kAreaTextureHeight &&
              AREATEX_PITCH == kAreaTexturePitch);
static_assert(sizeof(areaTexBytes) == kAreaTexturePitch * kAreaTextureHeight);
static_assert(SEARCHTEX_WIDTH == kSearchTextureWidth &&
              SEARCHTEX_HEIGHT == kSearchTextureHeight &&
              SEARCHTEX_PITCH == kSearchTexturePitch);
static_assert(sizeof(searchTexBytes) == kSearchTexturePitch * kSearchTextureHeight);

const uint8_t* GetAreaTextureData() { return areaTexBytes; }

const uint8_t* GetSearchTextureData() { return searchTexBytes; }

}  // namespace rex::graphics::smaa
