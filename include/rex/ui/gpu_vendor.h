#pragma once
/**
 * @file        rex/ui/gpu_vendor.h
 * @brief       Graphics card choice by vendor (gpu_vendor), shared by the Vulkan and
 *              Direct3D 12 providers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Ported for the ReXGlue fork from ForzaHorizon1-ReImagined
 *              (patches/rexglue-sdk/0054).
 *              Portions Copyright (c) 2026 Dyotson and Forza Horizon 1: ReImagined
 *              contributors, MIT License.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace rex::ui {

enum GpuVendorId : uint32_t {
  kGpuVendorNone = 0,
  kGpuVendorAmd = 0x1002,
  kGpuVendorNvidia = 0x10DE,
  kGpuVendorIntel = 0x8086,
};

// The PCI vendor id named by the gpu_vendor cvar: "nvidia", "amd", "intel" or a hex id such as
// "0x10de". 0 when empty, "auto" or not understood (any card).
inline uint32_t PreferredGpuVendorId(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
             text.end());
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (text.empty() || text == "auto") {
    return kGpuVendorNone;
  }
  if (text == "nvidia") {
    return kGpuVendorNvidia;
  }
  if (text == "amd" || text == "ati" || text == "radeon") {
    return kGpuVendorAmd;
  }
  if (text == "intel") {
    return kGpuVendorIntel;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 16);
  if (end != text.c_str() && *end == '\0' && value > 0 && value <= 0xFFFF) {
    return uint32_t(value);
  }
  return kGpuVendorNone;
}

}  // namespace rex::ui
