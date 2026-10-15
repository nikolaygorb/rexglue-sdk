/**
 * @file        graphics/register_reset_test.cpp
 * @brief       Tests for the Xenos GPU register reset defaults
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <rex/graphics/register_file.h>

using namespace rex::graphics;

// xenia-canary e20f26963.
TEST_CASE("Register file starts with hardware reset defaults", "[graphics][registers]") {
  // About 80 KB, keep it off the stack.
  auto regs = std::make_unique<RegisterFile>();
  const uint32_t* v = regs->values;

  CHECK(v[XE_GPU_REG_VGT_MAX_VTX_INDX] == 0x0000FFFF);
  CHECK(v[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] == 0x0000FFFF);
  CHECK(v[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] == 0x20002000);
  CHECK(v[XE_GPU_REG_RB_STENCILREFMASK_BF] == 0x00FFFF00);
  CHECK(v[XE_GPU_REG_PA_SU_POINT_SIZE] == 0x00080008);
  CHECK(v[XE_GPU_REG_PA_SU_POINT_MINMAX] == 0x04000010);
  CHECK(v[XE_GPU_REG_PA_SU_LINE_CNTL] == 0x00000008);
  CHECK(v[XE_GPU_REG_PA_SC_LINE_CNTL] == 0x00000400);
  CHECK(v[XE_GPU_REG_VGT_HOS_REUSE_DEPTH] == 0x0000000E);
  CHECK(v[XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL] == 0x0000000E);
  CHECK(v[XE_GPU_REG_VGT_OUT_DEALLOC_CNTL] == 0x00000010);
  CHECK(v[XE_GPU_REG_PA_CL_GB_VERT_CLIP_ADJ] == 0x40000000);
  CHECK(v[XE_GPU_REG_PA_CL_GB_VERT_DISC_ADJ] == 0x3F800000);
  CHECK(v[XE_GPU_REG_PA_CL_GB_HORZ_CLIP_ADJ] == 0x40000000);
  CHECK(v[XE_GPU_REG_PA_CL_GB_HORZ_DISC_ADJ] == 0x3F800000);
  CHECK(v[XE_GPU_REG_PA_SC_AA_MASK] == 0x0000FFFF);
}

TEST_CASE("Tessellation levels stay zero at reset", "[graphics][registers]") {
  // The backends use (register + 1.0f), so these must not be seeded with 1.0f.
  auto regs = std::make_unique<RegisterFile>();
  CHECK(regs->values[XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL] == 0);
  CHECK(regs->values[XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL] == 0);
}
