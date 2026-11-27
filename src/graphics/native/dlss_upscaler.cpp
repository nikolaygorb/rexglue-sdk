// NVIDIA DLSS Super Resolution / DLAA as the native temporal upscaler, through
// NGX (ui/vulkan/ngx.h, cmake/rexglue_dlss_sdk.cmake; Windows x64, GeForce
// RTX). The device carries NGX's extensions and features where NGX supports
// the GPU. NGX feature creation and evaluation are recorded into the
// submission thread's command buffer (replayed CmdExternal); a replaced
// feature is released once the submissions using it completed
// (Submitter::DestroyAfter), so a new output size (cutscene bars moving) costs
// no wait and no frame without the upscaler. ngx.h serialises NGX for the
// presenter's DLSS Frame Generation on other threads. Title-agnostic.
#include <rex/graphics/native/temporal_upscaler.h>
#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/submission.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/ngx.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

REXCVAR_DEFINE_STRING(native_dlss_preset, "default", "GPU",
    "DLSS model preset: default (NVIDIA's per mode: K for DLAA/Quality/Balanced, M Performance, L Ultra "
    "Performance; the NVIDIA app may override), j, k, l, m. Changing it re-creates DLSS (history reset).")
    .allowed({"default", "j", "k", "l", "m"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {
namespace {
namespace ngx = ui::vulkan::ngx;

ngx::Image ToNgx(const ResolvedTexture* texture, bool storage = false) {
  ngx::Image image;
  if (!texture || !texture->image) return image;
  image.image = texture->image;
  image.view = texture->view;
  image.format = texture->vk_format;
  image.width = texture->host_width;
  image.height = texture->host_height;
  image.storage = storage;
  return image;
}

uint32_t Preset() {
  const std::string& preset = REXCVAR_GET(native_dlss_preset);
  return preset == "j" ? 10u : preset == "k" ? 11u : preset == "l" ? 12u : preset == "m" ? 13u : 0u;
}

class DlssUpscaler final : public TemporalUpscaler {
 public:
  explicit DlssUpscaler(Submitter& submitter) : TemporalUpscaler(UpscalerProvider::kDlss, submitter) {}
  ~DlssUpscaler() override;
  bool Initialize(std::string& reason);
  bool Dispatch(const Frame& frame) override;
  uint32_t JitterPhases(uint32_t render_width, uint32_t display_width) override;
  double MaxUpscaleRatio() const override { return 3.0; }  // Ultra Performance.

 private:
  // One created feature: its handle written by the replay that creates it,
  // read by later replays and by the release after their submissions.
  struct Feature {
    ngx::Handle* handle = nullptr;
  };
  struct Payload {
    Feature* feature = nullptr;
    bool create = false;
    ngx::SrCreateInfo create_info;
    ngx::SrEvaluateInfo evaluate;
  };
  // The mode for this input and output: of those whose input range has it,
  // the one whose optimal input is the closest (2x: Performance).
  bool PickMode(uint32_t input_width, uint32_t input_height, uint32_t output_width, uint32_t output_height,
                ngx::SrMode& mode, ngx::SrRange& range);
  // The current feature goes once the submissions so far completed.
  void RetireFeature();
  static void Replay(VkCommandBuffer command_buffer, const void* payload, void* user);

  Feature* feature_ = nullptr;
  ngx::SrCreateInfo create_info_{};
  ngx::SrRange range_{};
  // Another format or depth direction than the feature's: evaluated with it
  // (not a frame without the upscaler) until it lasts.
  uint32_t mismatch_frames_ = 0;
  int logged_color_ = -1;
  // Feature creations on the submission thread (letterbox steps create one per
  // output size): count and time, logged at most every 10 s.
  std::atomic<uint64_t> creations_{0}, creation_us_{0}, creation_max_us_{0};
  uint64_t logged_creations_ = 0;
  std::chrono::steady_clock::time_point creations_logged_at_{};
};

bool DlssUpscaler::Initialize(std::string& reason) {
  auto* device = submitter_->vulkan_device();
  if (!device->ngx_sr_ready()) {
    reason = "the Vulkan device lacks DLSS's requirements: " +
             (device->ngx_sr_note().empty() ? std::string("unknown") : device->ngx_sr_note());
    return false;
  }
  if (!ngx::Initialize(*device, reason) || !ngx::Available(*device, ngx::Feature::kSuperResolution, reason))
    return false;
  const std::string version = ngx::RuntimeVersion(ngx::Feature::kSuperResolution);
  version_ = "SR " + (version.empty() ? std::string("?") : version);
  REXGPU_INFO("DLSS: SR {} available on '{}'", version.empty() ? "?" : version, device->properties().deviceName);
  return true;
}

DlssUpscaler::~DlssUpscaler() { RetireFeature(); }

void DlssUpscaler::RetireFeature() {
  if (!feature_) return;
  Feature* feature = feature_;
  feature_ = nullptr;
  // ngx::Release ignores a handle its device's shutdown released already.
  submitter_->DestroyAfter([feature] {
    ngx::Release(feature->handle);
    delete feature;
  });
}

uint32_t DlssUpscaler::JitterPhases(uint32_t render_width, uint32_t display_width) {
  return UpscalerJitterPhases(render_width, display_width);  // NVIDIA: 8 * (scale)^2.
}

bool DlssUpscaler::PickMode(uint32_t input_width, uint32_t input_height, uint32_t output_width,
                            uint32_t output_height, ngx::SrMode& mode, ngx::SrRange& range) {
  const auto& device = *submitter_->vulkan_device();
  if (input_width == output_width && input_height == output_height) {
    mode = ngx::SrMode::kDlaa;
    range = {output_width, output_height, output_width, output_height, output_width, output_height};
    return true;
  }
  static constexpr ngx::SrMode kModes[] = {ngx::SrMode::kUltraQuality, ngx::SrMode::kQuality,
                                           ngx::SrMode::kBalanced, ngx::SrMode::kPerformance,
                                           ngx::SrMode::kUltraPerformance};
  bool found = false;
  int64_t best = 0;
  for (const ngx::SrMode candidate : kModes) {
    ngx::SrRange r;
    if (!ngx::SrInputRange(device, candidate, output_width, output_height, r)) continue;
    if (input_width < r.min_width || input_width > r.max_width || input_height < r.min_height ||
        input_height > r.max_height)
      continue;
    const int64_t distance = std::llabs(int64_t(r.optimal_width) - int64_t(input_width)) +
                             std::llabs(int64_t(r.optimal_height) - int64_t(input_height));
    if (!found || distance < best) {
      found = true;
      best = distance;
      mode = candidate;
      range = r;
    }
  }
  return found;
}

void DlssUpscaler::Replay(VkCommandBuffer command_buffer, const void* payload, void* user) {
  auto* self = static_cast<DlssUpscaler*>(user);
  Payload p;
  std::memcpy(&p, payload, sizeof(p));
  Feature* feature = p.feature;
  if (p.create) {
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    feature->handle = ngx::SrCreate(*self->submitter_->vulkan_device(), command_buffer, p.create_info, error);
    const uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    self->creations_.fetch_add(1, std::memory_order_relaxed);
    self->creation_us_.fetch_add(us, std::memory_order_relaxed);
    uint64_t max = self->creation_max_us_.load(std::memory_order_relaxed);
    while (us > max && !self->creation_max_us_.compare_exchange_weak(max, us, std::memory_order_relaxed)) {}
    if (!feature->handle) {
      REXGPU_ERROR("DLSS: {}", error);
      self->replay_failed_.store(true, std::memory_order_release);
      return;
    }
  }
  if (!feature->handle) return;  // Its creation failed.
  std::string error;
  if (!ngx::SrEvaluate(command_buffer, feature->handle, p.evaluate, error)) {
    REXGPU_ERROR("DLSS: {}", error);
    self->replay_failed_.store(true, std::memory_order_release);
  }
}

bool DlssUpscaler::Dispatch(const Frame& frame) {
  if (!frame.color || !frame.depth || !frame.motion || !frame.output || !frame.width || !frame.height) {
    last_error_ = "incomplete inputs";
    return false;
  }
  const ColorSpace color = SceneColorSpace(frame.color_format);
  if (int(color) != logged_color_) {
    REXGPU_DEBUG("DLSS: color {} ({}, guest format {}){}", ColorSpaceName(color),
                 REXCVAR_GET(native_fsr_color_space), frame.color_format,
                 color == ColorSpace::kHdr ? ": HDR, auto exposure" : ": LDR input");
    logged_color_ = int(color);
  }
  const uint32_t output_width = frame.output_width ? frame.output_width : frame.width;
  const uint32_t output_height = frame.output_height ? frame.output_height : frame.height;
  if (const uint64_t creations = creations_.load(std::memory_order_relaxed); creations != logged_creations_) {
    const auto now = std::chrono::steady_clock::now();
    if (now - creations_logged_at_ >= std::chrono::seconds(10)) {
      const uint64_t count = creations - logged_creations_;
      const uint64_t total = creation_us_.exchange(0, std::memory_order_relaxed);
      const uint64_t max = creation_max_us_.exchange(0, std::memory_order_relaxed);
      REXGPU_INFO("DLSS: {} feature creation(s) since the last line, {:.1f} ms on average, {:.1f} ms at most "
                  "(the submission thread; one per output size, e.g. letterbox steps)",
                  count, double(total) / 1000.0 / double(count), double(max) / 1000.0);
      logged_creations_ = creations; creations_logged_at_ = now;
    }
  }
  ngx::SrCreateInfo wanted;
  wanted.output_width = output_width;
  wanted.output_height = output_height;
  wanted.hdr = color == ColorSpace::kHdr;
  wanted.auto_exposure = wanted.hdr;
  wanted.depth_inverted = frame.camera.depth_inverted;
  wanted.preset = Preset();
  bool create = !feature_;
  if (feature_) {
    // A new output size (cutscene bars) at once: DLSS can't change it, and the
    // old feature goes without a wait. Another format lasting 30 frames
    // re-creates; a stray frame of it is evaluated with the feature.
    constexpr uint32_t kFlagChangeFrames = 30;
    const bool size_changed =
        output_width != create_info_.output_width || output_height != create_info_.output_height;
    const bool flags_changed = wanted.hdr != create_info_.hdr || wanted.depth_inverted != create_info_.depth_inverted;
    mismatch_frames_ = flags_changed ? mismatch_frames_ + 1 : 0;
    // Inputs inside the created maximum and the mode's range keep the history.
    const bool in_range = frame.width <= create_info_.render_width && frame.height <= create_info_.render_height &&
                          frame.width >= range_.min_width && frame.height >= range_.min_height;
    create = size_changed || mismatch_frames_ >= kFlagChangeFrames || !in_range ||
             wanted.preset != create_info_.preset;
  }
  if (create) {
    ngx::SrMode mode;
    ngx::SrRange range;
    if (!PickMode(frame.width, frame.height, output_width, output_height, mode, range)) {
      last_error_ = fmt::format("{}x{} -> {}x{} is outside DLSS's ratios (up to 3x)", frame.width, frame.height,
                                output_width, output_height);
      return false;
    }
    // A size step of the same mode (letterbox bars, one per moving frame) at debug level.
    const bool same_mode = feature_ && create_info_.mode == mode && create_info_.hdr == wanted.hdr &&
                           create_info_.depth_inverted == wanted.depth_inverted &&
                           create_info_.preset == wanted.preset;
    RetireFeature();
    feature_ = new Feature();
    mismatch_frames_ = 0;
    wanted.mode = mode;
    wanted.render_width = std::max(range.max_width, frame.width);
    wanted.render_height = std::max(range.max_height, frame.height);
    create_info_ = wanted;
    range_ = range;
    const std::string version = ngx::RuntimeVersion(ngx::Feature::kSuperResolution);
    version_ = fmt::format("SR {}, {}", version.empty() ? "?" : version, ngx::SrModeName(mode));
    const std::string text = fmt::format(
        "DLSS: feature {}x{} (up to {}x{}) -> {}x{}, {}, preset {}, {}{}", frame.width, frame.height,
        wanted.render_width, wanted.render_height, output_width, output_height, ngx::SrModeName(mode),
        REXCVAR_GET(native_dlss_preset), wanted.hdr ? "HDR" : "LDR", wanted.depth_inverted ? ", inverted depth" : "");
    if (same_mode) {
      REXGPU_DEBUG("{}", text);
    } else {
      REXGPU_INFO("{}", text);
    }
  }
  Payload payload;
  payload.feature = feature_;
  payload.create = create;
  payload.create_info = create_info_;
  payload.evaluate.color = ToNgx(frame.color);
  payload.evaluate.depth = ToNgx(frame.depth);
  payload.evaluate.motion = ToNgx(frame.motion);  // RG of RGBA16F: input pixels, current -> previous, no jitter.
  payload.evaluate.output = ToNgx(frame.output, true);
  payload.evaluate.render_width = frame.width;
  payload.evaluate.render_height = frame.height;
  payload.evaluate.jitter_x = frame.jitter[0];  // NGX: input pixels, the motion vectors' axes (as FSR).
  payload.evaluate.jitter_y = frame.jitter[1];
  payload.evaluate.reset = frame.reset || create;
  payload.evaluate.delta_ms = frame.delta_ms;
  auto* commands = submitter_->OpenOutsideRenderPass();
  if (!commands) {
    if (create) RetireFeature();  // Created with the next dispatch instead.
    last_error_ = "no command buffer";
    return false;
  }
  commands->CmdExternal(&DlssUpscaler::Replay, this, &payload, sizeof(payload));
  ++dispatches_;
  return true;
}
}  // namespace

std::unique_ptr<TemporalUpscaler> LoadDlssUpscaler(Submitter& submitter, std::string& reason) {
  if (!ui::vulkan::ngx::RuntimePresent(ui::vulkan::ngx::Feature::kSuperResolution, &reason)) return nullptr;
  auto upscaler = std::make_unique<DlssUpscaler>(submitter);
  if (!upscaler->Initialize(reason)) return nullptr;
  return upscaler;
}
}  // namespace rex::graphics::native
