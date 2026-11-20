/**
 * @file        graphics/native/system_constants.h
 * @brief       Native renderer: the translated shaders' system constants of a draw
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The SDK's VulkanCommandProcessor::UpdateSystemConstantValues (Xenia, Copyright
 *              2022 Ben Vanik, BSD license) for host render targets.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::native {

struct SystemConstantsInput {
  bool primitive_polygonal = false;
  const PrimitiveProcessor::ProcessingResult* primitive = nullptr;
  // 32-bit guest indices loaded by the vertex shader (no fullDrawIndexUint32).
  bool shader_32bit_index_dma = false;
  const draw_util::ViewportInfo* viewport_info = nullptr;
  // Render targets of 16-bit fixed formats, stored /32 on the host
  // (RenderTarget::fixed16), by index.
  uint32_t fixed16_color_targets = 0;
  // The fetch constants the shaders sample, after TextureCache::RequestTextures.
  const TextureCache* texture_cache = nullptr;
  uint32_t used_texture_mask = 0;
  // Without it, the shaders swizzle (VkPhysicalDevicePortabilitySubsetFeatures).
  bool image_view_format_swizzle = true;
  // The polygon offset the pixel shader adds (DepthStencilMode::kPolygonOffset),
  // or nullptr.
  const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset = nullptr;
  // The draw resolution scale, host pixels a guest pixel (the viewport info is
  // in host pixels).
  float resolution_scale = 1.0f;
};

// Fills all of them (textures rendered by the game sampled at 1x).
void FillSystemConstants(const RegisterFile& regs, const SystemConstantsInput& input,
                         SpirvShaderTranslator::SystemConstants& out);

}  // namespace rex::graphics::native
