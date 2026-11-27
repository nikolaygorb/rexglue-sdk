/**
 * @file        graphics/native/graphics_system.h
 * @brief       Native renderer: graphics system
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/vulkan/graphics_system.h>

namespace rex::graphics::native {

// rexgpu-native: the Vulkan provider and presenter, the GPU registers, vblanks
// and interrupts of the SDK, with NativeCommandProcessor drawing the guest's
// commands directly instead of emulating the EDRAM (docs/gpu-plugins.md).
class NativeGraphicsSystem : public vulkan::VulkanGraphicsSystem {
 public:
  std::string name() const override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::native
