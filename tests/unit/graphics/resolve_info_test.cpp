/**
 * @file        graphics/resolve_info_test.cpp
 * @brief       Tests for the resolve setup taken from the guest's registers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

// Defined with the GPU core (graphics/command_processor.cpp), which unit_tests doesn't compile;
// read by draw_util. Same default.
REXCVAR_DEFINE_BOOL(occlusion_query_viz, false, "GPU", "Test copy of the plugin cvar");

using namespace rex::graphics;

namespace {

// A 1280x720 k_8_8_8_8 color resolve into a slice of a 3D texture whose slices are
// RB_COPY_SURFACE_SLICE / dest_pitch rows tall.
draw_util::ResolveInfo ResolveToVolume(uint32_t surface_slice, uint32_t dest_pitch = 1280,
                                       uint32_t dest_slice = 0) {
  rex::memory::Memory& memory = rex::testing::GetTestMemory();
  static const uint32_t vertices_physical = [&memory] {
    uint32_t address = memory.SystemHeapAlloc(4096, 4096, rex::memory::kSystemHeapPhysical);
    return memory.GetPhysicalAddress(address);
  }();
  // (0, 0), (1280, 0), (1280, 720) as big-endian floats.
  const uint32_t vertices[6] = {0, 0, 0x0000A044, 0, 0x0000A044, 0x00003444};
  std::memcpy(memory.TranslatePhysical(vertices_physical), vertices, sizeof(vertices));

  auto regs = std::make_unique<RegisterFile>();
  std::memset(regs->values, 0, sizeof(regs->values));
  xenos::xe_gpu_vertex_fetch_t vertex_fetch = {};
  vertex_fetch.type = xenos::FetchConstantType::kVertex;
  vertex_fetch.address = vertices_physical >> 2;
  vertex_fetch.endian = xenos::Endian::k8in32;
  vertex_fetch.size = 3 * 2;
  (*regs)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = vertex_fetch.dword_0;
  (*regs)[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1] = vertex_fetch.dword_1;

  reg::RB_SURFACE_INFO surface_info = {};
  surface_info.surface_pitch = 1280;
  (*regs)[reg::RB_SURFACE_INFO::register_index] = surface_info.value;
  reg::PA_SC_WINDOW_SCISSOR_BR window_scissor_br = {};
  window_scissor_br.br_x = 1280;
  window_scissor_br.br_y = 720;
  (*regs)[reg::PA_SC_WINDOW_SCISSOR_BR::register_index] = window_scissor_br.value;
  reg::PA_SC_SCREEN_SCISSOR_BR screen_scissor_br = {};
  screen_scissor_br.br_x = 8192;
  screen_scissor_br.br_y = 8192;
  (*regs)[reg::PA_SC_SCREEN_SCISSOR_BR::register_index] = screen_scissor_br.value;
  reg::RB_COPY_DEST_INFO copy_dest_info = {};
  copy_dest_info.copy_dest_endian = xenos::Endian128::k8in32;
  copy_dest_info.copy_dest_array = 1;
  copy_dest_info.copy_dest_slice = dest_slice;
  copy_dest_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
  (*regs)[reg::RB_COPY_DEST_INFO::register_index] = copy_dest_info.value;
  reg::RB_COPY_DEST_PITCH copy_dest_pitch = {};
  copy_dest_pitch.copy_dest_pitch = dest_pitch;
  copy_dest_pitch.copy_dest_height = 720;
  (*regs)[reg::RB_COPY_DEST_PITCH::register_index] = copy_dest_pitch.value;
  (*regs)[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x00100000;
  (*regs)[XE_GPU_REG_RB_COPY_SURFACE_SLICE] = surface_slice;

  draw_util::ResolveInfo info;
  REQUIRE(draw_util::GetResolveInfo(*regs, memory, 1, 1, xenos::kEdramTileCount, false, false,
                                    info));
  REQUIRE(info.coordinate_info.width_div_8 == 1280 / 8);
  REQUIRE(info.height_div_8 == 720 / 8);
  return info;
}

}  // namespace

TEST_CASE("Resolve to a volume takes the slice height from RB_COPY_SURFACE_SLICE",
          "[graphics][resolve]") {
  draw_util::ResolveInfo info = ResolveToVolume(1280 * 720);
  CHECK(info.copy_dest_coordinate_info.height_aligned_div_32 == 736 / 32);
  CHECK(info.copy_dest_extent_length != 0);
}

// Found by resolve_fuzzer: a slice height beyond what the shaders' 10-bit field holds (32736
// rows) was cut there, so the copy wrote outside the destination extent the render target
// caches request and mark as written.
TEST_CASE("Resolve to a volume with slices taller than the shaders take is dropped",
          "[graphics][resolve]") {
  draw_util::ResolveInfo info = ResolveToVolume(1280 * 40000);
  CHECK(info.copy_dest_extent_length == 0);
  // The largest height the field holds still copies.
  info = ResolveToVolume(1280 * 32736);
  CHECK(info.copy_dest_coordinate_info.height_aligned_div_32 == 1023);
  CHECK(info.copy_dest_extent_length != 0);
}

// Found by resolve_fuzzer: the end of the destination, 4.08 GB from its base here, was cut to
// 32 bits, so the render target caches got an 84 MB extent for a copy writing far beyond it.
TEST_CASE("Resolve to a volume ending beyond the guest's memory is dropped",
          "[graphics][resolve]") {
  draw_util::ResolveInfo info = ResolveToVolume(8192 * 32736, 8192, 7);
  CHECK(info.copy_dest_extent_length == 0);
  // The first 4 slices of the same volume end 88 MB from the base and still copy.
  info = ResolveToVolume(8192 * 32736, 8192, 3);
  CHECK(info.copy_dest_extent_length != 0);
}
