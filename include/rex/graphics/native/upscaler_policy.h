// The native temporal upscaler's choices that need no device, DLL or cvar:
// the provider and FSR major version a native_temporal_upscaler value names,
// the color encoding given to the provider, the jitter sequence length and the
// largest upscale ratio a provider takes. temporal_upscaler.h reads the cvars
// through these. Title-agnostic.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

enum class UpscalerProvider { kFsr, kXess, kDlss };

// native_temporal_upscaler: off, fsr3, fsr4, xess, dlss ("fsr", the older value: fsr3).
inline bool UpscalerOnFor(std::string_view value) { return value != "off"; }
inline int UpscalerMajorFor(std::string_view value) { return value == "fsr4" ? 4 : 3; }
inline UpscalerProvider UpscalerProviderFor(std::string_view value) {
  return value == "xess" ? UpscalerProvider::kXess : value == "dlss" ? UpscalerProvider::kDlss : UpscalerProvider::kFsr;
}
inline const char* UpscalerName(UpscalerProvider provider) {
  return provider == UpscalerProvider::kXess ? "Intel XeSS" : provider == UpscalerProvider::kDlss ? "NVIDIA DLSS" : "AMD FSR";
}

enum class UpscalerColorSpace { kSrgb, kLinear, kHdr };

// native_fsr_color_space: srgb, linear, hdr; auto (any other value): by the
// guest target format, float and 16-bit fixed point -> hdr, else srgb.
inline UpscalerColorSpace UpscalerColorSpaceFor(std::string_view setting, uint32_t color_format) {
  if (setting == "srgb") return UpscalerColorSpace::kSrgb;
  if (setting == "linear") return UpscalerColorSpace::kLinear;
  if (setting == "hdr") return UpscalerColorSpace::kHdr;
  using F = xenos::ColorRenderTargetFormat;
  switch (F(color_format)) {
    case F::k_2_10_10_10_FLOAT: case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: case F::k_16_16:
    case F::k_16_16_16_16: case F::k_16_16_FLOAT: case F::k_16_16_16_16_FLOAT: case F::k_32_FLOAT:
    case F::k_32_32_FLOAT: return UpscalerColorSpace::kHdr;
    default: return UpscalerColorSpace::kSrgb;
  }
}
inline const char* UpscalerColorSpaceName(UpscalerColorSpace space) {
  return space == UpscalerColorSpace::kHdr ? "hdr" : space == UpscalerColorSpace::kLinear ? "linear" : "srgb";
}

// Halton jitter phases for XeSS SR and DLSS SR: 8 * (display / render)^2,
// rounded up (Intel XeSS-SR developer guide: "at least 8 * (scale)^2";
// NVIDIA DLSS programming guide 3.7.1.1: Base Phase Count 8 times the area
// ratio, 72 at Ultra Performance's 3.0). 8 at native resolution (XeSS
// native AA, DLAA) or below it; at most 72, the 3x limit of both. AMD FSR
// asks its own provider (ffxQueryDescUpscaleGetJitterPhaseCount).
inline uint32_t UpscalerJitterPhases(uint32_t render_width, uint32_t display_width) {
  const double ratio = render_width ? double(std::max(display_width, render_width)) / double(render_width) : 1.0;
  return uint32_t(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 72.0));
}

// The output's resolution scale (scale4: quarter steps) is within `limit`
// times the scene's (0: 1x). A limit of 0 or less: no limit (AMD FSR).
inline bool UpscaleWithin(double limit, uint32_t output_scale4, uint32_t scene_scale4) {
  const uint32_t scene = scene_scale4 ? scene_scale4 : 4;
  return limit <= 0.0 || double(output_scale4) <= limit * double(scene);
}

}  // namespace rex::graphics::native
