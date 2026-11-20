/**
 * @file        graphics/vulkan/null_graphics_system.h
 * @brief       Vulkan graphics system with the null command processor
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/vulkan/graphics_system.h>

namespace rex::graphics::vulkan {

// graphics_backend "null" in a plugin with Vulkan (the Direct3D 12 version is
// d3d12/null_graphics_system.h): the Vulkan provider and presenter, the GPU
// registers, vblanks and interrupts, with a NullCommandProcessor
// (rex/graphics/null_command_processor.h). Also the shape of the native
// renderer's graphics system: a VulkanGraphicsSystem whose
// CreateCommandProcessor makes another command processor (docs/gpu-plugins.md).
class VulkanNullGraphicsSystem : public VulkanGraphicsSystem {
 public:
  std::string name() const override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::vulkan
