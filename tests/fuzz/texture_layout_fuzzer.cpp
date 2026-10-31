/**
 * @file        fuzz/texture_layout_fuzzer.cpp
 * @brief       libFuzzer target for the guest texture layout taken from a
 *              texture fetch constant
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/address.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>

using namespace rex::graphics;

// Input layout: the 6 dwords of a texture fetch constant (host byte order, as
// in the register file), then guest memory for the texture's base level (what
// texture replacement hashes; zeros past the end of the input).
//
// The fetch constant goes the way the texture cache takes it: the key
// (TextureCache::BindingInfoFromFetchConstant), the guest layout (the Texture
// constructor) and, for textures a replacement pack may stand for, the
// replacement key (TextureCache::ComputeReplacementKey). Checked on top of
// ASan:
// - the key keeps every value whole (its bit fields are narrower than the
//   fetch constant's);
// - every block the upload shaders read lies inside the level's
//   array_slice_data_extent_bytes, the memory the texture cache requests and
//   watches (the shaders get the strides from the layout and use the tiled
//   address functions of texture_address);
// - mips lie inside mips_total_extent_bytes;
// - the replacement key of the base level can be computed from the memory the
//   layout says the base level occupies.
// util.cpp is built with traps on integer overflow (tests/fuzz/CMakeLists.txt).

namespace {

// TextureCache::TextureKey's fields.
struct Key {
  xenos::DataDimension dimension;
  uint32_t width;
  uint32_t height;
  uint32_t depth_or_array_size;
  uint32_t pitch;
  uint32_t base_page;
  uint32_t mip_page;
  uint32_t mip_max_level;
  bool tiled;
  bool packed_mips;
  xenos::TextureFormat format;
  xenos::Endian endianness;
};

[[noreturn]] void Fail(const char* what, const Key& key) {
  std::fprintf(stderr,
               "texture_layout_fuzzer: %s\n"
               "  dimension %u, %ux%ux%u, pitch %u texels, format %u, tiled %u, packed mips %u,\n"
               "  base page 0x%X, mip page 0x%X, mip max level %u\n",
               what, uint32_t(key.dimension), key.width, key.height, key.depth_or_array_size,
               key.pitch << 5, uint32_t(key.format), uint32_t(key.tiled),
               uint32_t(key.packed_mips), key.base_page, key.mip_page, key.mip_max_level);
  std::abort();
}

// TextureCache::BindingInfoFromFetchConstant, without the logging, with the
// default gpu_allow_invalid_fetch_constants (off).
bool KeyFromFetchConstant(const xenos::xe_gpu_texture_fetch_t& fetch, Key& key) {
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return false;
  }
  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    return false;
  }
  uint32_t pitch = fetch.pitch;
  if (fetch.dimension == xenos::DataDimension::k1D) {
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      uint32_t total_width = width_minus_1 + 1;
      uint32_t row_width = xenos::kTexture2DCubeMaxWidthHeight;
      uint32_t num_rows = std::min((total_width + row_width - 1) / row_width,
                                   xenos::kTexture1DWideMaxRows);
      width_minus_1 = row_width - 1;
      height_minus_1 = num_rows - 1;
      mip_max_level = 0;
      pitch = xenos::kTexture2DCubeMaxWidthHeight >> 5;
    }
    if (fetch.tiled || fetch.packed_mips) {
      return false;
    }
  }
  key.dimension = fetch.dimension;
  key.width = width_minus_1 + 1;
  key.height = height_minus_1 + 1;
  key.depth_or_array_size = depth_or_array_size_minus_1 + 1;
  key.pitch = pitch;
  key.base_page = base_page;
  key.mip_page = mip_page;
  key.mip_max_level = mip_max_level;
  key.tiled = fetch.tiled;
  key.packed_mips = fetch.packed_mips;
  key.format = GetBaseFormat(fetch.format);
  key.endianness = fetch.endianness;
  // The widths of TextureKey's bit fields.
  if (width_minus_1 >= (1u << 13) || height_minus_1 >= (1u << 13) ||
      depth_or_array_size_minus_1 >= (1u << 10) || pitch >= (1u << 9) ||
      base_page >= (1u << 17) || mip_page >= (1u << 17) || mip_max_level >= (1u << 4)) {
    Fail("the texture key can't hold a value of the fetch constant", key);
  }
  return true;
}

// The upload shaders' guest address of a block, relative to the array slice.
int64_t BlockAddress(const Key& key, const texture_util::TextureGuestLayout::Level& level,
                     uint32_t bytes_per_block, uint32_t x, uint32_t y, uint32_t z) {
  uint32_t pitch_blocks = level.row_pitch_bytes / bytes_per_block;
  uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  if (!key.tiled) {
    return (int64_t(z) * level.z_slice_stride_block_rows + y) * level.row_pitch_bytes +
           int64_t(x) * bytes_per_block;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    return texture_address::Tiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks,
                                    level.z_slice_stride_block_rows, bytes_per_block_log2);
  }
  return texture_address::Tiled2D(int32_t(x), int32_t(y), pitch_blocks, bytes_per_block_log2);
}

// Coordinates to check along one axis: all of them when there are few,
// otherwise the first tile and the last two, where the largest addresses are.
void AxisSamples(uint32_t count, uint32_t tile, bool all, std::vector<uint32_t>& out) {
  out.clear();
  if (all || count <= 3 * tile) {
    for (uint32_t i = 0; i < count; ++i) {
      out.push_back(i);
    }
    return;
  }
  for (uint32_t i = 0; i < tile; ++i) {
    out.push_back(i);
  }
  for (uint32_t i = ((count - 1) & ~(tile - 1)) - tile; i < count; ++i) {
    out.push_back(i);
  }
}

void CheckLevelReads(const Key& key, const texture_util::TextureGuestLayout::Level& level,
                     const char* level_name) {
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t bytes_per_block = format_info->bytes_per_block();
  if (!level.x_extent_blocks || !level.y_extent_blocks || !level.z_extent) {
    Fail("a stored level is empty", key);
  }
  if (level.row_pitch_bytes % bytes_per_block) {
    Fail("the row pitch is not a whole number of blocks", key);
  }
  auto check = [&](uint32_t x, uint32_t y, uint32_t z) {
    int64_t address = BlockAddress(key, level, bytes_per_block, x, y, z);
    if (address < 0 || address + bytes_per_block > int64_t(level.array_slice_data_extent_bytes)) {
      std::fprintf(stderr, "texture_layout_fuzzer: %s block (%u, %u, %u) at 0x%llX, extent 0x%X\n",
                   level_name, x, y, z, (long long)address, level.array_slice_data_extent_bytes);
      Fail("an upload shader reads outside the level's data extent", key);
    }
  };
  // Every block of small levels; of large ones, the rows and the columns of
  // blocks at the edges (all of each).
  uint64_t blocks = uint64_t(level.x_extent_blocks) * level.y_extent_blocks * level.z_extent;
  bool all = blocks <= (uint64_t(1) << 16);
  static std::vector<uint32_t> x_edges, y_edges, zs;
  AxisSamples(level.x_extent_blocks, xenos::kTextureTileWidthHeight, all, x_edges);
  AxisSamples(level.y_extent_blocks, xenos::kTextureTileWidthHeight, all, y_edges);
  AxisSamples(level.z_extent, xenos::kTextureTileDepth, all, zs);
  for (uint32_t z : zs) {
    for (uint32_t y : y_edges) {
      for (uint32_t x = 0; x < level.x_extent_blocks; ++x) {
        check(x, y, z);
      }
    }
    if (!all) {
      for (uint32_t y = 0; y < level.y_extent_blocks; ++y) {
        for (uint32_t x : x_edges) {
          check(x, y, z);
        }
      }
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  xenos::xe_gpu_texture_fetch_t fetch;
  if (size < sizeof(fetch)) {
    return 0;
  }
  std::memcpy(&fetch, data, sizeof(fetch));
  data += sizeof(fetch);
  size -= sizeof(fetch);

  // Also done for every binding.
  texture_util::SwizzleSigns(fetch);
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);

  Key key;
  if (!KeyFromFetchConstant(fetch, key)) {
    return 0;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t bytes_per_block = format_info->bytes_per_block();
  // k_1 and k_1_REVERSE: no backend has a host format, so no texture (and no
  // layout) is created for them.
  if (!bytes_per_block) {
    return 0;
  }

  // TextureKey::GetGuestLayout.
  bool has_base = key.base_page != 0;
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      key.format, key.packed_mips, has_base, key.mip_max_level);
  // Larger than the guest's memory: nothing is loaded.
  if (!layout.array_size) {
    return 0;
  }

  // The tiled address functions take log2 of the block size, so a tiled
  // k_32_32_32_FLOAT (12 bytes per block, never tiled by Direct3D 9) is
  // addressed as 8 bytes per block.
  bool reads_checked = !key.tiled || rex::is_pow2(bytes_per_block);
  if (reads_checked && has_base) {
    CheckLevelReads(key, layout.base, "base");
  }
  if (layout.max_level) {
    // The stored mips: 1...min(max_level, packed_level), or the packed tail as
    // level 0.
    uint32_t level_first = layout.packed_level == 0 ? 0 : 1;
    uint32_t level_last = std::min(layout.max_level, layout.packed_level);
    for (uint32_t level = level_first; level <= level_last; ++level) {
      const texture_util::TextureGuestLayout::Level& mip = layout.mips[level];
      if (reads_checked) {
        CheckLevelReads(key, mip, "mip");
      }
      if (uint64_t(layout.mip_offsets_bytes[level]) + mip.level_data_extent_bytes >
          layout.mips_total_extent_bytes) {
        Fail("a mip lies outside mips_total_extent_bytes", key);
      }
    }
  }

  // TextureCache::IsReplacementCandidate and ComputeReplacementKey. Only for a
  // pitch of at least the width: the ripper (and ComputeKey) take a smaller
  // pitch as the width, so it reads past what the guest's own layout covers.
  if (key.dimension == xenos::DataDimension::k2DOrStacked && key.depth_or_array_size == 1 &&
      has_base && texture_replacement::IsFormatSupported(key.format) &&
      (key.pitch << 5) >= key.width) {
    uint32_t base_size = layout.base.level_data_extent_bytes;
    // Bounded to keep runs fast; the largest textures are left to the layout
    // checks above.
    if (base_size && base_size <= (UINT32_C(16) << 20)) {
      std::vector<uint8_t> guest(base_size);
      std::memcpy(guest.data(), data, std::min(size, guest.size()));
      texture_replacement::GuestBaseLevel level;
      level.data = guest.data();
      level.data_size = base_size;
      level.width = key.width;
      level.height = key.height;
      level.pitch_texels = key.pitch << 5;
      level.tiled = key.tiled;
      level.format = key.format;
      level.endianness = key.endianness;
      if (key.packed_mips) {
        uint32_t z_offset;
        texture_util::GetPackedMipOffset(level.width, level.height, 1, key.format, 0,
                                         level.x_offset_blocks, level.y_offset_blocks, z_offset);
      }
      static std::vector<uint8_t> scratch;
      uint64_t replacement_key;
      if (!texture_replacement::ComputeKey(level, scratch, replacement_key)) {
        Fail("the replacement key reads outside the base level's data extent", key);
      }
    }
  }
  return 0;
}
