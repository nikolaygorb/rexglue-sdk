/**
 * @file        graphics/vulkan/glsl_compile.h
 * @brief       GLSL to SPIR-V at run time with the linked glslang
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan {

// Vulkan GLSL (one stage of `stage`) to SPIR-V 1.0, unoptimized; false with
// glslang's log in error_out. Thread-safe (glslang is initialized once).
bool CompileGlslToSpirv(VkShaderStageFlagBits stage, std::string_view source,
                        std::vector<uint32_t>& spirv_out, std::string& error_out);

}  // namespace rex::graphics::vulkan
