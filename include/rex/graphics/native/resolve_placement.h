/**
 * @file        graphics/native/resolve_placement.h
 * @brief       Native renderer: where a resolve's pixels are within a texture the game samples
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The destination origin as the SDK's draw_util::GetResolveInfo derives it (Xenia,
 *              Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::native {

// The GPU writes a resolve's destination pixel (x, y) (the source rectangle's
// own coordinates) at RB_COPY_DEST_BASE + the 2D tiled address of (x, y).
// Direct3D moves RB_COPY_DEST_BASE by whole macro tiles to resolve into a
// part of a texture; for 8 and 16 bits per pixel a macro tile is less than a
// subresource's 4 KB alignment, and the part of the move below it is in the
// base's low bits: GetResolveInfo takes it out into x. The result is the
// origin of a texture holding the destination, aligned like any texture base,
// and how far right of it pixel x = 0 is.
struct ResolveDestOrigin {
  uint32_t texture_base = 0;
  uint32_t x_offset = 0;
};
ResolveDestOrigin GetResolveDestOrigin(uint32_t copy_dest_base, uint32_t bytes_per_pixel_log2);

// The macro-tile-aligned texel (x, y) of a 2D tiled texture at texture_base
// (pitch_aligned and height_aligned in blocks, multiples of 32) whose tiled
// address is dest_texture_base: where a resolve's destination origin is in
// that texture. False if there is none (before the texture, past its end, or
// not on a macro tile).
bool FindMacroTileInTexture(uint32_t texture_base, uint32_t dest_texture_base,
                            uint32_t pitch_aligned, uint32_t height_aligned,
                            uint32_t bytes_per_pixel_log2, uint32_t& x_out, uint32_t& y_out);

}  // namespace rex::graphics::native
