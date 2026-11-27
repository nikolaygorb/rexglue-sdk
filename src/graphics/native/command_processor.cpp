/**
 * @file        graphics/native/command_processor.cpp
 * @brief       Native renderer: command processor
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/command_processor.h>
#include <rex/graphics/native/deferred_texture.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native/resolve_placement.h>
#include <rex/graphics/native/resolve_rect.h>
#include <rex/graphics/native/system_constants.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/pipeline_wait.h>
#include <rex/graphics/util/draw_phase_timer.h>
#include <rex/graphics/vulkan/tessellation_shaders.h>
#include <rex/hash.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/system/xvideo.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#include "../vulkan/submission_watch.h"
REXCVAR_DECLARE(bool, native_scene_probe_tracking);
REXCVAR_DECLARE(std::string, native_scene_probe_dir);

// Diagnostics: each turns off one path of the native renderer, to find which
// one a wrong picture comes from (not for playing).
// Off by default: the resolve's image ignores the destination's offsets and
// sizes (AoT's simplification), which broke Perfect Dark Zero's depth of field
// and cutscenes; the guest's bits in memory are what xenos samples. Needed
// again for resolution scaling (milestone N6), fixed first.
REXCVAR_DEFINE_BOOL(native_sample_resolved_images, false, "GPU",
                    "Native renderer: a texture a resolve wrote is sampled from the resolve's "
                    "image where the format allows (incomplete: wrong with destination offsets); "
                    "off: from the guest's bits in memory, as xenos")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_edram_transfers, true, "GPU",
                    "Native renderer: surfaces sharing EDRAM get each other's contents (transfers "
                    "and clears); off: each keeps its own")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(native_drop_transfers, "", "GPU",
                      "Native renderer, debugging: EDRAM transfers to leave out, as rules "
                      "source>destination (C colour, D depth, * any; optional format number and "
                      "@samples), e.g. \"D>D@4, C6>D\"; empty: none. To find the transfers a "
                      "picture needs")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// The SDK's (vulkan/command_processor.cpp, in rexgpu-core): the depth bias in
// constant units with VK_EXT_depth_bias_control, and how long frames missing
// draws whose pipelines are being created are held back.
REXCVAR_DECLARE(bool, vulkan_depth_bias_force_unorm);
REXCVAR_DECLARE(int32_t, vulkan_async_skip_incomplete_frames_ms);
// The SDK's (vulkan/command_processor.cpp): draws a submission may hold
// (integrated GPUs), and how many frames the CPU may queue ahead.
REXCVAR_DECLARE(int32_t, vulkan_submission_draw_limit);
REXCVAR_DECLARE(int32_t, vulkan_max_frames_in_flight);
// The SDK's (command_processor.cpp): VIZ queries need the query pool even
// when occlusion_query is fake.
REXCVAR_DECLARE(bool, occlusion_query_viz);
// The SDK's (command_processor.cpp): memory export written back to the CPU
// through two buffers a frame late, or exactly.
REXCVAR_DECLARE(bool, readback_memexport_fast);
REXCVAR_DEFINE_BOOL(native_scaled_render_textures, true, "GPU",
                    "Native renderer, at a resolution scale: textures the game renders are "
                    "sampled at the scale where the resolves into them can be placed (the rest "
                    "at 1x); off: all at 1x")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_defer_full_resolve_loads, false, "GPU",
                    "Experimental Native texture loading: omit ordinary texture loads only "
                    "after a compatible single scaled resolve wholly covers the view. "
                    "Keeps dirty data and eager fallback; disabled for the context after memexport")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(native_resolution_scale, 0.0, "GPU",
                      "Native renderer: the draw resolution scale in steps of 0.25 (1.5 = "
                      "1920x1080 for a 1280x720 game, up to 4); 0: resolution_scale. Read at start")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_scene_upscale, false, "GPU",
                    "Native renderer, at a resolution scale above 1 with a temporal upscaler "
                    "running (native_temporal_upscaler): the title's scene (scene contract "
                    "surfaces) is drawn at 1x and the upscaler outputs it at the scale (faster); "
                    "post-processing and the HUD stay at the scale. Off (default): native-resolution "
                    "AA (scene drawn at the scale, the upscaler only antialiases).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_keep_render_pass, true, "GPU",
                    "Native renderer: a draw writing some of the render targets of the open "
                    "render pass stays in it, the others attached unwritten (as the SDK's "
                    "backends keep their render targets); off: each draw's own targets only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_depth_bias, true, "GPU",
                    "Native renderer: the guest's polygon offset (depth bias); off: none")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Declared in image_barrier.h.
REXCVAR_DEFINE_BOOL(native_log_edram, false, "GPU",
                    "Native renderer: logs every resolve (source surface, rectangle, destination) "
                    "and every transfer or clear that brings another surface's EDRAM tiles into "
                    "one (for development, with a GPU trace replay: many lines a frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_claim_draw_extent, true, "GPU",
                    "Native renderer: a draw takes over only the EDRAM tiles of the rows it can "
                    "reach (scissor, viewport, or its vertices when it isn't clipped), and a "
                    "surface of the draw ends where another one of the same draw begins; off: "
                    "the whole scissor, which with the 8192-row default lets the depth surface "
                    "take the colour surface's tiles")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_clear_rect_overwrite, true, "GPU",
                    "Native renderer: a rectangle drawn from the top left across the surface "
                    "that replaces all depth (stencil, colour) where it is drawn, as Direct3D 9 "
                    "clears are drawn on the console, doesn't bring in what another surface left "
                    "in the EDRAM tiles it covers whole (no transfer there); off: it does")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_hoist_uploads, true, "GPU",
                    "Native renderer: vertex and index data the game rewrote in the middle of a "
                    "render pass is uploaded before that render pass (when its earlier draws "
                    "didn't read it), which then stays open; off: each such upload ends the "
                    "render pass. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_threaded_submission, true, "GPU",
                    "Native renderer: its recorded commands are replayed into Vulkan command "
                    "buffers and submitted on a thread of its own (as vulkan_threaded_submission "
                    "for the Vulkan backend), so the driver's work leaves the GPU command "
                    "processor thread; off: on that thread. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Declared in image_barrier.h.
REXCVAR_DEFINE_BOOL(native_poison_new_images, false, "GPU",
                    "Native renderer: new textures, surfaces, resolve and composite images start "
                    "opaque magenta instead of zero, so a read of what nothing wrote shows on any "
                    "GPU (for development)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {

namespace {

// As the SDK's Vulkan backend: a float depth buffer's bias unit shrinks with
// the depth (decals and layered snow Z-fight, Perfect Dark Zero issue #15)
// unless VK_EXT_depth_bias_control applies it in constant units.
bool IsDepthBiasForceUnorm(const ui::vulkan::VulkanDevice::Properties& device_properties) {
  return REXCVAR_GET(vulkan_depth_bias_force_unorm) && device_properties.depthBiasControl &&
         device_properties.leastRepresentableValueForceUnormRepresentation &&
         device_properties.depthBiasExact;
}

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : NullCommandProcessor(graphics_system, kernel_state) {
  log_null_backend_ = false;
}

NativeCommandProcessor::~NativeCommandProcessor() = default;

std::string NativeCommandProcessor::GetWindowTitleText() const {
  return "Native (Vulkan, early)";
}

ui::vulkan::VulkanDevice* NativeCommandProcessor::GetVulkanDevice() const {
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(graphics_system_->provider());
  return provider ? provider->vulkan_device() : nullptr;
}

bool NativeCommandProcessor::SetupContext() {
  if (!NullCommandProcessor::SetupContext()) {
    return false;
  }
  // The null base fakes occlusion queries (it has no query pool); native
  // answers them from its own (EnsureQueryResources), so occlusion_query
  // applies. Fake, every query read as visible: 584109C2's light glares
  // shone through walls and floors.
  zpd_mode_ = GetZPDMode();
  ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  if (!vulkan_device) {
    REXGPU_ERROR("Native renderer: no Vulkan device");
    return false;
  }
  // A hoist point before each draws' render pass, so uploads and texture
  // loads can go before it (native_hoist_uploads); the replay and the
  // submission on a thread of their own (native_threaded_submission).
  submitter_.SetHoistBeforeRenderPasses(REXCVAR_GET(native_hoist_uploads));
  defer_full_resolve_loads_ = REXCVAR_GET(native_defer_full_resolve_loads);
  defer_disabled_by_memexport_ = false;
  defer_vertex_shader_ = defer_pixel_shader_ = nullptr;
  defer_shader_mask_ = 0;
  deferred_fetch_memos_.fill({});
  scaled_view_memos_.fill({});
  deferred_stats_ = {};
  if (defer_full_resolve_loads_) {
    REXGPU_INFO("Native renderer: experimental full-resolve texture deferral enabled");
  }
  submitter_.SetThreadedSubmission(REXCVAR_GET(native_threaded_submission));
  if (!submitter_.Initialize(vulkan_device)) {
    REXGPU_ERROR("Native renderer: failed to initialise");
    return false;
  }
  {
    // vulkan_submission_draw_limit's automatic limit is for integrated GPUs.
    VkPhysicalDeviceProperties physical_device_properties;
    vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(
        vulkan_device->physical_device(), &physical_device_properties);
    integrated_gpu_ =
        physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  }
  // The draw resolution scale, read at start: native_resolution_scale in
  // quarters, or the SDK's whole one (resolution_scale, or
  // draw_resolution_scale_x / _y), the same along both axes.
  if (double native_scale = REXCVAR_GET(native_resolution_scale); native_scale > 0.0) {
    scale4_ = Scale4FromFactor(native_scale);
    if (double(scale4_) * 0.25 != native_scale) {
      REXGPU_WARN("Native renderer: native_resolution_scale {} is used as {:.2f}", native_scale,
                  double(scale4_) * 0.25);
    }
  } else {
    uint32_t config_scale_x, config_scale_y;
    TextureCache::GetConfigDrawResolutionScale(config_scale_x, config_scale_y);
    uint32_t scale = std::min({config_scale_x, config_scale_y, kScale4Max / kScale4Min});
    if (config_scale_x != config_scale_y || scale != config_scale_x) {
      REXGPU_WARN(
          "Native renderer: draw resolution scale {}x{} is used as {}x (one scale for both axes, "
          "up to {}x)",
          config_scale_x, config_scale_y, scale, kScale4Max / kScale4Min);
    }
    scale4_ = std::max(scale, uint32_t(1)) * kScale4Min;
  }
  shared_memory_ = std::make_unique<NativeSharedMemory>(*memory_);
  primitive_processor_ =
      std::make_unique<NativePrimitiveProcessor>(*register_file_, *memory_, *shared_memory_);
  draw_extent_estimator_ = std::make_unique<DrawExtentEstimator>(*register_file_, *memory_);
  texture_cache_ =
      std::make_unique<NativeTextureCache>(*register_file_, *shared_memory_, submitter_);
  // Object motion: the scene draws' vertex shaders keep their positions in its
  // buffer (translated in, bound with the shared memory).
  object_motion_.reset();
  if (ObjectMotion::Requested()) {
    if (!vulkan_device->properties().vertexPipelineStoresAndAtomics) {
      REXGPU_WARN("Native object motion: off, the device can't store from vertex shaders");
    } else {
      auto object_motion = std::make_unique<ObjectMotion>();
      if (object_motion->Initialize(submitter_)) object_motion_ = std::move(object_motion);
    }
  }
  shaders_.SetMotionCapture(object_motion_ != nullptr);
  pipelines_.SetMotionCaptureBinding(object_motion_ != nullptr);
  if (!shared_memory_->Initialize(submitter_) || !primitive_processor_->Initialize(submitter_) ||
      !texture_cache_->Initialize() ||
      !shaders_.Initialize(vulkan_device, scale4_) ||
      !pipelines_.Initialize(submitter_) ||
      !bindings_.Initialize(submitter_, pipelines_, shared_memory_->buffer(),
                            SharedMemory::kBufferSize,
                            object_motion_ ? object_motion_->buffer() : VK_NULL_HANDLE) ||
      !render_targets_.Initialize(submitter_, xenos::kEdramTileCount, scale4_) ||
      !resolver_.Initialize(submitter_, render_targets_, shared_memory_.get()) ||
      !composites_.Initialize(submitter_, render_targets_) ||
      !present_.Initialize(submitter_)) {
    REXGPU_ERROR("Native renderer: failed to initialise");
    ShutdownModules();
    return false;
  }
  // native_scene_upscale draws the title's scene at 1x (its resolves then
  // become ordinary 1x textures any draw samples).
  render_targets_.SetSceneScale(kScale4Min);
  // Replaced once the upscaler runs or fails (the launcher shows the line).
  WriteUpscalerStatus(submitter_,
                      !TemporalUpscalerOn()
                          ? std::string("Upscaler off")
                          : std::string(UpscalerName(TemporalUpscalerProvider())) +
                                " on, not used: no 3D scene from the game (only menus, or no game support)");
  readback_.Initialize(submitter_, *shared_memory_);
  // Resolves at 1x into the textures of their destinations
  // (native_resolve_into_textures).
  resolver_.SetTextureCache(texture_cache_.get());
  pipeline_storage_.Initialize(shaders_, pipelines_);
  memexport_readback_.Initialize(submitter_, *shared_memory_, readback_, *memory_);
  // Occlusion queries: closed before the draws' render pass ends, copied for
  // the CPU at the end of their submission.
  submitter_.SetBeforeEndRenderPass([this]() { OnDrawRenderPassEnding(); });
  submitter_.AddBeforeSubmit([this]() { RecordQueryResolves(); });
  primitive_processor_->BeginFrame(frame_ + 1, 0);
  context_ready_ = true;
  REXGPU_WARN(
      "Native renderer: early (multisampled EDRAM sharing untested), resolution "
      "scale {:.2f}x (use gpu_plugin = \"xenos\" to play)",
      double(scale4_) * 0.25);
  return true;
}

void NativeCommandProcessor::ShutdownModules() {
  // Everything recorded is submitted and done before anything is destroyed.
  submitter_.AwaitAll();
  if (present_deferred_presenter_) {
    // Its queued steps have run on the submission thread.
    submitter_.AwaitQueued();
    present_deferred_presenter_->SetGuestOutputPublishDeferrer(nullptr);
    present_deferred_presenter_ = nullptr;
  }
  query_resolves_in_flight_.clear();
  active_query_index_ = UINT32_MAX;
  active_query_generation_ = 0;
  query_pool_.reset();
  viz_pending_copies_.clear();
  if (viz_predicate_buffer_ != VK_NULL_HANDLE || viz_predicate_memory_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, vulkan_device->device(),
                                           viz_predicate_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, vulkan_device->device(),
                                           viz_predicate_memory_);
  }
  viz_predicate_buffer_failed_ = false;
  present_.Shutdown();
  // Before the shaders and pipelines: writes what is queued, saves the
  // driver's cache.
  pipeline_storage_.Shutdown();
  if (scene_frames_) {
    const auto directory = REXCVAR_GET(native_scene_probe_dir);
    if (!directory.empty()) scene_frames_->Report(std::filesystem::path(directory) / "frame-consumer.json");
    scene_frames_.reset();
  }
  object_motion_.reset();
  scene_frames_init_failed_ = false;
  scene_resolve_pass_.reset();
  scene_last_draw_frame_ = 0; scene_last_jittered_ = false; scene_jitter_ = {}; scene_jitter_draws_ = 0;
  memexport_readback_.Shutdown();
  readback_.Shutdown();
  composites_.Shutdown();
  resolver_.Shutdown();
  render_targets_.Shutdown();
  bindings_.Shutdown();
  pipelines_.Shutdown();
  shaders_.Shutdown();
  // Before the shared memory, whose ranges its textures watch.
  texture_cache_.reset();
  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  if (shared_memory_) {
    shared_memory_->Shutdown();
    shared_memory_.reset();
  }
  submitter_.Shutdown();
}

void NativeCommandProcessor::ShutdownContext() {
  if (context_ready_) {
    ShutdownModules();
    context_ready_ = false;
  }
  frame_submissions_ = {};
  frame_ = 0;
  frames_in_flight_.clear();
  completed_frame_ = 0;
  frame_draws_recorded_ = 0;
  frame_used_pending_pipeline_ = false;
  incomplete_frames_held_ = false;
  draw_logged_ = 0;
  bound_serial_ = UINT64_MAX;
  bound_pipeline_ = VK_NULL_HANDLE;
  bound_dynamic_state_valid_ = false;
  NullCommandProcessor::ShutdownContext();
}

void NativeCommandProcessor::LogDrawOnce(uint32_t reason_bit, const char* message) {
  if (!(draw_logged_ & reason_bit)) {
    draw_logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

uint64_t NativeCommandProcessor::CompletedFrame() {
  uint64_t completed_submission = submitter_.completed();
  while (!frames_in_flight_.empty() && frames_in_flight_.front().second <= completed_submission) {
    completed_frame_ = frames_in_flight_.front().first;
    frames_in_flight_.pop_front();
  }
  return completed_frame_;
}

void NativeCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void NativeCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void NativeCommandProcessor::OnPrimaryBufferEnd() {
  // As the SDK's Vulkan backend: the GPU works on the start of the frame while
  // the rest is recorded, instead of on all of it after the swap. A wait in
  // the frame (a resolve the guest reads back, an occlusion query) then waits
  // for what is left, not for everything recorded since the last submission.
  // Submitting ends the render pass (its query segment too), as a wait's
  // submission always did.
  if (context_ready_ && REXCVAR_GET(vulkan_submit_on_primary_buffer_end) &&
      submitter_.is_open()) {
    submitter_.Submit();
  }
}

void NativeCommandProcessor::OnGuestMemorySyncPoint() {
  if (shared_memory_) {
    shared_memory_->AdvanceHotPageSyncPoint();
  }
}

void NativeCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     uint32_t title_id, bool blocking,
                                                     std::function<void()> completion_callback) {
  // On the command processor thread (or with it paused), once set up.
  if (context_ready_) {
    pipeline_storage_.Load(cache_root, title_id, blocking);
  }
  if (completion_callback) {
    completion_callback();
  }
}

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  if (!context_ready_) {
    return NullCommandProcessor::LoadShader(shader_type, guest_address, host_address, dword_count);
  }
  return shaders_.Load(shader_type, host_address, dword_count);
}

void NativeCommandProcessor::CollectRendererFrameCounters(RendererFrameCounters& counters) {
  counters.reported = true;
  counters.gpu_timed = submitter_.can_time_gpu();
  counters.gpu_ns += submitter_.TakeGpuTimeNs();
  // native_gpu_time_by_kind, of the same submissions.
  uint64_t gpu_kind_ns[size_t(GpuKind::kCount)];
  uint64_t gpu_kind_marks_dropped;
  submitter_.TakeGpuKindNs(gpu_kind_ns, gpu_kind_marks_dropped);
  Submitter::Counters submitter_counters = submitter_.TakeCounters();
  counters.submissions += submitter_counters.submissions;
  counters.draw_render_passes += submitter_counters.render_passes;
  counters.draw_pipeline_binds += stats_pipeline_binds_;
  stats_pipeline_binds_ = 0;
  // IssueDraw by phase, and why the draws' render pass ended, a frame.
  static const char* const kPhaseNames[] = {
      "shaders", "indices", "vertex data", "textures", "targets", "pipeline", "state",
      "layouts", "views",   "samplers",    "pass",     "descriptors", "record",
      "claims",
  };
  static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == size_t(DrawPhase::kCount));
  const auto claim_stats = render_targets_.TakeClaimStats();
  const auto resident_stats = texture_cache_ ? texture_cache_->TakeResidentLoadStats()
                                            : NativeTextureCache::ResidentLoadStats{};
  if (texture_cache_) {
    // The first swap discards pre-window counts, including texture attribution.
    texture_cache_->LogResidentLoadDetails(counters.frames != 0);
  }
  if (counters.frames) {
    double frames = double(counters.frames);
    if (texture_cache_ && texture_cache_->profile_transfers()) {
      const auto& c = stats_large_coverage_;
      REXGPU_INFO("Native large resolved textures (>= 1 MiB), computations per frame: pieces 0 "
                  "{:.1f}, 1 {:.1f}, 2 {:.1f}, 3+ {:.1f}; covered by one {:.1f}, by several {:.1f}",
                  double(c[0]) / frames, double(c[1]) / frames, double(c[2]) / frames,
                  double(c[3]) / frames, double(c[4]) / frames, double(c[5]) / frames);
    }
    double ms_per_tick = 1000.0 / double(rex::chrono::Clock::QueryHostTickFrequency());
    std::string detail =
        fmt::format("IssueDraw ms a frame (1 draw in {} timed):", kDrawPhaseSampleRate);
    for (size_t i = 0; i < size_t(DrawPhase::kCount); ++i) {
      detail += fmt::format(" {} {:.2f}{}", kPhaseNames[i],
                            double(stats_draw_phase_ticks_[i]) * kDrawPhaseSampleRate *
                                ms_per_tick / frames,
                            i + 1 < size_t(DrawPhase::kCount) ? "," : "");
    }
    Bindings::Stats binding_stats = bindings_.TakeStats();
    detail += fmt::format(
        "; draws with the last draw's textures a frame {:.0f}, texture views as the last time "
        "for their fetch constant {:.0f}, texture sets written {:.0f}, found written {:.0f}, "
        "pushed {:.0f}",
        double(stats_draw_textures_reused_) / frames, double(stats_binding_views_reused_) / frames,
        double(binding_stats.texture_sets_written) / frames,
        double(binding_stats.texture_sets_found) / frames,
        double(binding_stats.texture_sets_pushed) / frames);
    detail += fmt::format(
        "; draws kept in the open render pass a frame {:.0f}, not kept: more targets {:.0f}, "
        "other targets {:.0f}, scissor outside {:.0f}",
        double(stats_pass_kept_draws_) / frames, double(stats_pass_not_kept_[0]) / frames,
        double(stats_pass_not_kept_[1]) / frames, double(stats_pass_not_kept_[2]) / frames);
    detail += "; render passes ended a frame by:";
    bool any_end = false;
    for (size_t i = 0; i < size_t(RenderPassEnd::kCount); ++i) {
      if (submitter_counters.render_pass_ends[i]) {
        detail += fmt::format("{} {} {:.1f}", any_end ? "," : "",
                              RenderPassEndName(RenderPassEnd(i)),
                              double(submitter_counters.render_pass_ends[i]) / frames);
        any_end = true;
      }
    }
    if (!any_end) {
      detail += " none";
    }
    detail += fmt::format("; uploads before the render pass a frame {:.1f}, texture loads {:.1f}",
                          double(submitter_counters.uploads_before_render_pass) / frames,
                          double(submitter_counters.texture_loads_before_render_pass) / frames);
    detail += fmt::format(
        "; resolves ms a frame: resolve {:.2f}, readback {:.2f}; GPU waits a frame {:.1f}, "
        "{:.2f} ms; submission thread waits a frame {:.1f}, {:.2f} ms; scaled texture lookups "
        "a frame {:.0f}, computed {:.0f}; resolve images {}",
        double(stats_copy_resolve_ticks_) * ms_per_tick / frames,
        double(stats_copy_readback_ticks_) * ms_per_tick / frames,
        double(submitter_counters.gpu_waits) / frames,
        double(submitter_counters.gpu_wait_ticks) * ms_per_tick / frames,
        double(submitter_counters.thread_waits) / frames,
        double(submitter_counters.thread_wait_ticks) * ms_per_tick / frames,
        double(stats_scaled_view_lookups_) / frames,
        double(stats_scaled_view_computed_) / frames, resolver_.texture_count());
    if (stats_submission_splits_) {
      detail += fmt::format("; submissions ended for vulkan_submission_draw_limit a frame {:.1f}",
                            double(stats_submission_splits_) / frames);
    }
    if (submitter_.gpu_time_by_kind()) {
      // The SDK's Vulkan backend's vulkan_gpu_time kinds, resolves by step.
      detail += "; GPU ms a frame by kind:";
      for (size_t i = 0; i < size_t(GpuKind::kCount); ++i) {
        detail += fmt::format("{} {} {:.2f}", i ? "," : "", GpuKindName(GpuKind(i)),
                              double(gpu_kind_ns[i]) * 1.0e-6 / frames);
      }
      detail += fmt::format("; marks not written {}", gpu_kind_marks_dropped);
    }
    if (defer_full_resolve_loads_) {
      detail += fmt::format(
          "; deferred texture slots per frame: candidates {:.1f}, served {:.1f}, fallback {:.1f}, "
          "stale served {:.1f}, stale guest MiB {:.2f}, generation fallback {:.1f}; "
          "deferral disabled by memexport {}",
          double(deferred_stats_.candidates) / frames, double(deferred_stats_.served) / frames,
          double(deferred_stats_.fallback) / frames, double(deferred_stats_.stale_served) / frames,
          double(deferred_stats_.stale_bytes) / frames / 1048576.0,
          double(deferred_stats_.generation_fallback) / frames, defer_disabled_by_memexport_);
      detail += fmt::format(
          "; deferred resolve origin lookups per frame: queried {:.1f}, cached {:.1f}",
          double(deferred_stats_.origin_queries) / frames, double(deferred_stats_.origin_reused) / frames);
    }
    {
      // native_resolve_into_textures (scale 1).
      const Resolver::Stats resolve_stats = resolver_.TakeStats();
      const uint64_t* into = resolve_stats.into_textures;
      detail += fmt::format(
          "; resolves into textures a frame {:.1f} (not: format {:.1f}, red/blue unswizzled "
          "{:.1f}, memory not valid {:.1f}, none {:.1f}), images not written {:.1f}, copied "
          "from the render target without an image {:.1f}",
          double(into[size_t(NativeTextureCache::ResolveCopy::kCopied)]) / frames,
          double(into[size_t(NativeTextureCache::ResolveCopy::kFormat)]) / frames,
          double(into[size_t(NativeTextureCache::ResolveCopy::kSwap)]) / frames,
          double(into[size_t(NativeTextureCache::ResolveCopy::kNotMarked)]) / frames,
          double(into[size_t(NativeTextureCache::ResolveCopy::kNoTexture)]) / frames,
          double(resolve_stats.images_skipped) / frames,
          double(resolve_stats.images_direct) / frames);
    }
    detail += fmt::format(
        "; binding work per frame: uniform requests {:.0f}, bytes {:.0f}, "
        "constant sets {:.0f}, descriptor binds {:.0f}, push reasons invalid {:.0f}, "
        "layout {:.0f}, content {:.0f}",
        double(binding_stats.uniform_requests) / frames, double(binding_stats.uniform_bytes) / frames,
        double(binding_stats.constants_sets_written) / frames,
        double(binding_stats.descriptor_binds) / frames, double(binding_stats.pushes_invalid) / frames,
        double(binding_stats.pushes_layout) / frames, double(binding_stats.pushes_content) / frames);
    if (texture_cache_ && texture_cache_->profile_transfers()) {
      detail += fmt::format(
          "; resident loads per frame: attempts {:.1f}, recorded {:.1f}, initial {:.1f}, reload {:.1f}, "
          "same submission {:.1f}, base {:.1f}, mips {:.1f}, guest MB {:.2f}",
          double(resident_stats.attempts) / frames, double(resident_stats.recorded) / frames,
          double(resident_stats.initial) / frames, double(resident_stats.reload) / frames,
          double(resident_stats.same_submission) / frames, double(resident_stats.base) / frames,
          double(resident_stats.mips) / frames, double(resident_stats.guest_bytes) / frames / 1048576.0);
      detail += fmt::format(
          "; EDRAM claim work per frame: claims {:.1f}, no ops {:.1f}, clears {:.1f}, transfers {:.1f}, "
          "rects {:.1f}, guest Mpixels {:.2f}, reinterpret {:.1f}, stencil {:.1f}, tiles kept "
          "valid {:.1f}",
          double(claim_stats.claims) / frames, double(claim_stats.no_ops) / frames,
          double(claim_stats.clears) / frames, double(claim_stats.transfers) / frames,
          double(claim_stats.transfer_rects) / frames,
          double(claim_stats.transfer_guest_pixels) / frames / 1000000.0,
          double(claim_stats.reinterpret) / frames, double(claim_stats.stencil) / frames,
          double(claim_stats.kept_tiles) / frames);
      // Who claimed: claims, transfers, clears and transfer guest Mpixels a
      // frame, colour and depth targets apart.
      detail += "; EDRAM claims by caller a frame (claims/transfers/clears/Mpx):";
      bool any_caller = false;
      for (size_t i = 0; i < size_t(RenderTargets::ClaimCaller::kCount); ++i) {
        for (size_t depth = 0; depth < 2; ++depth) {
          const auto& by = claim_stats.by_caller[i][depth];
          if (!by.claims) {
            continue;
          }
          detail += fmt::format(
              "{} {} {} {:.1f}/{:.1f}/{:.1f}/{:.2f}", any_caller ? "," : "",
              RenderTargets::ClaimCallerName(RenderTargets::ClaimCaller(i)),
              depth ? "depth" : "colour", double(by.claims) / frames,
              double(by.transfers) / frames, double(by.clears) / frames,
              double(by.transfer_guest_pixels) / frames / 1000000.0);
          any_caller = true;
        }
      }
      if (!any_caller) {
        detail += " none";
      }
      // Claims and writes that walked their tiles (the rest took a fast path).
      const EdramOwnership::WalkCounts& walk = render_targets_.edram().walk_counts();
      detail += fmt::format(
          "; EDRAM walks a frame: claims {:.1f} ({:.0f} tiles), writes {:.1f} ({:.0f} tiles)",
          double(walk.claims - stats_walk_last_.claims) / frames,
          double(walk.claim_tiles - stats_walk_last_.claim_tiles) / frames,
          double(walk.writes - stats_walk_last_.writes) / frames,
          double(walk.write_tiles - stats_walk_last_.write_tiles) / frames);
    }
    if (binding_stats.bind_profile_calls) {
      static const char* const kBindPhaseNames[] = {
          "uniforms", "constants", "constant allocate", "constant update", "texture sets",
          "bind prepare", "bind API", "push prepare", "push API",
      };
      static_assert(sizeof(kBindPhaseNames) / sizeof(kBindPhaseNames[0]) ==
                    size_t(Bindings::BindPhase::kCount));
      detail += fmt::format("; Bind CPU samples {} / {} calls (probability 1/64)",
                            binding_stats.bind_profile_samples, binding_stats.bind_profile_calls);
      if (binding_stats.bind_profile_samples) {
        // All sampled Bind calls are the denominator, including calls skipping an API.
        // These are sample means, not per-API-call latencies or exact frame costs.
        detail += "; Bind us per sample:";
        for (size_t i = 0; i < size_t(Bindings::BindPhase::kCount); ++i) {
          detail += fmt::format(" {} {:.3f}{}", kBindPhaseNames[i],
                                double(binding_stats.bind_phase_ticks[i]) * ms_per_tick * 1000.0 /
                                    double(binding_stats.bind_profile_samples),
                                i + 1 < size_t(Bindings::BindPhase::kCount) ? "," : "");
        }
      }
    }
    counters.detail = std::move(detail);
  }
  std::memset(stats_draw_phase_ticks_, 0, sizeof(stats_draw_phase_ticks_));
  stats_pass_kept_draws_ = 0;
  std::memset(stats_pass_not_kept_, 0, sizeof(stats_pass_not_kept_));
  stats_draw_textures_reused_ = 0;
  stats_binding_views_reused_ = 0;
  stats_copy_resolve_ticks_ = 0;
  stats_copy_readback_ticks_ = 0;
  stats_scaled_view_lookups_ = 0;
  stats_scaled_view_computed_ = 0;
  stats_large_coverage_ = {};
  deferred_stats_ = {};
  stats_submission_splits_ = 0;
  stats_walk_last_ = render_targets_.edram().walk_counts();
  if (!counters.frames) {
    bindings_.TakeStats();
    resolver_.TakeStats();
  }
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  EndZPDFrame();
  // Command buffers are timed on the graphics card for the statistics, and by
  // kind for the performance counters too (native_gpu_time_by_kind).
  submitter_.SetGpuTimeEnabled(frame_stats_enabled() || submitter_.gpu_time_by_kind());
  submitter_.FlushGpuTimePerfCounters();
  if (!frame_) {
    REXGPU_INFO("Native renderer: first swap (front buffer {:08X}, {}x{}), {} draws, {} pipelines",
                frontbuffer_ptr, frontbuffer_width, frontbuffer_height, frame_draws_recorded_,
                pipelines_.pipeline_count());
  }
  if (!context_ready_) {
    return;
  }

  // Not before the frame vulkan_max_frames_in_flight (0: kMaxFramesInFlight)
  // back has finished on the GPU, as the Vulkan backend: fewer frames queued
  // shorten readback waits and input latency.
  uint32_t frame_slot = uint32_t(frame_ % kMaxFramesInFlight);
  {
    const int32_t configured_depth = REXCVAR_GET(vulkan_max_frames_in_flight);
    uint32_t queue_depth = configured_depth > 0
                               ? std::min(uint32_t(configured_depth), kMaxFramesInFlight)
                               : kMaxFramesInFlight;
    // Frame generation holds a real frame back already: fewer queued ones keep
    // its latency down.
    if (const ui::Presenter* fg_presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
        fg_presenter && fg_presenter->IsFrameGenerationActive()) {
      queue_depth = std::min(queue_depth, uint32_t(2));
    }
    const uint64_t await_frame_submission =
        frame_submissions_[uint32_t((frame_ + kMaxFramesInFlight - queue_depth) %
                                    kMaxFramesInFlight)];
    if (await_frame_submission) {
      submitter_.Await(await_frame_submission);
    }
  }
  // A frame missing draws whose pipelines are being created is not shown
  // (vulkan_async_skip_incomplete_frames, as the SDK's Vulkan backend), unless
  // frames have been held back for vulkan_async_skip_incomplete_frames_ms.
  bool hold_frame = REXCVAR_GET(async_shader_compilation) &&
                    REXCVAR_GET(vulkan_async_skip_incomplete_frames) &&
                    frame_used_pending_pipeline_;
  frame_used_pending_pipeline_ = false;
  if (hold_frame) {
    auto now = std::chrono::steady_clock::now();
    if (!incomplete_frames_held_) {
      incomplete_frames_held_ = true;
      incomplete_frames_held_since_ = now;
    }
    int32_t hold_ms = REXCVAR_GET(vulkan_async_skip_incomplete_frames_ms);
    if (hold_ms > 0 && now - incomplete_frames_held_since_ >= std::chrono::milliseconds(hold_ms)) {
      hold_frame = false;
    }
  } else {
    incomplete_frames_held_ = false;
  }
  ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
  // With native_threaded_submission the refresher's work reaches the queue
  // later, on the submission thread: the presenter's step after a refresh
  // (making the image the next to show) runs there too, behind the frame's
  // submission (as vulkan_present_on_submission_thread).
  if (presenter && submitter_.threaded_submission() && present_deferred_presenter_ != presenter) {
    if (present_deferred_presenter_) {
      submitter_.AwaitQueued();
      present_deferred_presenter_->SetGuestOutputPublishDeferrer(nullptr);
    }
    present_publication_callback_ = 0;
    presenter->SetGuestOutputPublishDeferrer(
        [this](std::function<void()> step) {
          present_publication_callback_ = submitter_.QueueAfterSubmissions(std::move(step));
        },
        [this](std::function<void()> step) {
          submitter_.QueueAfterSubmissions(std::move(step));
        });
    present_deferred_presenter_ = presenter;
  }
  if (presenter && !hold_frame) {
    PresentFrame(presenter, frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  }
  // Scene render scale: the next frame's scene interval opens at the swap,
  // not at a title marker (markers can be lost for ring space), while views
  // arrive and an upscaler runs; kSceneEnd closes it. Flipping the scene's
  // scale costs a history reset each time.
  if (scene_frames_) {
    scene_swapped_session_ = scene_marker_session_; scene_swapped_frame_ = scene_marker_frame_;
    scene_swaps_since_view_ = std::min(scene_swaps_since_view_ + 1, uint32_t(1) << 30);
    render_targets_.SetSceneInterval(REXCVAR_GET(native_scene_upscale) &&
                                     scene_frames_->UpscalerActive() && scene_frames_->UpscaleFits() &&
                                     scene_swaps_since_view_ <= 30);
  }
  // Also when the presenter skipped the refresher (minimised window).
  submitter_.Submit();
  // Occlusion query results of finished submissions to the guest.
  PumpQueryResolves();
  PumpPendingRetire();

  // The frame ends.
  uint64_t frame_end_submission = submitter_.current() - 1;
  frame_submissions_[frame_slot] = frame_end_submission;
  frames_in_flight_.emplace_back(frame_ + 1, frame_end_submission);
  primitive_processor_->EndFrame();
  bindings_.EndFrame(frame_end_submission);
  composites_.EndFrame(frame_ + 1);
  pipeline_storage_.OnFrameEnd();
  memexport_readback_.EndFrame(frame_ + 1);
  readback_.EndFrame(frame_ + 1);
  if (render_targets_.scale4() != kScale4Min && (frame_ + 1) % 300 == 0) {
    LogScaledFetchSummary();
  }
  texture_cache_->UpdateSubmissions();
  texture_cache_->BeginFrame();
  if (REXCVAR_GET(clear_memory_page_state)) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }
  shared_memory_->AdvanceHotPageFrame();
  ++frame_;
  frame_draws_recorded_ = 0;
  primitive_processor_->BeginFrame(frame_ + 1, CompletedFrame());
}

void NativeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  const bool constant = index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
                        index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31;
  const bool constant_changed = constant && register_file_->values[index] != value;
  bool fetch_changed = constant_changed && index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
                       index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5;
  NullCommandProcessor::WriteRegister(index, value);
  // The constant buffers holding it are written again for the next draw, if
  // its value changed (games rewrite many constants with the values they
  // have). (Float 0x4000..., fetch 0x4800..., bool and loop 0x4900...0x4927.)
  if (constant_changed) {
    bindings_.ConstantRegisterWritten(index);
  }
  // Games rewrite many fetch constants with the values they have: only the
  // changed ones make the cache look up the slot's texture again.
  if (fetch_changed) {
    ++fetch_constant_changes_;
    ++fetch_constant_serials_[(index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6];
  }
  if (fetch_changed && texture_cache_) {
    texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                6);
  }
}

void NativeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  // The ranges of the constant packets and of type 0 packets: through
  // WriteRegister each register cost two virtual calls and a bindings check,
  // more than a third of the time the command processor spent outside draws.
  const uint32_t range_end = start_index + num_registers;
  uint32_t index = start_index;
  while (index < range_end) {
    enum class Block { kNone, kFloat, kFetch, kBoolLoop } block;
    uint32_t part_end;
    if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
      block = Block::kFloat;
      part_end = XE_GPU_REG_SHADER_CONSTANT_511_W + 1;
    } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
               index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
      block = Block::kFetch;
      part_end = XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1;
    } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
               index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
      block = Block::kBoolLoop;
      part_end = XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1;
    } else {
      block = Block::kNone;
      if (index < XE_GPU_REG_SHADER_CONSTANT_000_X) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_000_X;
      } else if (index < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
      } else if (index < XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
      } else {
        part_end = range_end;
      }
    }
    part_end = std::min(part_end, range_end);
    uint32_t* part_base = base + (index - start_index);
    uint32_t part_count = part_end - index;
    switch (block) {
      case Block::kNone:
        // Other registers: stored, or WriteRegister for those with an effect.
        CommandProcessor::WriteRegistersFromMem(index, part_base, part_count);
        break;
      case Block::kFloat:
      case Block::kBoolLoop: {
        // Only the stage buffers holding a changed one are written again: games
        // rewrite many constants with the values they have, and each upload
        // gave the draw a new buffer offset to bind. Swapped in chunks, each
        // compared at once.
        constexpr uint32_t kChunk = 256;
        uint32_t swapped[kChunk];
        uint32_t* values = register_file_->values + index;
        uint32_t first_changed = UINT32_MAX, last_changed = 0;
        for (uint32_t chunk = 0; chunk < part_count; chunk += kChunk) {
          const uint32_t count = std::min(kChunk, part_count - chunk);
          rex::memory::copy_and_swap(swapped, part_base + chunk, count);
          if (std::memcmp(values + chunk, swapped, sizeof(uint32_t) * count)) {
            std::memcpy(values + chunk, swapped, sizeof(uint32_t) * count);
            if (first_changed == UINT32_MAX) {
              first_changed = chunk;
            }
            last_changed = chunk + count - 1;
          }
        }
        if (first_changed != UINT32_MAX) {
          bindings_.ConstantRegistersWritten(index + first_changed, index + last_changed);
        }
      } break;
      case Block::kFetch: {
        // Only the changed ones, as WriteRegister: the texture cache reads a
        // slot again and the draws' textures are looked up again.
        uint32_t* values = register_file_->values + index;
        uint32_t first_changed = UINT32_MAX, last_changed = 0, changed = 0;
        for (uint32_t i = 0; i < part_count; ++i) {
          uint32_t value = rex::memory::load_and_swap<uint32_t>(part_base + i);
          if (values[i] != value) {
            values[i] = value;
            if (first_changed == UINT32_MAX) {
              first_changed = i;
            }
            last_changed = i;
            ++changed;
            ++fetch_constant_serials_[(index + i - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6];
          }
        }
        if (changed) {
          // Their constant buffer too (as WriteRegister, only for a change).
          bindings_.ConstantRegistersWritten(index + first_changed, index + last_changed);
          fetch_constant_changes_ += changed;
          if (texture_cache_) {
            uint32_t first_dword = index + first_changed - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
            uint32_t last_dword = index + last_changed - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
            texture_cache_->TextureFetchConstantsWritten(first_dword / 6, last_dword / 6);
          }
        }
      } break;
    }
    index = part_end;
  }
}

namespace {

// Formats whose resolve images hold what the texture cache's images of the
// same memory hold (the same values in the same host format): 16-bit fixed
// ones don't (the resolve image has the guest's range, the texture cache the
// stored /32 one). Depth: both R32_SFLOAT with the depth values (k_24_8 the
// unorm one, k_24_8_FLOAT the guest's [0, 2), the resolve pass undoing the
// host's half); a resolve's format follows its depth format, so a fetch of
// the other one doesn't match it.
bool IsComposableFormat(xenos::TextureFormat format) {
  switch (GetBaseFormat(format)) {
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    // Packed 16-bit: resolve images in the texture cache's host format
    // (ResolveDestVkFormat), components in the same order.
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

}  // namespace

VkImageView NativeCommandProcessor::GetScaledRenderTextureView(uint32_t fetch_constant,
                                                               uint64_t frame, bool full_only) {
  // The same answer as the last time for this fetch constant while nothing it
  // depends on changed: the overlap search runs over every resolve image, for
  // every 2D binding of every draw otherwise.
  NativeTextureCache::BoundTexture2D bound;
  bool bound_2d = texture_cache_->GetActiveBoundTexture2D(fetch_constant, bound, full_only);
  VkImageView bound_view = bound_2d ? bound.identity_view : VK_NULL_HANDLE;
  const uint32_t* fetch_words =
      &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6];
  uint64_t resolver_generation = resolver_.generation();
  ScaledViewMemo& memo = scaled_view_memos_[fetch_constant & 31];
  ++stats_scaled_view_lookups_;
  if (memo.frame == frame && memo.bound_view == bound_view &&
      !std::memcmp(memo.fetch, fetch_words, sizeof(memo.fetch)) &&
      IsScaledViewMemoCurrent(memo, resolver_generation) &&
      CanReuseDeferredScaledMemo(full_only, memo.full_only, memo.view != VK_NULL_HANDLE)) {
    if (full_only && !memo.full_coverage) return VK_NULL_HANDLE;
    if (memo.counted != kScaledFetchCount) {
      CountScaledFetch(memo.counted);
    }
    return memo.view;
  }
  scaled_fetch_last_counted_ = kScaledFetchCount;
  ++stats_scaled_view_computed_;
  bool full_coverage = false;
  VkImageView view = ComputeScaledRenderTextureView(fetch_constant, frame, bound_2d, bound,
                                                   full_only, full_coverage);
  memo.full_only = full_only;
  memo.full_coverage = full_coverage;
  memo.frame = frame;
  memo.resolver_generation = resolver_generation;
  memo.bound_view = bound_view;
  std::memcpy(memo.fetch, fetch_words, sizeof(memo.fetch));
  // The memory ComputeScaledRenderTextureView asked the resolver about.
  if (bound_2d) {
    memo.range_start = uint32_t(bound.key.base_page) << 12;
    memo.range_end = memo.range_start + bound.guest_size;
  } else {
    memo.range_start = uint32_t(register_file_->GetTextureFetch(fetch_constant).base_address)
                       << 12;
    memo.range_end = memo.range_start + 1;
  }
  memo.view = view;
  memo.counted = scaled_fetch_last_counted_;
  memo.serial = ++scaled_view_serial_;
  return view;
}

VkImageView NativeCommandProcessor::ComputeScaledRenderTextureView(
    uint32_t fetch_constant, uint64_t frame, bool bound_2d,
    const NativeTextureCache::BoundTexture2D& bound, bool full_only, bool& full_coverage) {
  full_coverage = false;
  if (!bound_2d) {
    // Counted only where a resolve wrote its first page (signed other than a
    // float, stacked, 3D, cube or not loaded), the rest being the game's own
    // textures.
    xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(fetch_constant);
    uint32_t fetch_base = uint32_t(fetch.base_address) << 12;
    resolver_.GetPlaceableOverlapping(fetch_base, fetch_base + 1, composite_overlapping_);
    if (!composite_overlapping_.empty()) {
      CountScaledFetch(kScaledFetchNotA2DTexture);
      if (!(scaled_fetch_logged_ & (1u << kScaledFetchNotA2DTexture))) {
        scaled_fetch_logged_ |= 1u << kScaledFetchNotA2DTexture;
        REXGPU_INFO(
            "Native renderer: a texture resolves wrote is sampled at 1x: not an unsigned 2D "
            "texture ({:08X}, {}, dimension {}, signs {}{}{}{})",
            fetch_base, FormatInfo::Get(xenos::TextureFormat(fetch.format))->name,
            uint32_t(fetch.dimension), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
            uint32_t(fetch.sign_z), uint32_t(fetch.sign_w));
      }
    }
    return VK_NULL_HANDLE;
  }
  // One level, tiled as resolves write, in a format both hold alike.
  const auto& key = bound.key;
  uint32_t base = uint32_t(key.base_page) << 12;
  if (!key.tiled || key.mip_max_level) {
    resolver_.GetPlaceableOverlapping(base, base + bound.guest_size, composite_overlapping_);
    if (!composite_overlapping_.empty()) {
      CountScaledFetch(kScaledFetchLayout);
      if (!(scaled_fetch_logged_ & (1u << kScaledFetchLayout))) {
        scaled_fetch_logged_ |= 1u << kScaledFetchLayout;
        REXGPU_INFO(
            "Native renderer: a texture resolves wrote is sampled at 1x: {} ({:08X}, {}x{}, {}, "
            "{} mip levels)",
            key.tiled ? "it has mips" : "it is linear", base, key.GetWidth(), key.GetHeight(),
            FormatInfo::Get(key.format)->name, uint32_t(key.mip_max_level) + 1);
      }
    }
    return VK_NULL_HANDLE;
  }
  if (!IsComposableFormat(key.format) || !composites_.CanCompose(bound.format)) {
    // Only of interest where a resolve wrote it.
    resolver_.GetPlaceableOverlapping(base, base + bound.guest_size, composite_overlapping_);
    if (!composite_overlapping_.empty()) {
      CountScaledFetch(kScaledFetchFormat);
      if (!(scaled_fetch_logged_ & (1u << kScaledFetchFormat))) {
        scaled_fetch_logged_ |= 1u << kScaledFetchFormat;
        // Which resolve wrote it: for 16-bit fixed textures, whether the image
        // and the texture cache hold the same values depends on its source.
        const ResolvedTexture& piece = *composite_overlapping_.back();
        REXGPU_INFO(
            "Native renderer: a texture resolves wrote is sampled at 1x: format {} (host {}) "
            "is not composed ({:08X}, {}x{}); written by a resolve of a {} format {} surface{}, "
            "number format {}, image host format {}",
            FormatInfo::Get(key.format)->name, int32_t(bound.format), base, key.GetWidth(),
            key.GetHeight(), piece.from_depth ? "depth" : "colour", piece.source_format,
            piece.source_fixed16 ? " (16-bit fixed)" : "", piece.dest_number,
            int32_t(piece.vk_format));
      }
    }
    return VK_NULL_HANDLE;
  }
  resolver_.GetPlaceableOverlapping(base, base + bound.guest_size, composite_overlapping_);
  if (composite_overlapping_.empty()) {
    // The game's own textures: not counted.
    return VK_NULL_HANDLE;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t bpp_log2 = 0;
  while (bytes_per_block > (1u << bpp_log2)) {
    ++bpp_log2;
  }
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  uint32_t pitch_aligned = uint32_t(key.pitch) << 5;
  uint32_t height_aligned = (height + 31) & ~uint32_t(31);
  const uint32_t scale4 = render_targets_.scale4();
  // Each resolve's rectangles where they are in this texture (the same
  // layout and format proven), oldest first so newer ones are drawn over.
  composite_pieces_.clear();
  bool covered = false;
  bool any_mismatch = false, any_not_placed = false;
  // Every piece's host source holds its whole host destination: the condition
  // for taking several pieces together as covering the texture.
  bool all_hosts_valid = true;
  for (ResolvedTexture* texture : composite_overlapping_) {
    if (texture->scale4 != scale4 || texture->vk_format != bound.format ||
        GetBaseFormat(texture->dest_texture_format) != GetBaseFormat(key.format) ||
        texture->dest_pitch_aligned != pitch_aligned || texture->dest_bpp_log2 != bpp_log2 ||
        uint32_t(texture->dest_endian) != uint32_t(key.endianness)) {
      any_mismatch = true;
      if (!(scaled_fetch_logged_ & (1u << kScaledFetchResolveMismatch))) {
        scaled_fetch_logged_ |= 1u << kScaledFetchResolveMismatch;
        REXGPU_INFO(
            "Native renderer: a resolve into a sampled texture doesn't match it, 1x there: "
            "texture {:08X} {}x{} {} pitch {} endian {} host {}; resolve to {:08X} {} pitch {} "
            "texel 2^{} endian {} host {} scale4 {}",
            base, width, height, FormatInfo::Get(key.format)->name, pitch_aligned,
            uint32_t(key.endianness), int32_t(bound.format), texture->dest_texture_base,
            FormatInfo::Get(texture->dest_texture_format)->name, texture->dest_pitch_aligned,
            texture->dest_bpp_log2, uint32_t(texture->dest_endian), int32_t(texture->vk_format),
            texture->scale4);
      }
      continue;
    }
    uint32_t tile_x, tile_y;
    if (!FindMacroTileInTexture(base, texture->dest_texture_base, pitch_aligned, height_aligned,
                                bpp_log2, tile_x, tile_y)) {
      any_not_placed = true;
      if (!(scaled_fetch_logged_ & (1u << kScaledFetchNotPlaced))) {
        scaled_fetch_logged_ |= 1u << kScaledFetchNotPlaced;
        REXGPU_INFO(
            "Native renderer: a resolve into a sampled texture is not on its macro tiles, 1x "
            "there: texture {:08X} {}x{} pitch {}; resolve origin {:08X} x offset {}",
            base, width, height, pitch_aligned, texture->dest_texture_base,
            texture->dest_x_offset);
      }
      continue;
    }
    int32_t offset_x = int32_t(tile_x + texture->dest_x_offset);
    int32_t offset_y = int32_t(tile_y);
    for (const Rect& written : texture->written_rects) {
      // In the texture, clipped to it.
      Rect dest = {written.left + offset_x, written.top + offset_y, written.right + offset_x,
                   written.bottom + offset_y};
      Rect clipped = {std::max(dest.left, 0), std::max(dest.top, 0),
                      std::min(dest.right, int32_t(width)), std::min(dest.bottom, int32_t(height))};
      if (clipped.empty()) {
        continue;
      }
      Composites::Piece piece;
      piece.texture = texture;
      piece.source = {written.left + (clipped.left - dest.left),
                      written.top + (clipped.top - dest.top),
                      written.right - (dest.right - clipped.right),
                      written.bottom - (dest.bottom - clipped.bottom)};
      piece.dest_x = clipped.left;
      piece.dest_y = clipped.top;
      const Rect source_host = ScaleRect(piece.source, scale4);
      const Rect dest_host = ScaleRect(clipped, scale4);
      all_hosts_valid &= source_host.left >= 0 && source_host.top >= 0 &&
                         source_host.right <= int32_t(texture->host_width) &&
                         source_host.bottom <= int32_t(texture->host_height) &&
                         source_host.right - source_host.left >= dest_host.right - dest_host.left &&
                         source_host.bottom - source_host.top >= dest_host.bottom - dest_host.top;
      composite_pieces_.push_back(piece);
      covered |= clipped.left == 0 && clipped.top == 0 && clipped.right == int32_t(width) &&
                 clipped.bottom == int32_t(height);
    }
  }
  // No single piece covers the texture, several may together: predicated
  // tiling resolves 1280x720 as 1280x512 at the texture's base and 1280x208
  // one macro tile row 16 further (RDR: depth, HDR colour and two colour
  // textures every frame). Then the composite needs no 1x base under them.
  // Only with every piece's host source whole, the texture's host edges exact
  // and no resolve mismatched or unplaced: without a base a hole would stay
  // uncleared.
  bool covered_by_union = false;
  if (!covered && composite_pieces_.size() > 1 && all_hosts_valid && !any_mismatch &&
      !any_not_placed &&
      ScaleEdge(int32_t(width), scale4) == int32_t(ScaleSize(width, scale4)) &&
      ScaleEdge(int32_t(height), scale4) == int32_t(ScaleSize(height, scale4))) {
    composite_cover_rects_.clear();
    for (const Composites::Piece& piece : composite_pieces_) {
      composite_cover_rects_.push_back(
          {piece.dest_x, piece.dest_y, piece.dest_x + (piece.source.right - piece.source.left),
           piece.dest_y + (piece.source.bottom - piece.source.top)});
    }
    covered = covered_by_union = RectsCoverTexture(composite_cover_rects_, width, height);
  }
  if (bound.guest_size >= (1u << 20)) {
    ++stats_large_coverage_[std::min<size_t>(composite_pieces_.size(), 3)];
    if (covered) ++stats_large_coverage_[covered_by_union ? 5 : 4];
  }
  if (composite_pieces_.size() == 1 && !any_mismatch && !any_not_placed && all_hosts_valid) {
    const auto& piece = composite_pieces_.front();
    const Rect destination{piece.dest_x, piece.dest_y,
                           piece.dest_x + piece.source.right - piece.source.left,
                           piece.dest_y + piece.source.bottom - piece.source.top};
    full_coverage = DeferredSingleResolveCovers(piece.source, destination, width, height,
                                               piece.texture->host_width, piece.texture->host_height,
                                               scale4);
  }
  if (full_only && !full_coverage) {
    // Never form a composite which reads an uninitialized/stale ordinary base.
    // The eager retry may use the existing, more general partial/union path.
    return VK_NULL_HANDLE;
  }
  if (composite_pieces_.empty()) {
    CountScaledFetch(any_mismatch     ? kScaledFetchResolveMismatch
                     : any_not_placed ? kScaledFetchNotPlaced
                                      : kScaledFetchOutside);
    return VK_NULL_HANDLE;
  }
  Composites::Key composite_key;
  composite_key.base = base;
  composite_key.width = width;
  composite_key.height = height;
  composite_key.format = bound.format;
  VkImageView view =
      composites_.Get(composite_key, scale4, covered ? VK_NULL_HANDLE : bound.identity_view,
                      composite_pieces_, resolver_,
                      texture_cache_->GetActiveTextureHostSwizzle(fetch_constant), frame);
  CountScaledFetch(view != VK_NULL_HANDLE ? kScaledFetchServed : kScaledFetchComposeFailed);
  return view;
}

void NativeCommandProcessor::LogScaledFetchSummary() {
  const auto& c = scaled_fetch_counts_;
  uint64_t total = 0;
  for (uint64_t count : c) {
    total += count;
  }
  if (total) {
    REXGPU_INFO(
        "Native renderer: textures resolves wrote, fetches over 300 frames: {} at the scale; at "
        "1x: {} not an unsigned 2D texture, {} mips or linear, {} format, {} resolve layout "
        "differs, {} resolve not on its tiles, {} outside, {} compose failed",
        c[kScaledFetchServed], c[kScaledFetchNotA2DTexture], c[kScaledFetchLayout],
        c[kScaledFetchFormat], c[kScaledFetchResolveMismatch], c[kScaledFetchNotPlaced],
        c[kScaledFetchOutside], c[kScaledFetchComposeFailed]);
  }
  scaled_fetch_counts_ = {};
}

bool NativeCommandProcessor::IsResolvedTextureFetchable(const ResolvedTexture& texture,
                                                        const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (fetch.dimension != xenos::DataDimension::k1D &&
      fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    return false;
  }
  // The host image holds the render target's values, not the guest's bits:
  // only a fetch reading them as such samples it. Any other (another format of
  // the same texel size, depth read as colour bytes to rebuild it in the
  // shader) reads the guest's bits the resolve wrote into memory.
  xenos::TextureFormat fetch_format = GetBaseFormat(xenos::TextureFormat(fetch.format));
  if (texture.from_depth) {
    // Depth as floats, as the texture cache holds k_24_8 / k_24_8_FLOAT.
    return fetch_format == (texture.depth_float24 ? xenos::TextureFormat::k_24_8_FLOAT
                                                  : xenos::TextureFormat::k_24_8);
  }
  return fetch_format == ColorFormatToTextureFormat(texture.guest_format);
}

void NativeCommandProcessor::PresentFrame(ui::Presenter* presenter, uint32_t frontbuffer_ptr,
                                          uint32_t frontbuffer_width,
                                          uint32_t frontbuffer_height) {
  // Deferred PublishGuestOutput advances the presenter's writable mailbox on
  // the submission thread. Before starting another refresh, that publication
  // (and its refresh-completion fence signal) must have run. Otherwise a fast
  // CPU frame, particularly one skipped for a pending pipeline, can resize and
  // destroy the same mailbox image while its old commands await replay.
  // Await only the publication callback, not the subsequent host paint. That
  // may block in swapchain acquisition/presentation and should overlap CPU
  // preparation of the next frame. Full callback draining remains required
  // when detaching the presenter and shutting down.
  submitter_.AwaitCallback(present_publication_callback_);
  // The texture the front buffer was resolved to; else the last large colour
  // resolve if it was in this frame (AoT's fallback: the front buffer may be
  // resolved to elsewhere); else the front buffer in guest memory (drawn by
  // the CPU: loading screens, videos) through the texture cache; else the
  // last large resolve. Its part the size of the packet's, in host pixels;
  // black of the packet's size without one.
  ResolvedTexture* texture = resolver_.Find(frontbuffer_ptr);
  VkImageView memory_view = VK_NULL_HANDLE;
  uint32_t memory_width = 0;
  uint32_t memory_height = 0;
  xenos::TextureFormat memory_format = xenos::TextureFormat::k_8_8_8_8;
  if (!texture) {
    ResolvedTexture* last_resolve = resolver_.present_address() != UINT32_MAX
                                        ? resolver_.Find(resolver_.present_address())
                                        : nullptr;
    if (!last_resolve || last_resolve->last_frame != frame_) {
      texture_cache_->UpdateSubmissions();
      memory_view =
          texture_cache_->RequestSwapTexture(memory_width, memory_height, memory_format);
    }
    if (memory_view == VK_NULL_HANDLE) {
      texture = last_resolve;
      if (texture && !frontbuffer_miss_logged_) {
        frontbuffer_miss_logged_ = true;
        REXGPU_INFO(
            "Native renderer: front buffer {:08X} ({}x{}) not resolved to, presenting {:08X}",
            frontbuffer_ptr, frontbuffer_width, frontbuffer_height, texture->address);
      }
    }
  }
  Present::Source source;
  uint32_t output_width = std::max(frontbuffer_width, uint32_t(1));
  uint32_t output_height = std::max(frontbuffer_height, uint32_t(1));
  // The frame in guest pixels (frame generation maps the scene into it).
  uint32_t guest_width = output_width, guest_height = output_height;
  if (memory_view != VK_NULL_HANDLE) {
    output_width = std::max(
        frontbuffer_width ? std::min(frontbuffer_width, memory_width) : memory_width, uint32_t(1));
    output_height =
        std::max(frontbuffer_height ? std::min(frontbuffer_height, memory_height) : memory_height,
                 uint32_t(1));
    source.view = memory_view;
    source.width = output_width;
    source.height = output_height;
    guest_width = output_width;
    guest_height = output_height;
    source.pwl_gamma_ramp = memory_format == xenos::TextureFormat::k_2_10_10_10 ||
                            memory_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
  } else if (texture) {
    uint32_t width = frontbuffer_width ? std::min(frontbuffer_width, texture->width)
                                       : texture->width;
    uint32_t height = frontbuffer_height ? std::min(frontbuffer_height, texture->height)
                                         : texture->height;
    guest_width = std::max(width, uint32_t(1));
    guest_height = std::max(height, uint32_t(1));
    output_width = std::max(std::min(ScaleSize(width, texture->scale4), texture->host_width),
                            uint32_t(1));
    output_height = std::max(std::min(ScaleSize(height, texture->scale4), texture->host_height),
                             uint32_t(1));
    source.view = texture->view;
    source.width = output_width;
    source.height = output_height;
    source.pwl_gamma_ramp =
        texture->guest_format == xenos::ColorFormat::k_2_10_10_10 ||
        texture->guest_format == xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16;
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(video_mode.display_width), uint32_t(1));
  uint32_t display_height = std::max(uint32_t(video_mode.display_height), uint32_t(1));

  // swap_post_effect, as the SDK's Vulkan backend applies it.
  Present::PostEffect post_effect = Present::PostEffect::kNone;
  switch (GetActualSwapPostEffect()) {
    case SwapPostEffect::kFxaa:
      post_effect = Present::PostEffect::kFxaa;
      break;
    case SwapPostEffect::kFxaaExtreme:
      post_effect = Present::PostEffect::kFxaaExtreme;
      break;
    case SwapPostEffect::kSmaa:
      post_effect = Present::PostEffect::kSmaa;
      break;
    default:
      break;
  }
  // A temporal upscaler already antialiased the scene: a second, spatial AA
  // pass would only blur it (and the HUD). Logged on each change.
  // Only while views arrive: menus and a producer switched off get it back.
  const bool temporal =
      scene_frames_ && scene_frames_->UpscalerActive() && scene_swaps_since_view_ <= 30;
  if (temporal != scene_temporal_post_skip_logged_) {
    scene_temporal_post_skip_logged_ = temporal;
    if (temporal && post_effect != Present::PostEffect::kNone)
      REXGPU_INFO("Temporal upscaler active: swap_post_effect skipped while it runs");
    else if (!temporal)
      REXGPU_INFO("Temporal upscaler inactive: swap_post_effect applies again");
  }
  if (temporal) post_effect = Present::PostEffect::kNone;

  presenter->RefreshGuestOutput(
      output_width, output_height, display_width, display_height,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        CommandRecorder* command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kPresent);
        if (command_buffer == nullptr) {
          return false;
        }
        if (texture) {
          resolver_.Transition(command_buffer, *texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        auto& vulkan_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        bool recorded =
            present_.Record(vulkan_context, output_width, output_height, source,
                            gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(), post_effect);
        // Frame generation (the presenter's): the ended frame's depth and
        // motion, and the frame without the HUD through the same gamma pass.
        const auto* fg_targets = vulkan_context.frame_generation();
        if (recorded && fg_targets && fg_targets->inputs && scene_frames_) {
          SceneFrames::HudlessSource hudless;
          if (scene_frames_->WriteFrameGeneration(*fg_targets, output_width, output_height,
                                                  guest_width, guest_height, hudless)) {
            Present::Source hudless_source = source;
            hudless_source.view = hudless.view;
            hudless_source.width = hudless.width;
            hudless_source.height = hudless.height;
            bool hudless_8bpc = false;
            ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext hudless_context(
                hudless_8bpc, fg_targets->hudless, fg_targets->hudless_view,
                fg_targets->hudless_version, false);
            if (present_.Record(hudless_context, output_width, output_height, hudless_source,
                                gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(), post_effect)) {
              scene_frames_->FillFrameGenerationInputs(*fg_targets->inputs);
              fg_targets->inputs->valid = true;
            }
          }
        }
        // The presenter signals its fence right after the refresher returns:
        // the work must be on the queue by then.
        return submitter_.Submit() && recorded;
      });
}

bool NativeCommandProcessor::IssueCopy() {
  if (context_ready_) {
    const bool timed = frame_stats_enabled();
    uint64_t start = timed ? rex::chrono::Clock::QueryHostTickCount() : 0;
    uint32_t written_start, written_length;
    resolver_.Resolve(*register_file_, *memory_, frame_, written_start, written_length);
    if (auto* commands = submitter_.open_command_buffer()) {
      // With the resolve's own commands and whatever came since the last draw.
      ui::GpuBreadcrumbDetails details;
      if (GetVulkanDevice()->HasBreadcrumbs()) {
        details.work = commands->TakeBreadcrumbWork();
        details.filled = true;
      }
      commands->CmdBreadcrumb(GetVulkanDevice()->RecordBreadcrumb(
          {0, submitter_.current(), frame_ + 1, 0, 0, 0, true, details}));
    }
    uint64_t resolved = timed ? rex::chrono::Clock::QueryHostTickCount() : 0;
    if (written_length) {
      ReadbackResolve(written_start, written_length);
    }
    if (timed) {
      stats_copy_resolve_ticks_ += resolved - start;
      stats_copy_readback_ticks_ += rex::chrono::Clock::QueryHostTickCount() - resolved;
    }
  }
  return true;
}

void NativeCommandProcessor::ReadbackResolve(uint32_t written_start, uint32_t written_length) {
  ReadbackResolveMode mode = GetReadbackResolveMode(false);
  if (mode == ReadbackResolveMode::kDisabled) {
    return;
  }
  if (mode != ReadbackResolveMode::kFull) {
    LogDrawOnce(kDrawLogReadbackMode,
                "readback_resolve fast and some are done as full (no data from the previous "
                "frame; the GPU is waited for at the game's first read of a resolve, or at the "
                "resolve without readback_resolve_full_deferred)");
  }
  if (!memory_->TranslatePhysical(written_start)) {
    return;
  }
  uint32_t pixel_size_log2;
  uint32_t length =
      GetResolveReadbackLength(written_start, written_length,
                               register_file_->Get<reg::RB_COPY_DEST_INFO>(), false,
                               pixel_size_log2);
  if (!length) {
    return;
  }
  // Only what the guest's CPU accesses (readback_resolve_cpu_reads_only); the
  // rest is watched, and ReadBackResolvesNow copies it at the first access.
  if (!ShouldReadbackResolve(written_start, length,
                             GetReadbackResolveKey(written_start, written_length),
                             pixel_size_log2, false)) {
    return;
  }
  readback_ranges_.assign(1, {written_start, length});
  // readback_resolve_full_deferred: the copy recorded now, the GPU waited for
  // only when the guest reads it (CompleteDeferredReadbacks), usually long
  // done by then. Not deferred: waited for now.
  static const std::vector<std::pair<uint32_t, uint32_t>> kNoPieces;
  if (readback_.RecordDeferred(written_start, length)) {
    if (DeferResolveReadback(written_start, length, true, kNoPieces)) {
      return;
    }
    readback_.CompleteDeferred(readback_ranges_.data(), 1, readback_data_);
  } else {
    readback_.ReadNow(readback_ranges_.data(), 1, readback_data_);
  }
  if (readback_data_[0]) {
    CopyResolveToGuest(written_start, length, readback_data_[0]);
  }
}

void NativeCommandProcessor::CompleteDeferredReadbacks(const ResolveFetch* fetches, size_t count,
                                                       std::vector<const uint8_t*>& data_out) {
  if (!context_ready_) {
    data_out.assign(count, nullptr);
    return;
  }
  readback_ranges_.clear();
  for (size_t i = 0; i < count; ++i) {
    readback_ranges_.emplace_back(fetches[i].address, fetches[i].length);
  }
  readback_.CompleteDeferred(readback_ranges_.data(), count, data_out);
}

void NativeCommandProcessor::ReadBackResolvesNow(const ResolveFetch* fetches, size_t count,
                                                 std::vector<const uint8_t*>& data_out) {
  if (!context_ready_) {
    data_out.assign(count, nullptr);
    return;
  }
  // Scale 1: no downscale, the bytes as the resolves wrote them.
  readback_ranges_.clear();
  for (size_t i = 0; i < count; ++i) {
    readback_ranges_.emplace_back(fetches[i].address, fetches[i].length);
  }
  readback_.ReadNow(readback_ranges_.data(), count, data_out);
}

bool NativeCommandProcessor::RequestVertexBuffers(const Shader& shader) {
  const RegisterFile& regs = *register_file_;
  const Shader::ConstantRegisterMap& constant_map = shader.constant_register_map();
  for (uint32_t i = 0; i < uint32_t(rex::countof(constant_map.vertex_fetch_bitmap)); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      if (vfetch_constant.type != xenos::FetchConstantType::kVertex &&
          !REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
        LogDrawOnce(kDrawLogFetchConstant,
                    "a draw reads a fetch constant that is not a vertex one, skipped");
        return false;
      }
      // Physical, as the shader masks it: the guest may use a mirror window.
      if (!shared_memory_->RequestRange(xenos::CpuToGpu(vfetch_constant.address << 2),
                                        vfetch_constant.size << 2,
                                        SharedMemory::RequestKind::kVertex)) {
        REXGPU_ERROR("Native renderer: failed to request vertex data at {:08X} ({} bytes)",
                     vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      draw_read_ranges_.emplace_back(xenos::CpuToGpu(vfetch_constant.address << 2),
                                     vfetch_constant.size << 2);
    }
  }
  return true;
}

namespace {

// A Direct3D 9 style clear on the console (himdo's Fable 2 SDK, 80e0b5c): a
// three-vertex rectangle list with auto indices and clipping off, its corners
// in the first vertex binding as screen pixels, from the top left. Its right
// and bottom edges: the draw replaces whole only the tiles inside them (and
// the scissor), the claim brings the rest in as usual. himdo's version took a
// rectangle up to 80 pixels short of the pitch as covering it: the columns it
// left got stale memory instead of the tiles' earlier contents (Fuse, pitch
// 1152: a 32-pixel strip at the right edge, fixed by 0811). False when the
// draw isn't one.
struct ClearRectangle {
  float right = 0.0f;
  float bottom = 0.0f;
};
bool GetClearRectangle(const RegisterFile& regs, const memory::Memory& memory,
                       const Shader& vertex_shader, ClearRectangle& rectangle_out) {
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex || initiator.num_indices != 3 ||
      !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable || regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] ||
      vertex_shader.vertex_bindings().empty()) {
    return false;
  }
  const Shader::VertexBinding& binding = vertex_shader.vertex_bindings()[0];
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.endian != xenos::Endian::k8in32 ||
      binding.stride_words < 2 || fetch.size < binding.stride_words * 2 + 2) {
    return false;
  }
  const uint8_t* vertices = memory.TranslatePhysical(fetch.address << 2);
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  for (uint32_t i = 0; i < 3; ++i) {
    float position[2];
    for (uint32_t axis = 0; axis < 2; ++axis) {
      uint32_t raw;
      std::memcpy(&raw, vertices + (i * binding.stride_words + axis) * sizeof(uint32_t),
                  sizeof(raw));
      raw = rex::byte_swap(raw);
      std::memcpy(&position[axis], &raw, sizeof(raw));
      if (!std::isfinite(position[axis])) {
        return false;
      }
    }
    min_x = std::min(min_x, position[0]);
    max_x = std::max(max_x, position[0]);
    min_y = std::min(min_y, position[1]);
    max_y = std::max(max_y, position[1]);
  }
  if (min_x > 0.5f || min_y > 0.5f || max_x < 0.5f || max_y < 0.5f) {
    return false;
  }
  rectangle_out.right = max_x;
  rectangle_out.bottom = max_y;
  return true;
}

}  // namespace

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  const RegisterFile& regs = *register_file_;
  // VIZ survey geometry (occlusion_query_viz): drawn without any writes (the
  // normalized state has none) to count its ID's coverage in the query
  // segment. One not drawn, whatever the reason, isn't measured: the query
  // falls back to visible.
  const bool viz_survey = draw_util::IsVIZSurveyDraw(regs);
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    if (viz_survey) {
      OnVIZSurveyDraw(false);
      return true;
    }
    return IssueCopy();
  }
  struct VIZSurveyFallback {
    NativeCommandProcessor* command_processor;
    bool pending;
    ~VIZSurveyFallback() {
      if (pending) {
        command_processor->OnVIZSurveyDraw(false);
      }
    }
  } viz_survey_fallback{this, viz_survey};
  if (!context_ready_) {
    return true;
  }
  // On integrated GPUs, at most so many draws in one submission
  // (vulkan_submission_draw_limit, as the Vulkan backend): the GPU starts on
  // the frame earlier and no single submission can run into the Windows 2 s
  // GPU timeout. Submitting between draws ends the render pass and the query
  // segment, as OnPrimaryBufferEnd's submissions do.
  if (draws_in_submission_of_ != submitter_.current()) {
    draws_in_submission_of_ = submitter_.current();
    draws_in_submission_ = 0;
  }
  if (submitter_.is_open() &&
      vulkan::submission_watch::ShouldSplit(
          draws_in_submission_,
          vulkan::submission_watch::DrawLimit(REXCVAR_GET(vulkan_submission_draw_limit),
                                              integrated_gpu_)) &&
      submitter_.Submit()) {
    ++stats_submission_splits_;
    draws_in_submission_of_ = submitter_.current();
    draws_in_submission_ = 0;
  }
  ++draws_in_submission_;
  DrawPhaseTimer<DrawPhase> phase_timer(
      stats_draw_phase_ticks_,
      frame_stats_enabled() && stats_draw_phase_sample_++ % kDrawPhaseSampleRate == 0,
      DrawPhase::kShaders);

  // Shaders, as the SDK's Vulkan backend chooses them.
  auto* vertex_shader = static_cast<vulkan::VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    return true;
  }
  bool memexport_used = vertex_shader->memexport_eM_written() != 0;
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (rasterization_done && !regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    return true;
  }
  vulkan::VulkanShader* pixel_shader = nullptr;
  if (rasterization_done) {
    // Only with color and depth (xenos::EdramMode).
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<vulkan::VulkanShader*>(active_pixel_shader());
      if (pixel_shader && !draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
        pixel_shader = nullptr;
      }
    }
  } else if (!memexport_used) {
    // Nothing would be written.
    return true;
  }
  memexport_used |= pixel_shader && pixel_shader->memexport_eM_written();
  if (defer_full_resolve_loads_ && memexport_used && !defer_disabled_by_memexport_) {
    // Resolver generations do not describe arbitrary memory-export writes.
    // Remain conservative for every subsequent draw in this context too.
    defer_disabled_by_memexport_ = true;
    REXGPU_INFO("Native renderer: full-resolve texture deferral disabled after memexport");
  }
  // No sample passes and nothing is exported: no effect (not a VIZ survey,
  // whose result is measured).
  if (rasterization_done && !memexport_used && !viz_survey &&
      draw_util::IsDepthTestNeverPassing(regs)) {
    return true;
  }
  // A draw under a VIZ ID whose pixels may be rejected by the shader (kill,
  // alpha to coverage) counts at hi-Z on the console, before that: not
  // measurable here, the query falls back to visible.
  if (!viz_survey && pixel_shader &&
      (pixel_shader->kills_pixels() ||
       (pixel_shader->writes_color_target(0) &&
        draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>())))) {
    OnVIZSurveyDraw(false);
  }
  // Memory export: where the shaders write in the shared memory (from the
  // stream constants), as the SDK's Vulkan backend finds it.
  memexport_ranges_.clear();
  if (memexport_used) {
    const ui::vulkan::VulkanDevice::Properties& properties = GetVulkanDevice()->properties();
    if (vertex_shader->memexport_eM_written()) {
      if (!properties.vertexPipelineStoresAndAtomics) {
        LogDrawOnce(kDrawLogMemExport,
                    "the GPU can't store from vertex shaders (vertexPipelineStoresAndAtomics): "
                    "draws with memory export are skipped");
        return true;
      }
      draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
    }
    if (pixel_shader && pixel_shader->memexport_eM_written()) {
      if (!properties.fragmentStoresAndAtomics) {
        LogDrawOnce(kDrawLogMemExport,
                    "the GPU can't store from pixel shaders (fragmentStoresAndAtomics): draws "
                    "with memory export are skipped");
        return true;
      }
      draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
    }
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  // Indices (the guest's in the shared memory, or converted ones).
  phase_timer.Next(DrawPhase::kIndices);
  primitive_processor_->UploadBuiltinIndexBufferIfNeeded();
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  phase_timer.Next(DrawPhase::kShaders);
  // Tessellated: the guest's vertex shader runs as the evaluation shader,
  // after the host's vertex and control shaders, as on the SDK's Vulkan
  // backend.
  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  uint32_t patch_control_points = 0;
  if (primitive_processing_result.IsTessellated()) {
    if (!pipelines_.tessellation_supported()) {
      LogDrawOnce(kDrawLogTessellation,
                  "the GPU has no tessellation shaders: tessellated draws are skipped");
      return true;
    }
    xenos::TessellationMode tessellation_mode = primitive_processing_result.tessellation_mode;
    patch_control_points = vulkan::TessellationInputControlPoints(
        primitive_processing_result.host_vertex_shader_type, tessellation_mode);
    tessellation_vertex_shader = pipelines_.GetTessellationVertexShader(
        tessellation_mode == xenos::TessellationMode::kAdaptive);
    tessellation_control_shader = pipelines_.GetTessellationControlShader(
        primitive_processing_result.host_vertex_shader_type, tessellation_mode);
    if (!patch_control_points || tessellation_vertex_shader == VK_NULL_HANDLE ||
        tessellation_control_shader == VK_NULL_HANDLE) {
      LogDrawOnce(kDrawLogTessellation,
                  "a tessellated draw of a kind not supported is skipped");
      return true;
    }
  }

  // Colour written where the pixel shader writes and the mask lets it.
  uint32_t color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  // Scene render scale (native_scene_upscale): in the scene interval a draw
  // into a scene surface (depth or stencil tested on one, or a colour target
  // marked) runs at the scene scale with all of its targets, which become the
  // scene's (their resolves follow). Everything else stays at the scale.
  uint32_t draw_scale4 = scale4_;
  if (render_targets_.scene_interval() && rasterization_done) {
    const auto scene_surface = regs.Get<reg::RB_SURFACE_INFO>();
    uint64_t scene_keys[xenos::kMaxColorRenderTargets + 1];
    uint32_t scene_key_count = 0;
    bool scene = false;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!((color_mask >> (i * 4)) & 0b1111)) {
        continue;
      }
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      const uint64_t key = RenderTargets::SurfaceKey(
          info.color_base, uint32_t(KeyColorFormat(info.color_format)), false,
          scene_surface.surface_pitch, scene_surface.msaa_samples);
      scene |= render_targets_.IsScene(key);
      scene_keys[scene_key_count++] = key;
    }
    if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable) {
      auto info = regs.Get<reg::RB_DEPTH_INFO>();
      const uint64_t key =
          RenderTargets::SurfaceKey(info.depth_base, uint32_t(info.depth_format), true,
                                    scene_surface.surface_pitch, scene_surface.msaa_samples);
      scene |= render_targets_.IsScene(key);
      scene_keys[scene_key_count++] = key;
    }
    if (scene) {
      draw_scale4 = render_targets_.scene_scale4();
      for (uint32_t i = 0; i < scene_key_count; ++i) {
        render_targets_.MarkScene(scene_keys[i]);
      }
    }
  }
  // The depth bias in the pixel shader where the host's is inexact or the
  // draw is a coplanar redraw (depth_bias_shader_offset, as the SDK's Vulkan
  // backend), the fixed-function one zeroed then.
  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();
  const bool depth_bias_force_unorm = IsDepthBiasForceUnorm(device_properties);
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
  const bool polygon_offset_in_shader =
      pixel_shader && !pixel_shader->writes_depth() && REXCVAR_GET(native_depth_bias) &&
      draw_util::GetHostDepthPolygonOffsetIfNeeded(regs, primitive_polygonal,
                                                   normalized_depth_control, color_mask,
                                                   depth_bias_force_unorm,
                                                   host_depth_polygon_offset);
  if (polygon_offset_in_shader) {
    // The slope is per host pixel, as the SDK's Vulkan backend scales it.
    host_depth_polygon_offset.front_scale *= float(draw_scale4) * 0.25f;
    host_depth_polygon_offset.back_scale *= float(draw_scale4) * 0.25f;
  }

  // Translations for this draw's state.
  SpirvShaderTranslator::Modification vertex_shader_modification = shaders_.GetVertexModification(
      regs, *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
      ps_param_gen_pos != UINT32_MAX);
  // depth_float24_convert_in_pixel_shader: the guest's float24 depth exactly
  // (Vulkan only differs from D3D12 there: PDZ's light shafts under lamps break
  // off without it); per-sample shading is needed for it with MSAA.
  const bool depth_float24_convert =
      REXCVAR_GET(depth_float24_convert_in_pixel_shader) && device_properties.sampleRateShading;
  SpirvShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? shaders_.GetPixelModification(regs, *pixel_shader, interpolator_mask,
                                                   ps_param_gen_pos, normalized_depth_control,
                                                   polygon_offset_in_shader, depth_float24_convert)
                   : SpirvShaderTranslator::Modification(0);
  if (pixel_shader && draw_scale4 != scale4_) {
    // Pixel positions and memory export at 1x (the scene scale is 1x only).
    pixel_shader_modification.pixel.resolution_scale_native = 1;
  }
  // In the background with async_shader_compilation, as the pipelines below
  // (depth-only ones with async_depth_only_pipelines): the draw is skipped
  // until they are done. Never for memory export, whose writes would be lost.
  const bool async_translation =
      REXCVAR_GET(async_shader_compilation) && !memexport_used &&
      (pixel_shader != nullptr || REXCVAR_GET(async_depth_only_pipelines));
  // A draw to a surface at most 2 tiles wide (data kept past the frame, like
  // eye adaptation history) may never be redone, so it waits until its shaders
  // and pipeline are created rather than being skipped (xenia-canary f9431eb95).
  const bool async_must_wait =
      memexport_used || draw_util::IsAsyncPipelineWaitNeededForDraw(regs);
  const uint32_t async_wait_ms = uint32_t(std::max(
      PipelineWaitBudgetMs(async_must_wait, REXCVAR_GET(async_pipeline_wait_ms)), int32_t(0)));
  bool vertex_translation_pending = false, pixel_translation_pending = false;
  // The last ready translation of the same shader and modification as it is.
  auto translate = [&](uint32_t stage, vulkan::VulkanShader& shader, uint64_t modification,
                       bool& pending) -> vulkan::VulkanShader::VulkanTranslation* {
    TranslationMemo& memo = translation_memos_[stage];
    if (memo.shader == &shader && memo.modification == modification && memo.translation) {
      return memo.translation;
    }
    vulkan::VulkanShader::VulkanTranslation* translation =
        shaders_.Translate(shader, modification, async_translation, async_wait_ms, &pending);
    if (translation) {
      memo.shader = &shader;
      memo.modification = modification;
      memo.translation = translation;
    }
    return translation;
  };
  vulkan::VulkanShader::VulkanTranslation* vertex_translation = translate(
      0, *vertex_shader, vertex_shader_modification.value, vertex_translation_pending);
  vulkan::VulkanShader::VulkanTranslation* pixel_translation =
      pixel_shader ? translate(1, *pixel_shader, pixel_shader_modification.value,
                               pixel_translation_pending)
                   : nullptr;
  if (!vertex_translation || (pixel_shader && !pixel_translation)) {
    // Being translated: skipped, the frame held back as for pipelines. Else
    // logged by the translation.
    frame_used_pending_pipeline_ |= vertex_translation_pending || pixel_translation_pending;
    return true;
  }

  // Vertex data the shaders fetch.
  phase_timer.Next(DrawPhase::kVertexData);
  draw_read_ranges_.clear();
  if (!RequestVertexBuffers(*vertex_shader) ||
      (pixel_shader && !RequestVertexBuffers(*pixel_shader))) {
    return true;
  }
  // The memory the shaders export to, up to date in the shared memory (the
  // draw may write only part of it), and its extent for the barrier. Unknown
  // destinations (invalid or dynamic stream constants): all of it.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& range : memexport_ranges_) {
    uint32_t range_start = range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(range_start, range.size_bytes,
                                      SharedMemory::RequestKind::kMemExport)) {
      REXGPU_ERROR("Native renderer: failed to request memory export at {:08X} ({} bytes)",
                   range_start, range.size_bytes);
      return true;
    }
    memexport_extent_start = std::min(memexport_extent_start, range_start);
    memexport_extent_end = std::max(memexport_extent_end, range_start + range.size_bytes);
  }
  if (memexport_used && memexport_ranges_.empty() &&
      !shared_memory_->RequestRange(0, SharedMemory::kBufferSize,
                                    SharedMemory::RequestKind::kMemExport)) {
    REXGPU_ERROR("Native renderer: failed to request the shared memory for memory export");
    return true;
  }

  // Textures they sample, loaded and made sampleable (outside the render pass).
  phase_timer.Next(DrawPhase::kTextures);
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  texture_cache_->UpdateSubmissions(false);
  uint32_t deferred_mask = 0;
  VkImageView deferred_views[32];
  uint64_t deferred_generation = 0;
  if (defer_full_resolve_loads_ && !defer_disabled_by_memexport_ &&
      draw_scale4 > kScale4Min && REXCVAR_GET(native_scaled_render_textures) &&
      !REXCVAR_GET(native_sample_resolved_images)) {
    if (defer_vertex_shader_ != vertex_shader || defer_pixel_shader_ != pixel_shader) {
      DeferredTextureDimensions dimensions;
      auto add_dimensions = [&](const vulkan::VulkanShader& shader) {
        for (const auto& binding : shader.GetTextureBindingsAfterTranslation()) {
          dimensions.Add(binding.fetch_constant, binding.dimension);
        }
      };
      add_dimensions(*vertex_shader);
      if (pixel_shader) add_dimensions(*pixel_shader);
      defer_shader_mask_ = dimensions.candidates();
      defer_vertex_shader_ = vertex_shader;
      defer_pixel_shader_ = pixel_shader;
    }
    uint32_t candidates = used_texture_mask & defer_shader_mask_;
    uint32_t remaining = candidates;
    deferred_generation = resolver_.generation();
    while (remaining) {
      const uint32_t index = uint32_t(std::countr_zero(remaining));
      const uint32_t bit = uint32_t(1) << index;
      remaining &= ~bit;
      auto& fetch_memo = deferred_fetch_memos_[index];
      if (fetch_memo.serial != fetch_constant_serials_[index]) {
        fetch_memo.serial = fetch_constant_serials_[index];
        fetch_memo.candidate = texture_cache_->IsDeferredLoadCandidate(index);
        fetch_memo.origin_address = uint32_t(regs.GetTextureFetch(index).base_address) << 12;
      }
      auto& scaled_memo = scaled_view_memos_[index];
      const uint32_t* words = &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + index * 6];
      if (!fetch_memo.candidate ||
          // A normal partial composite can be non-null yet still fail the
          // experiment's strict proof. Cache that refusal as well: skipping
          // preflight only chooses eager loading, never skips required work.
          (scaled_memo.frame == frame_ + 1 &&
           (scaled_memo.view == VK_NULL_HANDLE || !scaled_memo.full_coverage) &&
           !std::memcmp(scaled_memo.fetch, words, sizeof(scaled_memo.fetch)) &&
           IsScaledViewMemoCurrent(scaled_memo, deferred_generation))) {
        candidates &= ~bit;
        continue;
      }
      // Test cheap refusals first. A stable origin lookup (including a miss)
      // needs no repeated walk over all resolve images. It caches no image or
      // coverage: the full preflight and both generation guards still run.
      const auto origin = fetch_memo.origin.Query(
          fetch_memo.origin_address, deferred_generation, [&] {
            return resolver_.Find(fetch_memo.origin_address) != nullptr;
          });
      if (origin.reused) {
        ++deferred_stats_.origin_reused;
      } else {
        ++deferred_stats_.origin_queries;
      }
      if (!origin.found) {
        candidates &= ~bit;
      }
    }
    if (candidates) {
      deferred_stats_.candidates += std::popcount(candidates);
      texture_cache_->RequestTexturesWithDeferredLoads(used_texture_mask, candidates);
      remaining = candidates;
      while (remaining) {
        const uint32_t index = uint32_t(std::countr_zero(remaining));
        const uint32_t bit = uint32_t(1) << index;
        remaining &= ~bit;
        VkImageView view = GetScaledRenderTextureView(index, frame_ + 1, true);
        if (view != VK_NULL_HANDLE) {
          deferred_views[index] = view;
          deferred_mask |= bit;
        }
      }
      if (resolver_.generation() != deferred_generation) {
        deferred_stats_.generation_fallback += std::popcount(deferred_mask);
        deferred_mask = 0;
      }
      const uint32_t fallback = candidates & ~deferred_mask;
      deferred_stats_.fallback += std::popcount(fallback);
      if (fallback) texture_cache_->RequestTextures(fallback);
    } else {
      texture_cache_->RequestTextures(used_texture_mask);
    }
  } else {
    texture_cache_->RequestTextures(used_texture_mask);
  }
  uint32_t fetched_as_2d_mask = 0;
  auto add_fetched_as_2d = [&](const vulkan::VulkanShader& shader) {
    for (const SpirvShader::TextureBinding& binding : shader.GetTextureBindingsAfterTranslation()) {
      if (binding.dimension == xenos::FetchOpDimension::k1D ||
          binding.dimension == xenos::FetchOpDimension::k2D) {
        fetched_as_2d_mask |= uint32_t(1) << binding.fetch_constant;
      }
    }
  };
  add_fetched_as_2d(*vertex_shader);
  if (pixel_shader) {
    add_fetched_as_2d(*pixel_shader);
  }
  texture_cache_->Request3DAs2DTextures(fetched_as_2d_mask);

  // The render targets written: colour where the pixel shader writes and the
  // mask lets it, depth where it is tested or stencil is used.
  phase_timer.Next(DrawPhase::kTargets);
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  RenderTarget* color_targets[xenos::kMaxColorRenderTargets] = {};
  uint32_t color_attachments = 0;
  uint32_t fixed16_color_targets = 0;
  // Not when nothing is rasterized (memory export only): nothing is written.
  const bool depth_needed =
      rasterization_done &&
      (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable);
  RenderTarget* depth_target = nullptr;
  DrawTargetsMemo targets_from;
  targets_from.valid = true;
  targets_from.surface_info = surface_info.value;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    targets_from.color_info[i] = regs[reg::RB_COLOR_INFO::rt_register_indices[i]];
  }
  targets_from.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
  targets_from.color_mask_in = color_mask;
  targets_from.depth_needed = depth_needed;
  targets_from.scale4 = draw_scale4;
  const DrawTargetsMemo& targets_memo = draw_targets_memo_;
  if (targets_memo.valid && targets_memo.surface_info == targets_from.surface_info &&
      !std::memcmp(targets_memo.color_info, targets_from.color_info,
                   sizeof(targets_from.color_info)) &&
      targets_memo.depth_info == targets_from.depth_info &&
      targets_memo.color_mask_in == targets_from.color_mask_in &&
      targets_memo.depth_needed == targets_from.depth_needed &&
      targets_memo.scale4 == targets_from.scale4) {
    std::memcpy(color_targets, targets_memo.color, sizeof(color_targets));
    depth_target = targets_memo.depth;
    color_mask = targets_memo.color_mask;
    color_attachments = targets_memo.color_attachments;
    fixed16_color_targets = targets_memo.fixed16_color_targets;
  } else {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!((color_mask >> (i * 4)) & 0b1111)) {
        continue;
      }
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      RenderTarget* target =
          render_targets_.GetColor(color_info.color_base, color_info.color_format,
                                   surface_info.surface_pitch, surface_info.msaa_samples,
                                   draw_scale4);
      for (uint32_t j = 0; target && j < i; ++j) {
        if (color_targets[j] == target) {
          // One image can't be two attachments; the shader picks one by flow
          // control in such passes (as in 4D5307E6's lighting).
          LogDrawOnce(kDrawLogAliasedTargets,
                      "a draw writes one surface through two render targets, the second is "
                      "dropped");
          target = nullptr;
        }
      }
      if (!target) {
        color_mask &= ~(uint32_t(0b1111) << (i * 4));
        continue;
      }
      color_targets[i] = target;
      color_attachments |= uint32_t(1) << i;
      if (target->fixed16) {
        fixed16_color_targets |= uint32_t(1) << i;
      }
    }
    if (depth_needed) {
      auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      depth_target = render_targets_.GetDepth(depth_info.depth_base, depth_info.depth_format,
                                              surface_info.surface_pitch,
                                              surface_info.msaa_samples, draw_scale4);
    }
    std::memcpy(targets_from.color, color_targets, sizeof(targets_from.color));
    targets_from.depth = depth_target;
    targets_from.color_mask = color_mask;
    targets_from.color_attachments = color_attachments;
    targets_from.fixed16_color_targets = fixed16_color_targets;
    draw_targets_memo_ = targets_from;
  }
  if (!color_attachments && !depth_target && !memexport_used && !viz_survey) {
    // Nothing is written.
    return true;
  }
  // Scene consumer: the opaque/translucent boundary is the first blended draw
  // into the scene color once the title asked for it (kSceneAutoOpaque).
  if (scene_frames_ && color_targets[0] && depth_target && (color_mask & 0b0111) &&
      normalized_depth_control.z_enable && scene_frames_->WantsAutoOpaque(depth_target->id)) {
    const auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    if (blend.color_srcblend != xenos::BlendFactor::kOne || blend.color_destblend != xenos::BlendFactor::kZero ||
        blend.color_comb_fcn != xenos::BlendOp::kAdd) {
      scene_frames_->SnapshotOpaque(*color_targets[0]);
    }
  }

  // EDRAM ownership over the scissor: what other surfaces left in these
  // tiles comes in first (clears and transfers, outside the render pass),
  // then the tiles the draw writes no longer hold a known value.
  draw_util::Scissor draw_scissor;
  draw_util::GetScissor(regs, draw_scissor);
  const Rect draw_rect = {int32_t(draw_scissor.offset[0]), int32_t(draw_scissor.offset[1]),
                          int32_t(draw_scissor.offset[0] + draw_scissor.extent[0]),
                          int32_t(draw_scissor.offset[1] + draw_scissor.extent[1])};
  // Not the whole scissor: Direct3D 9 leaves it at 8192 rows, and a depth
  // surface claimed that far takes the tiles of the draw's own colour surface
  // placed after it (425307DB: depth at tile 0, colour at 216 or 782). The
  // surface that owns the tiles is then not the one the draw wrote, and the
  // next claim brings in what was there before the draw. As xenos
  // (RenderTargetCache::Update): the rows the draw can reach, and a surface
  // ending where another surface of the draw begins.
  const bool claim_draw_extent = REXCVAR_GET(native_claim_draw_extent);
  int32_t draw_bottom = draw_rect.bottom;
  if (claim_draw_extent && draw_extent_estimator_ && vertex_shader &&
      vertex_shader->is_ucode_analyzed()) {
    draw_bottom = int32_t(std::min(
        uint32_t(std::max(draw_bottom, int32_t(0))),
        draw_extent_estimator_->EstimateMaxY(true, *vertex_shader, false)));
  }
  auto claim_rect = [&](const RenderTarget& target) {
    Rect rect = draw_rect;
    rect.bottom = std::min(rect.bottom, draw_bottom);
    const uint32_t pitch_tiles = PitchTiles(target.surface);
    if (!claim_draw_extent || !pitch_tiles) {
      return rect;
    }
    const uint32_t tile_count = render_targets_.tile_count();
    auto end_at = [&](const RenderTarget* other) {
      if (!other || other == &target) {
        return;
      }
      // Whole rows before the other surface's first tile (none: they begin in
      // the same row, nothing to divide by rows).
      uint32_t distance = (other->surface.base_tiles % tile_count + tile_count -
                           target.surface.base_tiles % tile_count) %
                          tile_count;
      uint32_t rows = distance / pitch_tiles;
      if (rows) {
        rect.bottom = int32_t(std::min(uint32_t(std::max(rect.bottom, int32_t(0))),
                                       rows * TileHeightPixels(target.surface)));
      }
    };
    for (const RenderTarget* other : color_targets) {
      end_at(other);
    }
    end_at(depth_target);
    return rect;
  };
  phase_timer.Next(DrawPhase::kClaims);
  // A clear rectangle (native_clear_rect_overwrite): what it replaces whole
  // in the tiles it covers isn't brought in. Every pixel reached is written:
  // no kill, alpha test or alpha to coverage, all samples.
  ClearRectangle clear_rectangle;
  const bool clear_rectangle_draw =
      REXCVAR_GET(native_clear_rect_overwrite) && claim_draw_extent && rasterization_done &&
      !viz_survey && !(pixel_shader && pixel_shader->kills_pixels()) &&
      [&]() {
        const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
        return !(color_control.alpha_test_enable &&
                 color_control.alpha_func != xenos::CompareFunction::kAlways) &&
               !color_control.alpha_to_mask_enable;
      }() &&
      (regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xF) == 0xF &&
      GetClearRectangle(regs, *memory_, *vertex_shader, clear_rectangle);
  // A claim with a clear rectangle: the part of the claim it writes whole
  // (the rectangle inside the claim, which lies in the scissor; whole pixels
  // only) is claimed first with the overwrite mask, and only the tiles inside
  // it skip their transfer (EdramOwnership::Claim). Then the whole rectangle
  // as usual: the tiles the clear leaves, or covers only in part, come in
  // (Fuse: 14 of 15 tile columns skipped, the last one transferred).
  auto claim_with_clear = [&](RenderTarget& target, const Rect& rect, uint32_t overwrite) {
    if (overwrite && clear_rectangle_draw) {
      Rect written = rect;
      written.right = std::min(written.right, int32_t(std::floor(clear_rectangle.right)));
      written.bottom = std::min(written.bottom, int32_t(std::floor(clear_rectangle.bottom)));
      if (!written.empty()) {
        render_targets_.Claim(target, written, overwrite,
                              RenderTargets::ClaimCaller::kDrawClearRect);
        if (written.right >= rect.right && written.bottom >= rect.bottom) {
          return;
        }
      }
    }
    render_targets_.Claim(target, rect, 0, RenderTargets::ClaimCaller::kDraw);
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* target = color_targets[i];
    if (!target) {
      continue;
    }
    const Rect target_rect = claim_rect(*target);
    uint32_t overwrite = 0;
    if (clear_rectangle_draw && ((color_mask >> (i * 4)) & 0b1111) == 0b1111) {
      // Not blended: the source as it is.
      const auto blend =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      if (blend.color_srcblend == xenos::BlendFactor::kOne &&
          blend.color_destblend == xenos::BlendFactor::kZero &&
          blend.color_comb_fcn == xenos::BlendOp::kAdd &&
          blend.alpha_srcblend == xenos::BlendFactor::kOne &&
          blend.alpha_destblend == xenos::BlendFactor::kZero &&
          blend.alpha_comb_fcn == xenos::BlendOp::kAdd) {
        overwrite = kEdramPartMain;
      }
    }
    claim_with_clear(*target, target_rect, overwrite);
    render_targets_.edram().Written(*target, target_rect, kEdramPartMain);
  }
  if (depth_target) {
    uint32_t depth_written = 0;
    if (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable) {
      depth_written |= kEdramPartMain;
    }
    if (normalized_depth_control.stencil_enable) {
      // From now on its claims bring the stencil in too.
      depth_target->stencil_used = true;
      // Written unless neither face can change it (a write mask of zero, or
      // every op keeping): stencil tests alone leave other surfaces' copies
      // of these tiles valid (EdramOwnership).
      auto face_writes = [](uint32_t write_mask, xenos::StencilOp fail, xenos::StencilOp zpass,
                            xenos::StencilOp zfail) {
        return write_mask != 0 &&
               (fail != xenos::StencilOp::kKeep || zpass != xenos::StencilOp::kKeep ||
                zfail != xenos::StencilOp::kKeep);
      };
      bool stencil_written = face_writes(
          regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask, normalized_depth_control.stencilfail,
          normalized_depth_control.stencilzpass, normalized_depth_control.stencilzfail);
      if (normalized_depth_control.backface_enable) {
        // Either register may be the front one (see draw_state.cpp): both.
        stencil_written =
            stencil_written ||
            face_writes(regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF)
                            .stencilwritemask,
                        normalized_depth_control.stencilfail_bf,
                        normalized_depth_control.stencilzpass_bf,
                        normalized_depth_control.stencilzfail_bf) ||
            face_writes(regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF)
                            .stencilwritemask,
                        normalized_depth_control.stencilfail, normalized_depth_control.stencilzpass,
                        normalized_depth_control.stencilzfail) ||
            face_writes(regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask,
                        normalized_depth_control.stencilfail_bf,
                        normalized_depth_control.stencilzpass_bf,
                        normalized_depth_control.stencilzfail_bf);
      }
      if (stencil_written) {
        depth_written |= kEdramPartStencil;
      }
    }
    const Rect depth_rect = claim_rect(*depth_target);
    uint32_t depth_overwrite = 0;
    if (clear_rectangle_draw) {
      const reg::RB_DEPTHCONTROL& dc = normalized_depth_control;
      const bool depth_passes = !dc.z_enable || dc.zfunc == xenos::CompareFunction::kAlways;
      if (dc.z_enable && dc.z_write_enable && depth_passes) {
        depth_overwrite |= kEdramPartMain;
      }
      // All 8 bits replaced by a value of the draw's (reference or zero), on
      // both faces when they differ.
      // Either register may be the front one (see draw_state.cpp).
      auto face_replaces = [](xenos::CompareFunction func, xenos::StencilOp zpass) {
        return func == xenos::CompareFunction::kAlways &&
               (zpass == xenos::StencilOp::kReplace || zpass == xenos::StencilOp::kZero);
      };
      const bool masks_full =
          (regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask & 0xFF) == 0xFF &&
          (!dc.backface_enable ||
           (regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilwritemask &
            0xFF) == 0xFF);
      if (dc.stencil_enable && depth_passes && masks_full &&
          face_replaces(dc.stencilfunc, dc.stencilzpass) &&
          (!dc.backface_enable || face_replaces(dc.stencilfunc_bf, dc.stencilzpass_bf))) {
        depth_overwrite |= kEdramPartStencil;
      }
    }
    claim_with_clear(*depth_target, depth_rect, depth_overwrite);
    render_targets_.edram().Written(*depth_target, depth_rect, depth_written);
  }
  phase_timer.Next(DrawPhase::kTargets);

  // The render pass and the framebuffer: as large as the smallest target.
  Pipelines::RenderPassKey render_pass_key;
  VkExtent2D extent = {UINT32_MAX, UINT32_MAX};
  auto add_attachment = [&](const RenderTarget& target) {
    render_pass_key.samples = target.samples;
    extent.width = std::min(extent.width, target.host_width);
    extent.height = std::min(extent.height, target.host_height);
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color_targets[i]) {
      render_pass_key.color[i] = color_targets[i]->vk_format;
      add_attachment(*color_targets[i]);
    }
  }
  if (depth_target) {
    render_pass_key.depth = depth_target->vk_format;
    add_attachment(*depth_target);
  }
  if (!color_attachments && !depth_target) {
    // Memory export or a VIZ survey only: a render pass without attachments,
    // rasterized over the scissor (one pixel when nothing is rasterized).
    const ui::vulkan::VulkanDevice::Properties& properties = GetVulkanDevice()->properties();
    extent.width = 1;
    extent.height = 1;
    if (rasterization_done) {
      extent.width =
          std::clamp(uint32_t(ScaleEdge(std::max(draw_rect.right, int32_t(1)), draw_scale4)),
                     uint32_t(1), properties.maxFramebufferWidth);
      extent.height =
          std::clamp(uint32_t(ScaleEdge(std::max(draw_rect.bottom, int32_t(1)), draw_scale4)),
                     uint32_t(1), properties.maxFramebufferHeight);
    }
  }
  // Still in the draws' render pass begun for targets of which this draw's are
  // a subset in the same slots, and its scissor inside that pass's extent: it
  // stays in it, the targets it doesn't write attached with nothing written to
  // them (as the SDK's backends keep their render targets bound), instead of a
  // new pass (with every attachment loaded and stored again). They are not
  // claimed: their EDRAM tiles are not touched by this draw, and an image that
  // stays attached keeps what it holds. The extent is the smallest target's,
  // and targets reach to the end of the EDRAM from their bases, so a draw
  // without the depth target may have a larger one of its own: nothing outside
  // its scissor is drawn anyway.
  if ((color_attachments || depth_target) && open_pass_.render_pass != VK_NULL_HANDLE &&
      submitter_.render_pass() == open_pass_.render_pass &&
      submitter_.render_pass_framebuffer() == open_pass_.framebuffer &&
      REXCVAR_GET(native_keep_render_pass)) {
    bool subset = !depth_target || depth_target == open_pass_.depth;
    bool superset = !open_pass_.depth || depth_target == open_pass_.depth;
    bool adds = depth_target == nullptr && open_pass_.depth != nullptr;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_targets[i]) {
        subset &= color_targets[i] == open_pass_.color[i];
      } else {
        adds |= open_pass_.color[i] != nullptr;
      }
      superset &= !open_pass_.color[i] || color_targets[i] == open_pass_.color[i];
    }
    bool fits =
        uint32_t(std::max(ScaleEdge(draw_rect.right, draw_scale4), 0)) <= open_pass_.extent.width &&
        uint32_t(std::max(ScaleEdge(draw_rect.bottom, draw_scale4), 0)) <= open_pass_.extent.height;
    bool same = subset && !adds && open_pass_.extent.width == extent.width &&
                open_pass_.extent.height == extent.height;
    if (!same) {
      // Why a draw with a pass open doesn't stay in it (frame_stats_interval).
      if (!subset) {
        ++stats_pass_not_kept_[superset ? 0 : 1];
      } else if (!fits) {
        ++stats_pass_not_kept_[2];
      }
    }
    if (subset && fits && !same) {
      extent = open_pass_.extent;
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        color_targets[i] = open_pass_.color[i];
        if (color_targets[i]) {
          color_attachments |= uint32_t(1) << i;
        }
      }
      depth_target = open_pass_.depth;
      render_pass_key = open_pass_.key;
      ++stats_pass_kept_draws_;
    }
  }
  VkRenderPass render_pass;
  VkFramebuffer framebuffer;
  DrawFramebufferMemo& framebuffer_memo = draw_framebuffer_memo_;
  if (framebuffer_memo.valid &&
      !std::memcmp(&framebuffer_memo.key, &render_pass_key, sizeof(render_pass_key)) &&
      !std::memcmp(framebuffer_memo.color, color_targets, sizeof(framebuffer_memo.color)) &&
      framebuffer_memo.depth == depth_target &&
      framebuffer_memo.extent.width == extent.width &&
      framebuffer_memo.extent.height == extent.height) {
    render_pass = framebuffer_memo.render_pass;
    framebuffer = framebuffer_memo.framebuffer;
  } else {
    render_pass = pipelines_.GetRenderPass(render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      return true;
    }
    framebuffer =
        render_targets_.GetDrawFramebuffer(render_pass, color_targets, depth_target, extent);
    if (framebuffer == VK_NULL_HANDLE) {
      return true;
    }
    framebuffer_memo.valid = true;
    framebuffer_memo.key = render_pass_key;
    std::memcpy(framebuffer_memo.color, color_targets, sizeof(framebuffer_memo.color));
    framebuffer_memo.depth = depth_target;
    framebuffer_memo.extent = extent;
    framebuffer_memo.render_pass = render_pass;
    framebuffer_memo.framebuffer = framebuffer;
  }

  // The pipeline.
  phase_timer.Next(DrawPhase::kPipeline);
  PipelineStateInput state_input;
  state_input.host_primitive_type = primitive_processing_result.host_primitive_type;
  state_input.host_primitive_reset = primitive_processing_result.host_primitive_reset_enabled;
  state_input.primitive_polygonal = primitive_polygonal;
  state_input.normalized_depth_control = normalized_depth_control;
  state_input.color_mask = color_mask;
  state_input.color_attachments = color_attachments;
  state_input.depth_attachment = depth_target != nullptr;
  state_input.rt0_rgb_premultiplied =
      pixel_shader &&
      pixel_shader_modification.pixel.rt0_blend_rgb_factor_for_premult != xenos::BlendFactor::kOne;
  state_input.rt0_alpha_premultiplied =
      pixel_shader &&
      pixel_shader_modification.pixel.rt0_blend_a_factor_for_premult != xenos::BlendFactor::kOne;
  state_input.depth_clamp_requested = REXCVAR_GET(force_depth_clamp);
  state_input.rasterizer_discard = !rasterization_done;
  state_input.patch_control_points = patch_control_points;
  PipelineState pipeline_state;
  if (!GetPipelineState(regs, state_input, pipelines_.caps(), pipeline_state)) {
    LogDrawOnce(kDrawLogTopology, "a draw of an unexpected host primitive type is skipped");
    return true;
  }
  const uint32_t layout_counts[4] = {
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size()),
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size()),
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0,
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0};
  const Pipelines::Layout* layout;
  if (layout_memo_.layout && !std::memcmp(layout_memo_.counts, layout_counts,
                                          sizeof(layout_counts))) {
    layout = layout_memo_.layout;
  } else {
    layout = pipelines_.GetLayout(layout_counts[0], layout_counts[1], layout_counts[2],
                                  layout_counts[3]);
    if (!layout) {
      return true;
    }
    std::memcpy(layout_memo_.counts, layout_counts, sizeof(layout_counts));
    layout_memo_.layout = layout;
  }
  // In the background with async_shader_compilation, as the SDK's backends:
  // depth-only pipelines (shadows, depth prepasses) only with
  // async_depth_only_pipelines, since their draws would be missing until then.
  bool async_pipeline =
      REXCVAR_GET(async_shader_compilation) &&
      (pixel_translation != nullptr || REXCVAR_GET(async_depth_only_pipelines));
  bool pipeline_pending;
  bool pipeline_requested_first;
  // Depth-only draws testing D24FS8 depth convert too (the translator's
  // depth-only shader), so the depth buffer holds one encoding, as on the
  // SDK's Vulkan backend.
  VkShaderModule pixel_shader_module =
      pixel_translation ? pixel_translation->shader_module() : VK_NULL_HANDLE;
  uint64_t depth_only_float24 = 0;
  if (!pixel_translation && depth_float24_convert && depth_target && pipeline_state.depth_test &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    bool round = REXCVAR_GET(depth_float24_round);
    pixel_shader_module = shaders_.GetDepthOnlyFloat24Shader(round);
    depth_only_float24 = pixel_shader_module != VK_NULL_HANDLE ? (round ? 2 : 1) : 0;
  }
  // The last draw's pipeline for the same shaders, render pass, layout and
  // state (a created one; a new one may need storing, a pending one waiting).
  const VkShaderModule pipeline_modules[4] = {vertex_translation->shader_module(),
                                              pixel_shader_module, tessellation_vertex_shader,
                                              tessellation_control_shader};
  VkPipeline pipeline;
  if (pipeline_memo_.pipeline != VK_NULL_HANDLE && pipeline_memo_.render_pass == render_pass &&
      pipeline_memo_.pipeline_layout == layout->pipeline_layout &&
      !std::memcmp(pipeline_memo_.modules, pipeline_modules, sizeof(pipeline_modules)) &&
      !std::memcmp(&pipeline_memo_.state, &pipeline_state, sizeof(pipeline_state))) {
    pipeline = pipeline_memo_.pipeline;
    pipeline_pending = false;
    pipeline_requested_first = false;
  } else {
    pipeline = pipelines_.GetPipeline(
        vertex_translation->shader_module(), pixel_shader_module, tessellation_vertex_shader,
        tessellation_control_shader, render_pass_key, *layout, pipeline_state, async_pipeline,
        async_wait_ms, pipeline_pending, &pipeline_requested_first);
    if (pipeline != VK_NULL_HANDLE) {
      std::memcpy(pipeline_memo_.modules, pipeline_modules, sizeof(pipeline_modules));
      pipeline_memo_.render_pass = render_pass;
      pipeline_memo_.pipeline_layout = layout->pipeline_layout;
      pipeline_memo_.state = pipeline_state;
      pipeline_memo_.pipeline = pipeline;
    }
  }
  // A new pipeline and its shaders kept on disk (store_shaders), created at
  // the next start before the game draws.
  if (pipeline_storage_.is_active()) {
    pipeline_storage_.StoreShader(*vertex_shader);
    if (pixel_shader) {
      pipeline_storage_.StoreShader(*pixel_shader);
    }
    if (pipeline_requested_first) {
      NativePipelineDescription description;
      std::memset(&description, 0, sizeof(description));
      description.vertex_shader_hash = vertex_shader->ucode_data_hash();
      description.vertex_shader_modification = vertex_shader_modification.value;
      if (pixel_shader) {
        description.pixel_shader_hash = pixel_shader->ucode_data_hash();
        description.pixel_shader_modification = pixel_shader_modification.value;
      } else {
        // The host's depth-only float24 shader, if any (1 truncating, 2
        // rounding).
        description.pixel_shader_modification = depth_only_float24;
      }
      description.render_pass = render_pass_key;
      description.state = pipeline_state;
      pipeline_storage_.StorePipeline(description);
    }
  }
  if (pipeline == VK_NULL_HANDLE) {
    // Being created: the draw is skipped, and the frame held back
    // (vulkan_async_skip_incomplete_frames). Else logged once by the pipeline
    // cache.
    frame_used_pending_pipeline_ |= pipeline_pending;
    return true;
  }

  // Viewport, scissor and the rest of the dynamic state, in host pixels.
  // At a fractional scale the SDK's viewport in guest pixels, then scaled
  // (its limits in guest pixels a pixel short, as rounding outwards may add
  // one host pixel).
  phase_timer.Next(DrawPhase::kState);
  draw_util::ViewportInfo viewport_info;
  const bool scale_whole = !(draw_scale4 & 3);
  draw_util::GetHostViewportInfo(
      regs, scale_whole ? draw_scale4 >> 2 : 1, scale_whole ? draw_scale4 >> 2 : 1, false,
      scale_whole ? device_properties.maxViewportDimensions[0]
                  : device_properties.maxViewportDimensions[0] * kScale4Min / draw_scale4 - 1,
      scale_whole ? device_properties.maxViewportDimensions[1]
                  : device_properties.maxViewportDimensions[1] * kScale4Min / draw_scale4 - 1,
      REXCVAR_GET(vulkan_reverse_depth_range), normalized_depth_control, depth_float24_convert,
      true, pixel_shader && pixel_shader->writes_depth(), viewport_info);
  if (!scale_whole) {
    ScaleViewportInfo(viewport_info, draw_scale4);
  }
  DynamicState dynamic_state;
  GetDynamicState(regs, viewport_info, primitive_polygonal, normalized_depth_control, extent,
                  pipelines_.caps(), dynamic_state, draw_scale4);
  if (!REXCVAR_GET(native_depth_bias) || polygon_offset_in_shader) {
    dynamic_state.depth_bias_constant = 0.0f;
    dynamic_state.depth_bias_slope = 0.0f;
  } else if (depth_bias_force_unorm) {
    dynamic_state.depth_bias_force_unorm = 1;
    if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      // In constant units, the host's [0, 0.5] for the guest's float24 [0, 1]
      // no longer lowers the exponent (kD3D10PolygonOffsetFactorFloat24).
      dynamic_state.depth_bias_constant *= 0.5f;
    }
  }

  // 32-bit guest indices the host can't take in full: fetched by the vertex
  // shader.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      regs.Get<reg::VGT_DRAW_INITIATOR>().index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex;
  SystemConstantsInput constants_input;
  constants_input.primitive_polygonal = primitive_polygonal;
  constants_input.primitive = &primitive_processing_result;
  constants_input.shader_32bit_index_dma = shader_32bit_index_dma;
  constants_input.viewport_info = &viewport_info;
  constants_input.fixed16_color_targets = fixed16_color_targets;
  constants_input.texture_cache = texture_cache_.get();
  constants_input.used_texture_mask = used_texture_mask;
  constants_input.image_view_format_swizzle = device_properties.imageViewFormatSwizzle;
  constants_input.resolution_scale = float(draw_scale4) * 0.25f;
  constants_input.host_depth_polygon_offset =
      polygon_offset_in_shader ? &host_depth_polygon_offset : nullptr;
  SpirvShaderTranslator::SystemConstants system_constants;
  FillSystemConstants(regs, constants_input, system_constants);
  // Scene jitter: every depth-tested draw against the scene depth over the
  // scene viewport, whatever its colour targets (depth-only, G-buffer and
  // colour passes alike, so depth-equal tests still match). Full-screen post
  // passes test no depth: shifting them would move the image once per pass.
  const std::array<float, 3> unjittered_ndc_offset = {
      system_constants.ndc_offset[0], system_constants.ndc_offset[1], system_constants.ndc_offset[2]};
  bool scene_jittered = false;
  if (scene_jitter_.armed && normalized_depth_control.z_enable && depth_target &&
      depth_target->id == scene_jitter_.depth_id &&
      dynamic_state.viewport.x == float(scene_jitter_.origin_x) &&
      dynamic_state.viewport.y == float(scene_jitter_.origin_y) &&
      dynamic_state.viewport.width == float(scene_jitter_.width) &&
      dynamic_state.viewport.height == float(scene_jitter_.height)) {
    system_constants.ndc_offset[0] += 2.f * scene_jitter_.pixels[0] / float(scene_jitter_.width);
    system_constants.ndc_offset[1] += 2.f * scene_jitter_.pixels[1] / float(scene_jitter_.height);
    scene_jittered = true;
    ++scene_jitter_draws_;
  }
  // Object motion: the scene draws writing the scene depth (opaque geometry,
  // depth prepasses) keep their vertex positions; drawn again at kSceneEnd
  // with the same draw's of the previous frame.
  if (object_motion_ && depth_target && normalized_depth_control.z_enable &&
      normalized_depth_control.z_write_enable && rasterization_done && primitive_polygonal &&
      !memexport_used && !(pixel_shader && pixel_shader->writes_depth()) &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex &&
      object_motion_->IsSceneDraw(depth_target->id, dynamic_state.viewport)) {
    ObjectMotion::Draw motion_draw;
    bool capturable = true;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        motion_draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        motion_draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        motion_draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        break;
      default:
        capturable = false;
        break;
    }
    // The same indices as the draw below (alive until the frame's end).
    if (primitive_processing_result.index_buffer_type !=
            PrimitiveProcessor::ProcessedIndexBufferType::kNone &&
        !shader_32bit_index_dma) {
      std::pair<VkBuffer, VkDeviceSize> motion_indices{VK_NULL_HANDLE, 0};
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
          motion_indices = std::make_pair(
              shared_memory_->buffer(), VkDeviceSize(primitive_processing_result.guest_index_base));
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          motion_indices = primitive_processor_->GetConvertedIndexBuffer(
              primitive_processing_result.host_index_buffer_handle);
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          motion_indices = primitive_processor_->GetBuiltinIndexBuffer(
              primitive_processing_result.host_index_buffer_handle);
          break;
        default:
          capturable = false;
          break;
      }
      motion_draw.index_buffer = motion_indices.first;
      motion_draw.index_offset = motion_indices.second;
      motion_draw.index_type = primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
                                   ? VK_INDEX_TYPE_UINT16
                                   : VK_INDEX_TYPE_UINT32;
      capturable = capturable && motion_indices.first != VK_NULL_HANDLE;
    }
    if (capturable) {
      motion_draw.vertex_count = primitive_processing_result.host_draw_vertex_count;
      motion_draw.index_endian = uint32_t(system_constants.vertex_index_endian);
      motion_draw.primitive_restart = primitive_processing_result.host_primitive_reset_enabled;
      if (scene_jittered) {
        motion_draw.jitter = {2.f * scene_jitter_.pixels[0] / float(scene_jitter_.width),
                              2.f * scene_jitter_.pixels[1] / float(scene_jitter_.height)};
      }
      motion_draw.depth_range = {dynamic_state.viewport.minDepth, dynamic_state.viewport.maxDepth};
      // The draw's identity across frames: its shaders, indices and vertex
      // buffers (positions and bones may change, the draw is the same).
      std::array<uint64_t, 10> key{vertex_shader->ucode_data_hash(),
                                  pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                                  primitive_processing_result.guest_index_base,
                                  uint64_t(primitive_processing_result.index_buffer_type),
                                  primitive_processing_result.host_draw_vertex_count,
                                  uint64_t(primitive_processing_result.host_primitive_type),
                                  uint64_t(primitive_processing_result.host_index_format),
                                  regs[XE_GPU_REG_VGT_INDX_OFFSET], 0, 0};
      const auto& vertex_bindings = vertex_shader->vertex_bindings();
      for (size_t i = 0; i < vertex_bindings.size() && i < 2; ++i) {
        key[8 + i] = regs.GetVertexFetch(vertex_bindings[i].fetch_constant).address;
      }
      motion_draw.key = XXH3_64bits(key.data(), sizeof(key));
      // Ties (the same mesh drawn several times): by the title's identity
      // registers when the shader reads them all, else the first float constants.
      std::array<float, ObjectMotion::kFingerprintRegisters * 4> fingerprint;
      uint32_t fingerprint_count = 0;
      const auto& constant_map = vertex_shader->constant_register_map();
      const auto reads = [&](uint32_t r) { return ((constant_map.float_bitmap[r >> 6] >> (r & 63)) & 1) != 0; };
      uint32_t fingerprint_first = 0, fingerprint_end = 256;
      if (const uint32_t count = object_motion_->identity_count()) {
        const uint32_t first = object_motion_->identity_first();
        bool all = true;
        for (uint32_t r = first; r < first + count; ++r) all = all && reads(r);
        if (all) { fingerprint_first = first; fingerprint_end = first + count; }
      }
      for (uint32_t r = fingerprint_first; r < fingerprint_end && fingerprint_count < fingerprint.size(); ++r) {
        if (!((constant_map.float_bitmap[r >> 6] >> (r & 63)) & 1)) continue;
        for (uint32_t i = 0; i < 4; ++i) {
          fingerprint[fingerprint_count++] =
              std::bit_cast<float>(regs[XE_GPU_REG_SHADER_CONSTANT_000_X + r * 4 + i]);
        }
      }
      uint32_t capture_base, capture_mask, capture_tag;
      if (object_motion_->Capture(motion_draw, fingerprint.data(), fingerprint_count, capture_base,
                                  capture_mask, capture_tag)) {
        system_constants.motion_capture_base = capture_base;
        system_constants.motion_capture_mask = capture_mask;
        system_constants.motion_capture_tag = capture_tag;
      }
    }
  }

  // readback_memexport without readback_memexport_fast: the exported ranges
  // as they are before the draw, so only what it changes is written back.
  const bool memexport_readback =
      !memexport_ranges_.empty() && IsReadbackMemexportEnabled(false);
  if (memexport_readback && !REXCVAR_GET(readback_memexport_fast)) {
    memexport_readback_.TakeSnapshot(memexport_ranges_);
  }

  // Outside the render pass: the attachments' layouts, and the shared memory
  // readable by the draws (a barrier only after a write); writable where they
  // export to it.
  phase_timer.Next(DrawPhase::kLayouts);
  for (RenderTarget* target : color_targets) {
    if (target && target->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
      CommandRecorder* command_buffer =
          submitter_.OpenOutsideRenderPass(RenderPassEnd::kTargetLayout);
      if (command_buffer == nullptr) {
        return false;
      }
      render_targets_.Transition(command_buffer, *target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
  }
  if (depth_target && depth_target->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
    CommandRecorder* command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kTargetLayout);
    if (command_buffer == nullptr) {
      return false;
    }
    render_targets_.Transition(command_buffer, *depth_target,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  }
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        NativeSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start, memexport_extent_end - memexport_extent_start));
  } else if (memexport_used) {
    shared_memory_->Use(NativeSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(uint32_t(0), SharedMemory::kBufferSize));
  } else {
    shared_memory_->Use(NativeSharedMemory::Usage::kRead);
  }
  // The views and samplers of the shaders' bindings: a texture a resolve
  // wrote to the fetch's address is sampled from its image (2D only; AoT's
  // approach) when the fetch reads texels of its size, the rest from the
  // texture cache.
  phase_timer.Next(DrawPhase::kViews);
  if (deferred_mask && resolver_.generation() != deferred_generation) {
    const uint32_t count = std::popcount(deferred_mask);
    deferred_stats_.generation_fallback += count;
    deferred_stats_.fallback += count;
    texture_cache_->RequestTextures(deferred_mask);
    deferred_mask = 0;
    draw_textures_memo_.valid = false;
  }
  if (deferred_mask) deferred_stats_.served += std::popcount(deferred_mask);
  for (uint32_t remaining = deferred_mask; remaining; remaining &= remaining - 1) {
    const uint32_t index = uint32_t(std::countr_zero(remaining));
    const uint64_t bytes = texture_cache_->DeferredLoadGuestBytes(index);
    if (bytes) {
      ++deferred_stats_.stale_served;
      deferred_stats_.stale_bytes += bytes;
    }
  }
  const bool scaled_render_textures =
      draw_scale4 != kScale4Min && REXCVAR_GET(native_scaled_render_textures);
  // As the last draw's when nothing they come from changed (not with
  // native_sample_resolved_images, whose images change layouts here).
  const bool sample_resolved_images = REXCVAR_GET(native_sample_resolved_images);
  DrawTexturesMemo textures_from;
  textures_from.valid = !sample_resolved_images;
  textures_from.vertex_shader = vertex_shader;
  textures_from.pixel_shader = pixel_shader;
  textures_from.frame = frame_ + 1;
  textures_from.fetch_constant_changes = fetch_constant_changes_;
  textures_from.binding_generation = texture_cache_->binding_generation();
  textures_from.resolver_generation = scaled_render_textures ? resolver_.generation() : 0;
  textures_from.scaled = scaled_render_textures;
  textures_from.deferred_mask = deferred_mask;
  const DrawTexturesMemo& textures_memo = draw_textures_memo_;
  const bool textures_reused =
      textures_from.valid && textures_memo.valid &&
      textures_memo.vertex_shader == textures_from.vertex_shader &&
      textures_memo.pixel_shader == textures_from.pixel_shader &&
      textures_memo.frame == textures_from.frame &&
      textures_memo.fetch_constant_changes == textures_from.fetch_constant_changes &&
      textures_memo.binding_generation == textures_from.binding_generation &&
      textures_memo.resolver_generation == textures_from.resolver_generation &&
      textures_memo.scaled == textures_from.scaled &&
      textures_memo.deferred_mask == textures_from.deferred_mask;
  if (textures_reused) {
    ++stats_draw_textures_reused_;
  } else {
    draw_textures_.Clear();
    scaled_texture_mask_ = 0;
    auto texture_view_uncached = [&](const SpirvShader::TextureBinding& binding,
                                     bool& scaled_out) -> VkImageView {
      scaled_out = false;
      if (binding.dimension == xenos::FetchOpDimension::k1D ||
          binding.dimension == xenos::FetchOpDimension::k2D) {
        // At a resolution scale: what the game rendered into it, scaled (signed
        // bindings only of a format whose signed view is the unsigned one).
        if (scaled_render_textures) {
          VkImageView scaled_view = GetScaledRenderTextureView(binding.fetch_constant, frame_ + 1);
          if (scaled_view != VK_NULL_HANDLE) {
            scaled_out = true;
            return scaled_view;
          }
        }
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
        ResolvedTexture* resolved = sample_resolved_images
                                        ? resolver_.Find(uint32_t(fetch.base_address) << 12)
                                        : nullptr;
        if (resolved && !IsResolvedTextureFetchable(*resolved, fetch)) {
          LogDrawOnce(kDrawLogResolvedFormat,
                      "a resolve destination is fetched in another format, it is read from the "
                      "guest's bits in memory");
          resolved = nullptr;
        }
        if (resolved) {
          if (resolved->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            CommandRecorder* command_buffer =
                submitter_.OpenOutsideRenderPass(RenderPassEnd::kTargetLayout);
            if (command_buffer != nullptr) {
              resolver_.Transition(command_buffer, *resolved,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
          }
          return resolver_.GetSampledView(
              *resolved, texture_cache_->GetActiveTextureHostSwizzle(binding.fetch_constant));
        }
      }
      return texture_cache_->GetActiveBindingImageView(binding.fetch_constant, binding.dimension,
                                                       binding.is_signed);
    };
    auto texture_view = [&](const SpirvShader::TextureBinding& binding) -> VkImageView {
      const uint32_t deferred_slot = binding.fetch_constant & 31;
      if (deferred_mask & (uint32_t(1) << deferred_slot)) {
        scaled_texture_mask_ |= uint32_t(1) << deferred_slot;
        return deferred_views[deferred_slot];
      }
      const uint32_t fetch_constant =
          binding.fetch_constant & (xenos::kTextureFetchConstantCount - 1);
      const bool scaled_lookup = scaled_render_textures &&
                                 (binding.dimension == xenos::FetchOpDimension::k1D ||
                                  binding.dimension == xenos::FetchOpDimension::k2D);
      // Not with native_sample_resolved_images, whose images change layouts
      // here.
      BindingViewMemo* memo =
          textures_from.valid
              ? &binding_view_memos_[fetch_constant * 8 + (uint32_t(binding.dimension) & 3) * 2 +
                                     uint32_t(binding.is_signed)]
              : nullptr;
      const uint64_t binding_generation = texture_cache_->binding_generation(fetch_constant);
      // A scaled lookup's view as long as that lookup's memo is (no resolve
      // into the memory it looked at); the others don't depend on resolves.
      ScaledViewMemo& scaled_view_memo = scaled_view_memos_[fetch_constant];
      if (memo && memo->view != VK_NULL_HANDLE && memo->frame == textures_from.frame &&
          memo->fetch_serial == fetch_constant_serials_[fetch_constant] &&
          memo->binding_generation == binding_generation &&
          memo->scaled_lookup == scaled_lookup &&
          (!scaled_lookup ||
           (memo->scaled_serial == scaled_view_memo.serial &&
            IsScaledViewMemoCurrent(scaled_view_memo, textures_from.resolver_generation)))) {
        ++stats_binding_views_reused_;
        if (scaled_lookup) {
          // Counted as GetScaledRenderTextureView's own memo counts it.
          ++stats_scaled_view_lookups_;
          const ScaledViewMemo& scaled_memo = scaled_view_memos_[fetch_constant];
          if (scaled_memo.counted != kScaledFetchCount) {
            CountScaledFetch(scaled_memo.counted);
          }
        }
        if (memo->scaled) {
          scaled_texture_mask_ |= uint32_t(1) << fetch_constant;
        }
        return memo->view;
      }
      bool scaled = false;
      VkImageView view = texture_view_uncached(binding, scaled);
      if (scaled) {
        scaled_texture_mask_ |= uint32_t(1) << fetch_constant;
      }
      if (memo) {
        memo->frame = textures_from.frame;
        memo->fetch_serial = fetch_constant_serials_[fetch_constant];
        memo->binding_generation = binding_generation;
        memo->scaled_serial = scaled_view_memo.serial;
        memo->view = view;
        memo->scaled_lookup = scaled_lookup;
        memo->scaled = scaled;
      }
      return view;
    };
    for (const SpirvShader::TextureBinding& binding :
         vertex_shader->GetTextureBindingsAfterTranslation()) {
      draw_textures_.vertex_views.push_back(texture_view(binding));
    }
    if (pixel_shader) {
      for (const SpirvShader::TextureBinding& binding :
           pixel_shader->GetTextureBindingsAfterTranslation()) {
        draw_textures_.pixel_views.push_back(texture_view(binding));
      }
    }
    phase_timer.Next(DrawPhase::kSamplers);
    for (const SpirvShader::SamplerBinding& binding :
         vertex_shader->GetSamplerBindingsAfterTranslation()) {
      draw_textures_.vertex_samplers.push_back(texture_cache_->GetSampler(binding));
    }
    if (pixel_shader) {
      for (const SpirvShader::SamplerBinding& binding :
           pixel_shader->GetSamplerBindingsAfterTranslation()) {
        draw_textures_.pixel_samplers.push_back(texture_cache_->GetSampler(binding));
      }
    }
    draw_textures_memo_ = textures_from;
  }

  // A draw under a VIZ ID whose survey is still in flight runs under its
  // predicate (the survey's result copied for conditional rendering, which may
  // need recording outside the render pass first).
  VkDeviceSize viz_predicate_offset = 0;
  const VkBuffer viz_predicate_buffer = BindVIZPredicate(viz_predicate_offset);

  // In the render pass (kept open across draws to the same targets).
  phase_timer.Next(DrawPhase::kPass);
  CommandRecorder* command_buffer = submitter_.EnterRenderPass(render_pass, framebuffer, extent);
  if (command_buffer == nullptr) {
    return false;
  }
  // What it reads in this render pass: uploads of it now end the render pass
  // instead of going before it (native_hoist_uploads).
  for (const std::pair<uint32_t, uint32_t>& range : draw_read_ranges_) {
    shared_memory_->MarkReadInRenderPass(range.first, range.second);
  }
  const std::pair<uint32_t, uint32_t> index_range = primitive_processor_->last_guest_index_range();
  shared_memory_->MarkReadInRenderPass(index_range.first, index_range.second);
  // And the textures it samples: their loads now end it too
  // (native_hoist_texture_loads).
  texture_cache_->MarkSampledInRenderPass(used_texture_mask);
  if (open_pass_.render_pass != render_pass || open_pass_.framebuffer != framebuffer) {
    open_pass_.render_pass = render_pass;
    open_pass_.framebuffer = framebuffer;
    open_pass_.extent = extent;
    open_pass_.key = render_pass_key;
    std::memcpy(open_pass_.color, color_targets, sizeof(open_pass_.color));
    open_pass_.depth = depth_target;
  }
  if (bound_serial_ != submitter_.buffer_serial() ||
      bound_graphics_state_ != render_targets_.graphics_state_changes()) {
    // A new command buffer (nothing is bound in it), or EDRAM transfers or
    // resolve passes bound their own pipelines and state in it.
    bound_serial_ = submitter_.buffer_serial();
    bound_graphics_state_ = render_targets_.graphics_state_changes();
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_dynamic_state_valid_ = false;
    bindings_.InvalidateBound();
  }
  if (bound_pipeline_ != pipeline) {
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = pipeline;
    ++stats_pipeline_binds_;
  }
  // Only what differs from what is set (all of it in a new command buffer or
  // after passes setting their own).
  const bool dynamic_state_all = !bound_dynamic_state_valid_;
  const DynamicState& bound_state = bound_dynamic_state_;
  auto dynamic_differs = [&](const void* a, const void* b, size_t size) {
    return dynamic_state_all || std::memcmp(a, b, size) != 0;
  };
  if (dynamic_differs(&bound_state.viewport, &dynamic_state.viewport,
                      sizeof(dynamic_state.viewport))) {
    command_buffer->CmdVkSetViewport(0, 1, &dynamic_state.viewport);
  }
  if (dynamic_differs(&bound_state.scissor, &dynamic_state.scissor,
                      sizeof(dynamic_state.scissor))) {
    command_buffer->CmdVkSetScissor(0, 1, &dynamic_state.scissor);
  }
  // depth_bias_constant, depth_bias_slope and depth_bias_force_unorm follow
  // each other in the struct.
  if (dynamic_differs(&bound_state.depth_bias_constant, &dynamic_state.depth_bias_constant,
                      offsetof(DynamicState, blend_constants) -
                          offsetof(DynamicState, depth_bias_constant))) {
    if (dynamic_state.depth_bias_force_unorm) {
      command_buffer->CmdVkSetDepthBias2EXT(
          dynamic_state.depth_bias_constant, 0.0f, dynamic_state.depth_bias_slope,
          VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT, true);
    } else {
      command_buffer->CmdVkSetDepthBias(dynamic_state.depth_bias_constant, 0.0f,
                            dynamic_state.depth_bias_slope);
    }
  }
  if (dynamic_differs(bound_state.blend_constants, dynamic_state.blend_constants,
                      sizeof(dynamic_state.blend_constants))) {
    command_buffer->CmdVkSetBlendConstants(dynamic_state.blend_constants);
  }
  // Front and back at once where they are equal (the portability subset has
  // no separate ones).
  auto set_stencil = [&](void (CommandRecorder::*set_function)(VkStencilFaceFlags, uint32_t),
                         const uint32_t bound_values[2], const uint32_t values[2]) {
    if (!dynamic_differs(bound_values, values, sizeof(uint32_t) * 2)) {
      return;
    }
    if (values[0] == values[1]) {
      (command_buffer->*set_function)(VK_STENCIL_FACE_FRONT_AND_BACK, values[0]);
    } else {
      (command_buffer->*set_function)(VK_STENCIL_FACE_FRONT_BIT, values[0]);
      (command_buffer->*set_function)(VK_STENCIL_FACE_BACK_BIT, values[1]);
    }
  };
  set_stencil(&CommandRecorder::CmdVkSetStencilCompareMask, bound_state.stencil_compare_mask,
              dynamic_state.stencil_compare_mask);
  set_stencil(&CommandRecorder::CmdVkSetStencilWriteMask, bound_state.stencil_write_mask,
              dynamic_state.stencil_write_mask);
  set_stencil(&CommandRecorder::CmdVkSetStencilReference, bound_state.stencil_reference,
              dynamic_state.stencil_reference);
  bound_dynamic_state_ = dynamic_state;
  bound_dynamic_state_valid_ = true;
  // The fetch constants sampled from a scaled composite: the shaders scale
  // their sizes (unnormalized coordinates, offsets).
  system_constants.textures_resolution_scaled = scaled_texture_mask_;
  phase_timer.Next(DrawPhase::kDescriptors);
  if (!bindings_.Bind(command_buffer, *layout, regs, system_constants, *vertex_shader, pixel_shader,
                      draw_textures_)) {
    return false;
  }

  // Counted by the occlusion query of the open report (a segment of it per
  // render pass; samples at the scale, normalised by its area).
  phase_timer.Next(DrawPhase::kRecord);
  UpdateQuerySegment(draw_scale4 * draw_scale4, false, viz_survey);
  if (viz_survey) {
    // Measured if the segment carries its ID (else the base falls back).
    viz_survey_fallback.pending = false;
    OnVIZSurveyDraw(true);
  }
  if (viz_predicate_buffer != VK_NULL_HANDLE) {
    VkConditionalRenderingBeginInfoEXT conditional_rendering_info = {};
    conditional_rendering_info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
    conditional_rendering_info.buffer = viz_predicate_buffer;
    conditional_rendering_info.offset = viz_predicate_offset;
    command_buffer->CmdVkBeginConditionalRenderingEXT(conditional_rendering_info.buffer, conditional_rendering_info.offset, conditional_rendering_info.flags);
  }

  // Device-loss breadcrumbs (ReIGlue 1100): the work recorded for this draw
  // before its draw command (a render pass, EDRAM transfers, texture loads,
  // barriers); with gpu_device_lost_setup_breadcrumbs, a marker once it has
  // finished, so that a loss tells the work from the draw.
  ui::GpuBreadcrumbDetails breadcrumb_details;
  {
    const ui::vulkan::VulkanDevice* const breadcrumb_device = GetVulkanDevice();
    if (breadcrumb_device->HasBreadcrumbs()) {
      breadcrumb_details.work = command_buffer->TakeBreadcrumbWork();
      if (breadcrumb_details.work.Any() && breadcrumb_device->CanMarkBreadcrumbSetup()) {
        command_buffer->CmdSetupBreadcrumb(breadcrumb_device->NextBreadcrumb());
        breadcrumb_details.setup_marked = true;
      }
      breadcrumb_details.vertex_shader_modification = vertex_shader_modification.value;
      breadcrumb_details.pixel_shader_modification =
          pixel_shader ? pixel_shader_modification.value : 0;
      uint32_t color_formats[xenos::kMaxColorRenderTargets];
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        color_formats[i] = uint32_t(render_pass_key.color[i]);
      }
      breadcrumb_details.render_pass = ui::PackNativeRenderPass(
          uint32_t(render_pass_key.samples), uint32_t(render_pass_key.depth), color_formats);
      breadcrumb_details.render_pass_kind = ui::GpuRenderPassKind::kNative;
      breadcrumb_details.filled = true;
    }
  }

  // The draw.
  uint32_t vertex_count = primitive_processing_result.host_draw_vertex_count;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    command_buffer->CmdVkDraw(vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer = std::make_pair(shared_memory_->buffer(),
                                      VkDeviceSize(primitive_processing_result.guest_index_base));
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        return false;
    }
    command_buffer->CmdVkBindIndexBuffer(index_buffer.first, index_buffer.second,
                             primitive_processing_result.host_index_format ==
                                     xenos::IndexFormat::kInt16
                                 ? VK_INDEX_TYPE_UINT16
                                 : VK_INDEX_TYPE_UINT32);
    command_buffer->CmdVkDrawIndexed(vertex_count, 1, 0, 0, 0);
  }
  if (viz_predicate_buffer != VK_NULL_HANDLE) {
    command_buffer->CmdVkEndConditionalRenderingEXT();
  }
  // The draw command itself is not work recorded before the next draw.
  command_buffer->TakeBreadcrumbWork();
  command_buffer->CmdBreadcrumb(GetVulkanDevice()->RecordBreadcrumb(
      {0, submitter_.current(), frame_ + 1, vertex_shader->ucode_data_hash(),
       pixel_shader ? pixel_shader->ucode_data_hash() : 0, vertex_count, false,
       breadcrumb_details}));
  ++frame_draws_recorded_;
  if (REXCVAR_GET(native_scene_probe_tracking) || scene_frames_) {
    scene_last_draw_frame_ = frame_ + 1; scene_last_draw_ordinal_ = frame_draws_recorded_;
    scene_last_vs_hash_ = vertex_shader->ucode_data_hash();
    scene_last_ps_hash_ = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    scene_last_system_flags_ = system_constants.flags;
    std::copy_n(system_constants.ndc_scale, 3, scene_last_ndc_scale_.begin());
    scene_last_ndc_offset_ = unjittered_ndc_offset;
    scene_last_jittered_ = scene_jittered;
    scene_last_viewport_ = {dynamic_state.viewport.x, dynamic_state.viewport.y, dynamic_state.viewport.width,
                           dynamic_state.viewport.height, dynamic_state.viewport.minDepth, dynamic_state.viewport.maxDepth};
  }

  // What the draw exported: textures loaded from there are outdated, the CPU
  // writing there is watched; and copied back for the CPU (readback_memexport).
  // Guest DMA indices in the exported memory are read as the draw goes, not
  // from a copy as on xenos (no title known to export into its own indices).
  if (memexport_used) {
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(range.base_address_dwords << 2, range.size_bytes);
      }
    } else {
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    if (memexport_readback) {
      memexport_readback_.ReadBack(memexport_ranges_, REXCVAR_GET(readback_memexport_fast),
                                   frame_ + 1);
    }
  }
  return true;
}

void NativeCommandProcessor::OnDrawRenderPassEnding() {
  // The report and VIZ ID stay open: the next draw opens a new segment.
  CloseQuerySegment();
}

void NativeCommandProcessor::RecordQueryResolves() {
  if (!query_pool_) {
    return;
  }
  CommandRecorder* command_buffer = submitter_.open_command_buffer();
  if (command_buffer == nullptr) {
    return;
  }
  query_pool_->TakeResolveBatch(query_resolve_batch_);
  if (!query_resolve_batch_.indices.empty() || !query_resolve_batch_.counter_indices.empty()) {
    // RecordResolveBatch writes into a VkCommandBuffer: at the end of the
    // replay of this submission's commands (on the submission thread with
    // native_threaded_submission, which alone uses the query pool's resolve
    // scratch then), after its queries.
    submitter_.RecordAfterReplay(
        [query_pool = query_pool_.get(),
         batch = std::move(query_resolve_batch_)](VkCommandBuffer replay_command_buffer) mutable {
          query_pool->RecordResolveBatch(replay_command_buffer, batch);
        });
    query_resolve_batch_ = {};
  }
  RecordVIZPredicateCopies(command_buffer);
}

bool NativeCommandProcessor::EnsureVIZPredicateBuffer() {
  if (viz_predicate_buffer_ != VK_NULL_HANDLE) {
    return true;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  if (viz_predicate_buffer_failed_ || !vulkan_device->properties().conditionalRendering) {
    return false;
  }
  // One 32-bit value per VIZ ID.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(uint32_t) * uint32_t(viz_queries_.size()),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, viz_predicate_buffer_,
          viz_predicate_memory_)) {
    REXGPU_ERROR("Native renderer: failed to create the VIZ predicate buffer");
    viz_predicate_buffer_ = VK_NULL_HANDLE;
    viz_predicate_memory_ = VK_NULL_HANDLE;
    viz_predicate_buffer_failed_ = true;
    return false;
  }
  return true;
}

void NativeCommandProcessor::RecordVIZPredicateCopies(CommandRecorder* command_buffer) {
  if (viz_pending_copies_.empty() || !query_pool_ || viz_predicate_buffer_ == VK_NULL_HANDLE) {
    viz_pending_copies_.clear();
    return;
  }
  // Earlier draws may still read the predicates, earlier copies write them.
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask =
      VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = viz_predicate_buffer_;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
  // 32-bit results, the query waited for by the copy: non-zero draws.
  for (const PendingVIZCopy& copy : viz_pending_copies_) {
    command_buffer->CmdVkCopyQueryPoolResults(query_pool_->query_pool(), copy.query_index, 1,
                                  viz_predicate_buffer_, VkDeviceSize(copy.id) * sizeof(uint32_t),
                                  sizeof(uint32_t), VK_QUERY_RESULT_WAIT_BIT);
  }
  viz_pending_copies_.clear();
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT, 0, 0, nullptr, 1,
                           &barrier, 0, nullptr);
}

VkBuffer NativeCommandProcessor::BindVIZPredicate(VkDeviceSize& offset_out) {
  if (viz_predicate_buffer_ == VK_NULL_HANDLE || !IsVIZPredicateArmed()) {
    return VK_NULL_HANDLE;
  }
  for (const PendingVIZCopy& copy : viz_pending_copies_) {
    if (copy.id == viz_draw_predicate_.id) {
      // Its value is still to be copied, outside the render pass (everything
      // queued goes with it).
      CommandRecorder* command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kViz);
      if (command_buffer == nullptr) {
        return VK_NULL_HANDLE;
      }
      RecordVIZPredicateCopies(command_buffer);
      break;
    }
  }
  offset_out = VkDeviceSize(viz_draw_predicate_.id) * sizeof(uint32_t);
  return viz_predicate_buffer_;
}

void NativeCommandProcessor::EnsureQueryResources() {
  // VIZ queries need the pool even when the reports are faked.
  if (!context_ready_ || (zpd_mode_ == ZPDMode::kFake && !REXCVAR_GET(occlusion_query_viz))) {
    return;
  }
  if (!query_pool_) {
    query_pool_ = std::make_unique<vulkan::VulkanQueryPool>();
  }
  bool can_recreate = zpd_current_report_.handle == kInvalidReportHandle &&
                      !active_segment_.segment_active && active_query_index_ == UINT32_MAX &&
                      !query_pool_->has_pending_resolve_batch() &&
                      query_resolves_in_flight_.empty();
  if (!query_pool_->EnsureInitialized(GetVulkanDevice(), kQueryPoolCapacity, can_recreate)) {
    LogDrawOnce(kDrawLogQueryPool,
                "the occlusion query pool could not be created, occlusion queries are fake");
  }
}

bool NativeCommandProcessor::IsQueryPoolReady() const {
  return query_pool_ && query_pool_->fbo_initialized();
}

bool NativeCommandProcessor::CanOpenQuery() const {
  // Host occlusion queries live in the draws' render pass.
  return context_ready_ && submitter_.in_render_pass();
}

CommandProcessor::QueryOpenResult NativeCommandProcessor::OpenQuery(bool can_close_submission) {
  (void)can_close_submission;
  if (!query_pool_ || !submitter_.in_render_pass()) {
    return QueryOpenResult::kDeferred;
  }
  if (!query_pool_->has_free_indices()) {
    PumpQueryResolves();
    if (!query_pool_->has_free_indices()) {
      if (zpd_mode_ != ZPDMode::kStrict) {
        return QueryOpenResult::kPoolExhausted;
      }
      // Strict can't guess: the oldest submitted query hands its slot back.
      // One in the open submission can't be waited for in the render pass;
      // the segment opens at a later draw.
      if (query_resolves_in_flight_.empty() ||
          query_resolves_in_flight_.front().submission >= submitter_.current()) {
        return QueryOpenResult::kDeferred;
      }
      submitter_.Await(query_resolves_in_flight_.front().submission);
      PumpQueryResolves();
      if (!query_pool_->has_free_indices()) {
        return QueryOpenResult::kDeferred;
      }
    }
  }
  if (!query_pool_->AcquireQueryIndex(active_query_index_, active_query_generation_)) {
    active_query_index_ = UINT32_MAX;
    active_query_generation_ = 0;
    return QueryOpenResult::kFailed;
  }
  // Reports count samples (some games use the number); VIZ only needs zero
  // or not.
  bool precise = active_segment_.report && GetVulkanDevice()->properties().occlusionQueryPrecise;
  query_pool_->BeginQuery(*submitter_.open_command_buffer(), active_query_index_, precise);
  return QueryOpenResult::kOpened;
}

bool NativeCommandProcessor::CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                                        uint64_t& out_submission) {
  if (!query_pool_ || active_query_index_ == UINT32_MAX || !submitter_.in_render_pass()) {
    return false;
  }
  query_pool_->EndQuery(*submitter_.open_command_buffer(), active_query_index_);
  query_pool_->QueueQueryResolve(active_query_index_);
  // A survey's result as the predicate of the draws under its ID until it
  // resolves (conditional rendering), copied at the next point outside the
  // render pass; a newer survey of the ID replaces a queued copy. Without
  // conditional rendering, the base waits for the result or draws.
  if (viz.generation != kInvalidVIZGeneration && CanArmVIZPredicate(viz.id, viz.generation)) {
    if (EnsureVIZPredicateBuffer()) {
      PendingVIZCopy* queued = nullptr;
      for (PendingVIZCopy& copy : viz_pending_copies_) {
        if (copy.id == viz.id) {
          queued = &copy;
          break;
        }
      }
      if (!queued) {
        queued = &viz_pending_copies_.emplace_back();
        queued->id = viz.id;
      }
      queued->query_index = active_query_index_;
      ArmVIZPredicate(viz.id, viz.generation);
    } else {
      BlockVIZPredicate(viz.id, viz.generation);
    }
  }
  PendingQueryResolve resolve;
  resolve.submission = submitter_.current();
  resolve.query_index = active_query_index_;
  resolve.query_generation = active_query_generation_;
  // The segment's draw scale (a scene drawn at 1x opens segments of its own).
  resolve.scale_area16 = active_segment_.scale_area ? active_segment_.scale_area : scale4_ * scale4_;
  resolve.report_handle = report_handle;
  resolve.viz = viz;
  query_resolves_in_flight_.push_back(resolve);
  out_submission = resolve.submission;
  active_query_index_ = UINT32_MAX;
  active_query_generation_ = 0;
  return true;
}

void NativeCommandProcessor::PumpQueryResolves() {
  if (!query_pool_ || query_resolves_in_flight_.empty()) {
    return;
  }
  uint64_t completed = submitter_.completed();
  if (query_resolves_in_flight_.front().submission > completed) {
    return;
  }
  query_pool_->InvalidateReadback();
  while (!query_resolves_in_flight_.empty() &&
         query_resolves_in_flight_.front().submission <= completed) {
    PendingQueryResolve resolve = query_resolves_in_flight_.front();
    query_resolves_in_flight_.pop_front();
    if (!query_pool_->GenerationMatches(resolve.query_index, resolve.query_generation)) {
      continue;
    }
    XenosZPDReport raw_counts =
        query_pool_->GetQueryReadbackValue(resolve.query_index, false, false);
    query_pool_->ReleaseQueryIndex(resolve.query_index, resolve.query_generation);
    if (resolve.report_handle != kInvalidReportHandle) {
      // Host samples to guest ones: divided by the scale's area, which is
      // in sixteenths (2.25 at 1.5x).
      XenosZPDReport counts16 = raw_counts;
      counts16.z_fail *= 16;
      counts16.z_pass *= 16;
      counts16.stencil_fail *= 16;
      OnZPDQueryResolved(resolve.report_handle, counts16.Normalized(resolve.scale_area16), 1);
    }
    if (resolve.viz.generation != kInvalidVIZGeneration) {
      OnVIZQueryResolved(resolve.viz.id, resolve.viz.generation, raw_counts.z_pass != 0);
    }
  }
}

bool NativeCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                               uint64_t wait_for_submission) {
  if (!context_ready_ || !wait_for_submission) {
    return false;
  }
  // Still in the open submission: submitted now (ending the render pass
  // closes the open segment; the next draw opens another).
  if (wait_for_submission >= submitter_.current() && !submitter_.is_open()) {
    return false;
  }
  if (!submitter_.Await(wait_for_submission)) {
    return false;
  }
  PumpQueryResolves();
  const ZPDReport* report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

void NativeCommandProcessor::AwaitVIZQueryResolve(uint64_t wait_for_submission) {
  if (!context_ready_ || wait_for_submission >= submitter_.current()) {
    return;
  }
  submitter_.Await(wait_for_submission);
  PumpQueryResolves();
}

void NativeCommandProcessor::PollCompletedSubmission() {
  if (context_ready_) {
    PumpQueryResolves();
  }
}

uint64_t NativeCommandProcessor::GetCompletedSubmission() const {
  // completed() polls the fences, which changes nothing the caller sees.
  return context_ready_ ? const_cast<Submitter&>(submitter_).completed() : 0;
}

}  // namespace rex::graphics::native
