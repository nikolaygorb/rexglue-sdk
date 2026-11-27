/**
 * @file        launcher/launcher_devices.h
 * @brief       Rendering adapters in the order of the game's backend settings
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/ui/vulkan/fsr_versions.h>

#include "launcher_manifest.h"

namespace rex::launcher {

struct RenderDevices {
  std::vector<Choice> vulkan;
  std::vector<Choice> dxgi;
  // AMD FSR providers per Vulkan device index (empty without AMD's DLLs).
  std::vector<rex::ui::vulkan::FsrVersions> fsr;
  // Per Vulkan device index: 1 when it can run Intel XeSS SR (empty without libxess.dll).
  std::vector<uint8_t> xess;
  // Per Vulkan device index: 1 when NVIDIA NGX runs DLSS SR / DLSS Frame
  // Generation on it (empty without nvngx_dlss.dll / nvngx_dlssg.dll).
  std::vector<uint8_t> dlss, dlss_frame_generation;
};

// Enumerates without creating logical devices, except, with AMD's FSR loader
// next to the launcher, one per GPU to ask which FSR versions it offers (XeSS
// and NVIDIA NGX ask the physical device only). Keep the providers' original
// indices, including gaps, so a choice selects the same GPU in the game.
RenderDevices EnumerateRenderDevices();

}  // namespace rex::launcher
