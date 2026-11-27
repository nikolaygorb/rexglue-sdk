// Native temporal upscaler: a provider loaded at run time on the title's scene
// (SceneFrames' inputs, title scene contract): AMD FSR through AMD's signed
// ffx-api DLLs (cmake/rexglue_fsr_sdk.cmake; FSR 3.1, or FSR 4 when asked for
// and the GPU offers it), Intel XeSS SR through libxess.dll
// (cmake/rexglue_xess_sdk.cmake) or NVIDIA DLSS SR / DLAA through NGX
// (cmake/rexglue_dlss_sdk.cmake, ui/vulkan/ngx.h). Unavailable (no DLL,
// platform, device) leaves the renderer's image untouched. Title-agnostic.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <rex/cvar.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/scene_history.h>
#include <rex/graphics/native/upscaler_policy.h>

REXCVAR_DECLARE(std::string, native_temporal_upscaler);
REXCVAR_DECLARE(double, native_fsr_sharpness);
REXCVAR_DECLARE(std::string, native_fsr_color_space);
REXCVAR_DECLARE(bool, native_fsr_debug_checking);
REXCVAR_DECLARE(bool, native_fsr_reactive);
REXCVAR_DECLARE(bool, native_xess_responsive_mask);
REXCVAR_DECLARE(std::string, native_dlss_preset);

namespace rex::graphics::native {
class Submitter;

// native_temporal_upscaler, read through upscaler_policy.h.
inline bool TemporalUpscalerOn() { return UpscalerOnFor(REXCVAR_GET(native_temporal_upscaler)); }
inline int TemporalUpscalerMajor() { return UpscalerMajorFor(REXCVAR_GET(native_temporal_upscaler)); }
inline UpscalerProvider TemporalUpscalerProvider() { return UpscalerProviderFor(REXCVAR_GET(native_temporal_upscaler)); }

class TemporalUpscaler {
 public:
  // Inputs at the render size; the output at output_width x output_height
  // (the render size: native-resolution AA).
  struct Frame {
    ResolvedTexture* color = nullptr;     // Sampled.
    uint32_t color_format = 0;            // Guest xenos::ColorRenderTargetFormat of the scene.
    ResolvedTexture* depth = nullptr;     // R32F guest clip z/w, sampled.
    ResolvedTexture* motion = nullptr;    // RG = current -> previous pixels, no jitter.
    ResolvedTexture* reactive = nullptr;  // R channel, optional.
    ResolvedTexture* output = nullptr;    // Storage image.
    uint32_t width = 0, height = 0;
    uint32_t output_width = 0, output_height = 0;  // 0: the render size (native AA).
    std::array<float, 2> jitter{};        // Pixels, content moved +x right, +y down.
    bool reset = false;
    scene::CameraInfo camera{};
    float delta_ms = 0, meters_per_unit = 0;
  };
  // Null when unavailable; `reason` says why (logged once by the caller).
  static std::unique_ptr<TemporalUpscaler> Load(UpscalerProvider provider, Submitter& submitter,
                                                std::string& reason);
  virtual ~TemporalUpscaler() = default;
  // Records the work into the submitter's stream (outside a render pass).
  // Inputs must be in SHADER_READ_ONLY, the output in GENERAL layout; they are
  // left so. False: not dispatched this frame (see last_error()).
  virtual bool Dispatch(const Frame& frame) = 0;
  // AMD FSR's wanted major version (3 or 4, TemporalUpscalerMajor()): FSR 4
  // only where the GPU offers it. A change re-creates the context.
  virtual void SetWantedMajor(int wanted) { (void)wanted; }
  virtual uint32_t JitterPhases(uint32_t render_width, uint32_t display_width) = 0;
  // The largest output/render ratio it upscales (0: no limit).
  virtual double MaxUpscaleRatio() const { return 0.0; }
  UpscalerProvider provider() const { return provider_; }
  const char* name() const { return UpscalerName(provider_); }
  // A replayed dispatch failed on the submission thread: disable the provider.
  bool failed() const { return replay_failed_.load(std::memory_order_acquire); }
  const std::string& version() const { return version_; }
  // For the status line: what differs from the request (FSR 4 -> 3), else empty.
  const std::string& fallback_note() const { return fallback_note_; }
  const std::string& last_error() const { return last_error_; }
  uint64_t dispatches() const { return dispatches_; }

 protected:
  using ColorSpace = UpscalerColorSpace;
  TemporalUpscaler(UpscalerProvider provider, Submitter& submitter)
      : submitter_(&submitter), provider_(provider) {}
  // native_fsr_color_space through UpscalerColorSpaceFor.
  static ColorSpace SceneColorSpace(uint32_t color_format);
  static const char* ColorSpaceName(ColorSpace space) { return UpscalerColorSpaceName(space); }
  Submitter* submitter_ = nullptr;
  std::string version_, last_error_, fallback_note_;
  std::atomic<bool> replay_failed_{false};
  uint64_t dispatches_ = 0;

 private:
  UpscalerProvider provider_;
};

// The providers (temporal_upscaler.cpp, xess_upscaler.cpp, dlss_upscaler.cpp).
std::unique_ptr<TemporalUpscaler> LoadFsrUpscaler(Submitter& submitter, std::string& reason);
std::unique_ptr<TemporalUpscaler> LoadXessUpscaler(Submitter& submitter, std::string& reason);
std::unique_ptr<TemporalUpscaler> LoadDlssUpscaler(Submitter& submitter, std::string& reason);
}  // namespace rex::graphics::native
