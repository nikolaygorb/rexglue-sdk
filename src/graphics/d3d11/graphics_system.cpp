/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 graphics system.
 */

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11GraphicsSystem::D3D11GraphicsSystem() {}

D3D11GraphicsSystem::~D3D11GraphicsSystem() {}

std::string D3D11GraphicsSystem::name() const { return "Direct3D 11 (experimental)"; }

void D3D11GraphicsSystem::CreateProvider(bool /*with_presentation*/) {
  // Swap chains are created per window by the presenter, as on D3D12.
  provider_ = rex::ui::d3d11::D3D11Provider::Create();
}

std::unique_ptr<CommandProcessor> D3D11GraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new D3D11CommandProcessor(this, kernel_state_));
}

}  // namespace rex::graphics::d3d11
