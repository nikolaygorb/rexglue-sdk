/**
 * @file        fuzz/resolve_fuzzer.cpp
 * @brief       libFuzzer target for the resolve (EDRAM to memory copy) setup
 *              taken from the guest's registers
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
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/address.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>

// Defined with the GPU core (graphics/command_processor.cpp), which isn't
// compiled into the fuzzer; read by draw_util. Same default.
REXCVAR_DEFINE_BOOL(occlusion_query_viz, false, "GPU", "Fuzzer copy of the plugin cvar");

using namespace rex::graphics;

// Input layout:
//   0: bits 0..1  - draw resolution scale X - 1 (1 to 3);
//      bits 2..3  - draw resolution scale Y - 1 (1 to 3);
//      bit 4      - extended EDRAM tile count (edram_tile_count = extended);
//      bit 5, 6   - k_16_16 / k_16_16_16_16 render targets emulated as snorm;
//   1: bits 0..1  - endianness of the vertex fetch constant 0 (XOR k8in32);
//   2..25         - the resolve rectangle: 3 vertices of 2 floats, as in guest
//                   memory (XOR a 1280x720 rectangle);
//   then          - a dword for each register in kRegisters (XOR the value
//                   in it), missing ones are 0.
// The XORed values are those of a 1280x720 k_8_8_8_8 color resolve with the
// Direct3D 9 defaults, so an all-zero input is a resolve that does something.
//
// draw_util::GetResolveInfo runs on the registers, as the render target caches
// call it, then the copy and clear shader setup. Checked on top of ASan: for a
// destination inside the shared memory buffer, every texel the copy shader
// writes (resolve.xesli XeResolveDestPixelAddress) lies inside
// [copy_dest_extent_start, + copy_dest_extent_length), the range the render
// target caches request and mark as written. An extent beyond the shared
// memory buffer is not checked (the caches refuse it), and addresses wrap
// around at 32 bits like in the shaders. texture_util is built with traps on
// integer overflow (tests/fuzz/CMakeLists.txt).

namespace {

constexpr uint32_t kSharedMemorySize = UINT32_C(512) << 20;

struct RegisterDefault {
  uint32_t index;
  uint32_t value;
};

std::vector<RegisterDefault> MakeRegisters() {
  reg::RB_SURFACE_INFO surface_info = {};
  surface_info.surface_pitch = 1280;
  reg::RB_COPY_DEST_INFO copy_dest_info = {};
  copy_dest_info.copy_dest_endian = xenos::Endian128::k8in32;
  copy_dest_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
  reg::RB_COPY_DEST_PITCH copy_dest_pitch = {};
  copy_dest_pitch.copy_dest_pitch = 1280;
  copy_dest_pitch.copy_dest_height = 720;
  reg::PA_SC_WINDOW_SCISSOR_BR window_scissor_br = {};
  window_scissor_br.br_x = 1280;
  window_scissor_br.br_y = 720;
  reg::PA_SC_SCREEN_SCISSOR_BR screen_scissor_br = {};
  screen_scissor_br.br_x = 8192;
  screen_scissor_br.br_y = 8192;
  return {
      {reg::RB_COPY_CONTROL::register_index, 0},
      {reg::RB_COPY_DEST_INFO::register_index, copy_dest_info.value},
      {reg::RB_COPY_DEST_PITCH::register_index, copy_dest_pitch.value},
      {XE_GPU_REG_RB_COPY_DEST_BASE, 0x00100000},
      {XE_GPU_REG_RB_COPY_SURFACE_SLICE, 0},
      {reg::RB_SURFACE_INFO::register_index, surface_info.value},
      {reg::RB_DEPTH_INFO::register_index, 0},
      {uint32_t(reg::RB_COLOR_INFO::rt_register_indices[0]), 0},
      {uint32_t(reg::RB_COLOR_INFO::rt_register_indices[1]), 0},
      {uint32_t(reg::RB_COLOR_INFO::rt_register_indices[2]), 0},
      {uint32_t(reg::RB_COLOR_INFO::rt_register_indices[3]), 0},
      {reg::PA_SU_VTX_CNTL::register_index, 0},
      {reg::PA_SU_SC_MODE_CNTL::register_index, 0},
      {reg::PA_SC_WINDOW_OFFSET::register_index, 0},
      {reg::PA_SC_WINDOW_SCISSOR_TL::register_index, 0},
      {reg::PA_SC_WINDOW_SCISSOR_BR::register_index, window_scissor_br.value},
      {reg::PA_SC_SCREEN_SCISSOR_TL::register_index, 0},
      {reg::PA_SC_SCREEN_SCISSOR_BR::register_index, screen_scissor_br.value},
      {XE_GPU_REG_RB_COPY_FUNC, 0},
      {XE_GPU_REG_RB_COPY_REF, 0},
      {XE_GPU_REG_RB_COPY_MASK, 0},
      {XE_GPU_REG_RB_DEPTH_CLEAR, 0},
      {XE_GPU_REG_RB_COLOR_CLEAR, 0},
      {XE_GPU_REG_RB_COLOR_CLEAR_LO, 0},
  };
}

// (0, 0), (1280, 0), (1280, 720) as big-endian floats.
constexpr uint8_t kDefaultVertices[24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0xA0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x44, 0xA0, 0x00, 0x00, 0x44, 0x34, 0x00, 0x00,
};

struct Guest {
  rex::memory::Memory memory;
  uint32_t vertices_physical = 0;
  uint8_t* vertices = nullptr;
};

Guest* GetGuest() {
  static Guest* guest = [] {
    auto* guest = new Guest;
    if (!guest->memory.Initialize()) {
      std::fprintf(stderr, "resolve_fuzzer: can't reserve the guest address space\n");
      std::abort();
    }
    uint32_t address =
        guest->memory.SystemHeapAlloc(4096, 4096, rex::memory::kSystemHeapPhysical);
    guest->vertices_physical = guest->memory.GetPhysicalAddress(address);
    guest->vertices = guest->memory.TranslatePhysical(guest->vertices_physical);
    return guest;
  }();
  return guest;
}

[[noreturn]] void Fail(const char* what, const draw_util::ResolveInfo& info) {
  std::fprintf(stderr,
               "resolve_fuzzer: %s\n"
               "  %u x %u pixels, dest base 0x%08X, extent 0x%08X + 0x%X, dest info 0x%08X,\n"
               "  dest coordinate info 0x%08X\n",
               what, uint32_t(info.coordinate_info.width_div_8) << 3, info.height_div_8 << 3,
               info.copy_dest_base, info.copy_dest_extent_start, info.copy_dest_extent_length,
               info.copy_dest_info.value, info.copy_dest_coordinate_info.packed);
  std::abort();
}

// Coordinates to check along one axis: all of them when there are few,
// otherwise the first tile and the last two, where the largest addresses are.
void AxisSamples(uint32_t count, bool all, std::vector<uint32_t>& out) {
  constexpr uint32_t kTile = xenos::kTextureTileWidthHeight;
  out.clear();
  if (all || count <= 3 * kTile) {
    for (uint32_t i = 0; i < count; ++i) {
      out.push_back(i);
    }
    return;
  }
  for (uint32_t i = 0; i < kTile; ++i) {
    out.push_back(i);
  }
  for (uint32_t i = ((count - 1) & ~(kTile - 1)) - kTile; i < count; ++i) {
    out.push_back(i);
  }
}

// The copy shader's destination addresses (unscaled, one texel per pixel; the
// k_DXT3A_AS_1_1_1_1 conversion writes 4x4 blocks and is left out).
void CheckCopyDestination(const draw_util::ResolveInfo& info) {
  uint64_t extent_end = uint64_t(info.copy_dest_extent_start) + info.copy_dest_extent_length;
  if (!info.copy_dest_extent_length || extent_end > kSharedMemorySize) {
    // Nothing copied, or the render target caches refuse the range.
    return;
  }
  const FormatInfo* format_info =
      FormatInfo::Get(xenos::TextureFormat(info.copy_dest_info.copy_dest_format));
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  uint32_t pitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 << 5;
  uint32_t height = info.copy_dest_coordinate_info.height_aligned_div_32 << 5;
  uint32_t offset_x = info.copy_dest_coordinate_info.offset_x_div_8 << 3;
  uint32_t offset_y = info.copy_dest_coordinate_info.offset_y_div_8 << 3;
  uint32_t width_pixels = uint32_t(info.coordinate_info.width_div_8) << 3;
  uint32_t height_pixels = info.height_div_8 << 3;
  bool is_array = info.copy_dest_info.copy_dest_array != 0;
  uint32_t slice = info.copy_dest_info.copy_dest_slice;
  auto check = [&](uint32_t x, uint32_t y) {
    int64_t tiled =
        is_array ? texture_address::Tiled3D(int32_t(offset_x + x), int32_t(offset_y + y),
                                            int32_t(slice), pitch, height, bytes_per_block_log2)
                 : texture_address::Tiled2D(int32_t(offset_x + x), int32_t(offset_y + y), pitch,
                                            bytes_per_block_log2);
    // 32-bit, wrapping around, like the shader's uint arithmetic.
    uint32_t address = info.copy_dest_base + uint32_t(tiled);
    if (address < info.copy_dest_extent_start || uint64_t(address) + bytes_per_block > extent_end) {
      std::fprintf(stderr, "resolve_fuzzer: pixel (%u, %u) written at 0x%08X\n", x, y, address);
      Fail("the copy writes outside the destination extent", info);
    }
  };
  bool all = uint64_t(width_pixels) * height_pixels <= (uint64_t(1) << 16);
  static std::vector<uint32_t> x_edges, y_edges;
  AxisSamples(width_pixels, all, x_edges);
  AxisSamples(height_pixels, all, y_edges);
  for (uint32_t y : y_edges) {
    for (uint32_t x = 0; x < width_pixels; ++x) {
      check(x, y);
    }
  }
  if (!all) {
    for (uint32_t y = 0; y < height_pixels; ++y) {
      for (uint32_t x : x_edges) {
        check(x, y);
      }
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr size_t kHeaderSize = 2 + sizeof(kDefaultVertices);
  if (size < kHeaderSize) {
    return 0;
  }
  static const std::vector<RegisterDefault> registers = MakeRegisters();
  Guest* guest = GetGuest();
  static auto regs = std::make_unique<RegisterFile>();

  uint32_t draw_resolution_scale_x = (data[0] & 3) % 3 + 1;
  uint32_t draw_resolution_scale_y = ((data[0] >> 2) & 3) % 3 + 1;
  uint32_t edram_tile_count =
      (data[0] & (1 << 4)) ? xenos::kEdramTileCountMax : xenos::kEdramTileCount;
  bool fixed_rg16_truncated = (data[0] & (1 << 5)) != 0;
  bool fixed_rgba16_truncated = (data[0] & (1 << 6)) != 0;

  std::memset(regs->values, 0, sizeof(regs->values));
  xenos::xe_gpu_vertex_fetch_t vertex_fetch = {};
  vertex_fetch.type = xenos::FetchConstantType::kVertex;
  vertex_fetch.address = guest->vertices_physical >> 2;
  vertex_fetch.endian = xenos::Endian((data[1] & 3) ^ uint32_t(xenos::Endian::k8in32));
  vertex_fetch.size = 3 * 2;
  (*regs)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = vertex_fetch.dword_0;
  (*regs)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1] = vertex_fetch.dword_1;
  for (size_t i = 0; i < sizeof(kDefaultVertices); ++i) {
    guest->vertices[i] = data[2 + i] ^ kDefaultVertices[i];
  }
  data += kHeaderSize;
  size -= kHeaderSize;
  for (const RegisterDefault& reg : registers) {
    uint32_t value = 0;
    std::memcpy(&value, data, std::min(size, sizeof(value)));
    size_t used = std::min(size, sizeof(value));
    data += used;
    size -= used;
    (*regs)[reg.index] = value ^ reg.value;
  }

  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(*regs, guest->memory, draw_resolution_scale_x,
                                 draw_resolution_scale_y, edram_tile_count, fixed_rg16_truncated,
                                 fixed_rgba16_truncated, info)) {
    return 0;
  }
  if (!info.coordinate_info.width_div_8 || !info.height_div_8) {
    return 0;
  }
  // What the render target caches do with the info next.
  uint32_t span_base, span_row_length, span_rows, span_pitch;
  info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows, span_pitch);
  if (info.copy_dest_extent_length) {
    draw_util::ResolveCopyShaderConstants copy_constants;
    uint32_t group_count_x, group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader = info.GetCopyShader(
        draw_resolution_scale_x, draw_resolution_scale_y, copy_constants, group_count_x,
        group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown &&
        info.rb_copy_control.copy_command != xenos::CopyCommand::kConvertTo1111) {
      CheckCopyDestination(info);
    }
  }
  draw_util::ResolveClearShaderConstants clear_constants;
  if (info.IsClearingDepth()) {
    info.GetDepthClearShaderConstants(clear_constants);
    info.GetClearShaderGroupCount(draw_resolution_scale_x, draw_resolution_scale_y);
  }
  if (info.IsClearingColor()) {
    info.GetColorClearShaderConstants(clear_constants);
    info.GetClearShaderGroupCount(draw_resolution_scale_x, draw_resolution_scale_y);
  }
  return 0;
}
