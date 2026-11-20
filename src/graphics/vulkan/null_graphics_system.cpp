/**
 * @file        graphics/vulkan/null_graphics_system.cpp
 * @brief       Vulkan graphics system with the null command processor
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/vulkan/null_graphics_system.h>

#include <rex/graphics/null_command_processor.h>

namespace rex::graphics::vulkan {

std::string VulkanNullGraphicsSystem::name() const {
  return "Null (no GPU emulation, Vulkan)";
}

std::unique_ptr<CommandProcessor> VulkanNullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::vulkan
