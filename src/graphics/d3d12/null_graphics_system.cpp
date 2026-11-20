/**
 * @file        graphics/d3d12/null_graphics_system.cpp
 * @brief       Direct3D 12 graphics system with the null command processor
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Idea from EddieVanHelgen/rexglue-sdk adb783a (PerfectDarkZeroRestored, null
 * GPU backend); written for this tree.
 */

#include <rex/graphics/d3d12/null_graphics_system.h>

#include <rex/graphics/null_command_processor.h>

namespace rex::graphics::d3d12 {

std::string D3D12NullGraphicsSystem::name() const {
  return "Null (no GPU emulation)";
}

std::unique_ptr<CommandProcessor> D3D12NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::d3d12
