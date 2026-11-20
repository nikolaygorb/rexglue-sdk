/**
 * @file        graphics/native/resolve_placement.cpp
 * @brief       Native renderer: where a resolve's pixels are within a texture the game samples
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The destination origin as the SDK's draw_util::GetResolveInfo derives it (Xenia,
 *              Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/resolve_placement.h>

#include <rex/graphics/pipeline/texture/address.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

ResolveDestOrigin GetResolveDestOrigin(uint32_t copy_dest_base, uint32_t bytes_per_pixel_log2) {
  // As GetResolveInfo for 2D destinations: the macro tile phase within the
  // subresource alignment, moved into x.
  uint32_t macro_tile_bytes_log2 = 2 * xenos::kTextureTileWidthHeightLog2 + bytes_per_pixel_log2;
  uint32_t phase =
      (copy_dest_base & (xenos::kTextureSubresourceAlignmentBytes - 1)) >> macro_tile_bytes_log2;
  ResolveDestOrigin origin;
  origin.texture_base = copy_dest_base - (phase << macro_tile_bytes_log2);
  origin.x_offset = phase << xenos::kTextureTileWidthHeightLog2;
  return origin;
}

bool FindMacroTileInTexture(uint32_t texture_base, uint32_t dest_texture_base,
                            uint32_t pitch_aligned, uint32_t height_aligned,
                            uint32_t bytes_per_pixel_log2, uint32_t& x_out, uint32_t& y_out) {
  x_out = 0;
  y_out = 0;
  if (dest_texture_base < texture_base) {
    return false;
  }
  uint32_t offset = dest_texture_base - texture_base;
  if (!offset) {
    return true;
  }
  uint32_t tiles_x = pitch_aligned >> texture_address::kMacroTileWidthLog2;
  uint32_t tiles_y = height_aligned >> texture_address::kMacroTileHeight2DLog2;
  if (!tiles_x || !tiles_y) {
    return false;
  }
  // Macro tiles in storage order: the tiled address grows with the tile index
  // (y / 32 rows of pitch / 32 tiles, then x / 32), so a binary search.
  auto tile_address = [&](uint32_t tile) {
    return uint32_t(texture_address::Tiled2D(
        int32_t((tile % tiles_x) << texture_address::kMacroTileWidthLog2),
        int32_t((tile / tiles_x) << texture_address::kMacroTileHeight2DLog2), pitch_aligned,
        bytes_per_pixel_log2));
  };
  uint32_t low = 0;
  uint32_t high = tiles_x * tiles_y;
  while (low < high) {
    uint32_t middle = low + (high - low) / 2;
    if (tile_address(middle) < offset) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low >= tiles_x * tiles_y || tile_address(low) != offset) {
    return false;
  }
  x_out = (low % tiles_x) << texture_address::kMacroTileWidthLog2;
  y_out = (low / tiles_x) << texture_address::kMacroTileHeight2DLog2;
  return true;
}

}  // namespace rex::graphics::native
