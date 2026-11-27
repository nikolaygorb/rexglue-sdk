/**
 * @file        graphics/native/graphics_system.cpp
 * @brief       Native renderer: graphics system
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/graphics_system.h>

#include <rex/graphics/native/command_processor.h>

namespace rex::graphics::native {

std::string NativeGraphicsSystem::name() const {
  if (const CommandProcessor* processor = command_processor()) {
    std::string title = processor->GetWindowTitleText();
    if (!title.empty()) {
      return title;
    }
  }
  return "Native (Vulkan)";
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::native
