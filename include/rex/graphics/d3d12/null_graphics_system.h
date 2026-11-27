/**
 * @file        graphics/d3d12/null_graphics_system.h
 * @brief       Direct3D 12 graphics system with the null command processor
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Idea from EddieVanHelgen/rexglue-sdk adb783a (PerfectDarkZeroRestored, null
 * GPU backend); written for this tree.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/d3d12/graphics_system.h>

namespace rex::graphics::d3d12 {

// graphics_backend "null-d3d12" ("null" when the plugin has no Vulkan), only
// by name (never chosen by "any"): the Direct3D
// 12 provider and presenter (the window, the overlays, frame pacing), the GPU
// registers, vblanks and interrupts, with a NullCommandProcessor
// (rex/graphics/null_command_processor.h): the guest runs as with GPU
// emulation, nothing is drawn. Also the shape of a title plugin's graphics
// system: a D3D12GraphicsSystem whose CreateCommandProcessor makes another
// command processor (docs/gpu-plugins.md).
class D3D12NullGraphicsSystem : public D3D12GraphicsSystem {
 public:
  std::string name() const override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::d3d12
