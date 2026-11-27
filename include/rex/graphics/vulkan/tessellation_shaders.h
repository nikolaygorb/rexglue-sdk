/**
 * @file        graphics/vulkan/tessellation_shaders.h
 * @brief       The host stages around a guest vertex shader run as a tessellation
 *              evaluation shader: the vertex and tessellation control shaders, in GLSL
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Moved out of VulkanPipelineCache (Xenia, Copyright 2022 Ben Vanik, BSD
 *              license) for the native renderer too.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::vulkan {

// The host vertex shader: the guest's index (control point or patch) as a
// float, or with adaptive tessellation the edge factor the index buffer holds.
std::string GetTessellationIndexedVertexShaderGlsl();
std::string GetTessellationAdaptiveVertexShaderGlsl();
// Sets the tessellation levels and passes the control point indices to the
// guest's shader (the domain types of Shader::HostVertexShaderType).
std::string GetTessellationControlShaderGlsl(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             uint32_t input_control_points,
                                             uint32_t output_control_points);
// patchControlPoints of the pipeline; 0 where not supported.
uint32_t TessellationInputControlPoints(Shader::HostVertexShaderType host_vertex_shader_type,
                                        xenos::TessellationMode tessellation_mode);
// The tessellation control shader's output vertices; 0 where not supported.
uint32_t TessellationOutputControlPoints(Shader::HostVertexShaderType host_vertex_shader_type,
                                         xenos::TessellationMode tessellation_mode);

}  // namespace rex::graphics::vulkan
