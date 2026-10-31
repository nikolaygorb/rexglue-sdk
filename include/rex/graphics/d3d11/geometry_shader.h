/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Ported for the ReXGlue fork
 */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>

namespace rex::graphics::d3d11 {

// Point, rectangle and quad list expansion with geometry shaders, as in the
// D3D12 backend. The generator (geometry_shader.cpp) is a copy of the D3D12
// pipeline cache's, made by tools/d3d11/sync_copies.py; the key has the same
// layout as d3d12::PipelineCache::GeometryShaderKey, and the enum the same
// values as its PipelineGeometryShader.

enum class GeometryShaderType : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union GeometryShaderKey {
  uint32_t key;
  struct {
    GeometryShaderType type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };

  GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const GeometryShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
  bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
};

bool GetGeometryShaderKey(GeometryShaderType geometry_shader_type,
                          DxbcShaderTranslator::Modification vertex_shader_modification,
                          DxbcShaderTranslator::Modification pixel_shader_modification,
                          GeometryShaderKey& key_out);
// SM 5.1 DXBC, like the D3D12 backend's; convert with ConvertShaderBytecode.
void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);

}  // namespace rex::graphics::d3d11
