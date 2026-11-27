#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native/command_processor.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <bit>
#include <iomanip>
#include <limits>
#include <locale>
#include <type_traits>
#include <fmt/format.h>

REXCVAR_DEFINE_STRING(native_scene_probe_dir, "", "GPU",
    "Ordered title scene probe output folder. Empty rejects probes; configured by the title observer.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_scene_probe_images, false, "GPU",
    "Ordered probes: synchronously export target color/depth as float32 images. "
    "Diagnostic GPU waits; not a temporal-ready scene guarantee.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_scene_probe_tracking, false, "GPU",
    "Record the last real draw's host viewport/NDC conversion for ordered probes. Configured by title diagnostics.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_scene_probe_limit, 64, "GPU", "Maximum persisted scene probes per context.")
    .range(1, 1024).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {
namespace {
template <class Range> void Array(std::ostream& file, const Range& values) {
  file << '['; bool first = true;
  for (auto value : values) {
    if (!first) file << ','; first = false;
    if constexpr (std::is_floating_point_v<decltype(value)>) {
      if (!std::isfinite(value)) { file << "null"; continue; }
    }
    file << value;
  }
  file << ']';
}
void Target(std::ostream& file, const RenderTarget* target) {
  if (!target) { file << "null"; return; }
  file << "{\"id\":" << target->id << ",\"base_tiles\":" << target->base_tiles
       << ",\"pitch\":" << target->pitch << ",\"guest_format\":" << target->format
       << ",\"depth\":" << (target->depth ? "true" : "false")
       << ",\"vk_format\":" << uint32_t(target->vk_format) << ",\"samples\":" << uint32_t(target->samples)
       << ",\"host_width\":" << target->host_width << ",\"host_height\":" << target->host_height
       << ",\"scale4\":" << target->scale4 << ",\"fixed16\":" << (target->fixed16 ? "true" : "false") << '}';
}
}  // namespace

bool NativeCommandProcessor::OnTitleSceneMarker(const title::SceneMarker& marker) {
  const bool frame_ok = ProcessSceneFrameMarker(marker);
  if (!(marker.flags & title::kSceneCapture)) return frame_ok;
  const auto directory = REXCVAR_GET(native_scene_probe_dir);
  if (directory.empty() || scene_probe_count_ >= uint64_t(REXCVAR_GET(native_scene_probe_limit))) return false;
  const auto ordinal = ++scene_probe_count_;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) { REXGPU_ERROR("Scene probe: create directory: {}", error.message()); return false; }
  const auto stem = fmt::format("scene-{}-{}-{}-{}", marker.session, marker.frame, uint32_t(marker.point), ordinal);
  const auto base = std::filesystem::path(directory) / stem;
  std::ofstream file(base.string() + ".json");
  if (!file) return false;
  file.imbue(std::locale::classic());
  file << std::setprecision(std::numeric_limits<float>::max_digits10);
  const auto& regs = *register_file_;
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto depth = regs.Get<reg::RB_DEPTH_INFO>();
  std::array<RenderTarget*, 4> colors{};
  for (size_t i = 0; i < 4; ++i) {
    const auto color = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    colors[i] = render_targets_.Find(color.color_base, uint32_t(color.color_format), false,
                                    surface.surface_pitch, surface.msaa_samples);
  }
  auto* depth_target = render_targets_.Find(depth.depth_base, uint32_t(depth.depth_format), true,
                                           surface.surface_pitch, surface.msaa_samples);
  file << "{\"schema\":1,\"snapshot_serial\":" << ordinal << ",\"session\":" << marker.session << ",\"title_frame\":" << marker.frame
       << ",\"native_frame\":" << frame_ + 1 << ",\"point\":" << uint32_t(marker.point)
       << ",\"source_hash\":" << marker.source_hash << ",\"flags\":" << marker.flags
       << ",\"view_id\":" << marker.view_id << ",\"delta_time\":" << marker.delta_time
       << ",\"reset_reason\":" << marker.reset_reason << ",\"camera_register\":" << marker.camera_register
       << ",\"locked_distance\":" << marker.locked_distance
       << ",\"guest_width\":" << marker.width << ",\"guest_height\":" << marker.height
       << ",\"candidate_camera\":" << ((marker.flags & title::kSceneCamera) ? "true" : "false");
  // Camera words in contract order (view 12, projection 16, view_projection 16).
  std::array<uint32_t, 44> camera_words{};
  for (size_t i = 0; i < 12; ++i) camera_words[i] = std::bit_cast<uint32_t>(marker.camera.view[i]);
  for (size_t i = 0; i < 16; ++i) camera_words[12 + i] = std::bit_cast<uint32_t>(marker.camera.projection[i]);
  for (size_t i = 0; i < 16; ++i) camera_words[28 + i] = std::bit_cast<uint32_t>(marker.camera.view_projection[i]);
  file << ",\"cpu_camera_words\":"; Array(file, camera_words);
  // First 44 VS float constant words as bound (native binding base 0), and
  // the title-declared view_projection registers.
  std::array<uint32_t, 44> actual_constants{};
  for (size_t i = 0; i < 44; ++i) actual_constants[i] = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + i];
  file << ",\"native_binding_base\":0,\"gpu_camera_words\":"; Array(file, actual_constants);
  bool pv_match = marker.camera_register < 252 && (marker.flags & title::kSceneCamera);
  for (size_t i = 0; pv_match && i < 16; ++i)
    pv_match = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + marker.camera_register * 4 + i] ==
               std::bit_cast<uint32_t>(marker.camera.view_projection[i]);
  file << ",\"camera_words_match\":" << (pv_match ? "true" : "false");
  bool shader_io_ok = true;
  auto shader = [&](const Shader* program, const char* name) {
    file << ",\"" << name << "\":";
    if (!program) { file << "null"; return; }
    file << "{\"hash\":" << program->ucode_data_hash() << ",\"words\":" << program->ucode_dword_count()
         << ",\"analyzed\":" << (program->is_ucode_analyzed() ? "true" : "false");
    if (program->is_ucode_analyzed()) {
      file << ",\"float_bitmap\":"; Array(file, program->constant_register_map().float_bitmap);
      file << ",\"float_dynamic\":" << (program->constant_register_map().float_dynamic_addressing ? "true" : "false");
    }
    file << '}';
    std::ofstream code(base.string() + "." + name + ".u32le", std::ios::binary);
    code.write(reinterpret_cast<const char*>(program->ucode_dwords()), std::streamsize(program->ucode_dword_count() * 4));
    code.flush(); shader_io_ok = bool(code) && shader_io_ok;
    if (program->is_ucode_analyzed()) {
      std::ofstream assembly(base.string() + "." + name + ".ucode.txt");
      assembly << program->ucode_disassembly();
      assembly.flush(); shader_io_ok = bool(assembly) && shader_io_ok;
    }
  };
  shader(active_vertex_shader_, "vs"); shader(active_pixel_shader_, "ps");
  file << ",\"texture_fetches\":[";
  bool first_fetch = true;
  for (const Shader* program : {active_vertex_shader_, active_pixel_shader_}) {
    if (!program || !program->is_ucode_analyzed()) continue;
    for (const auto& binding : program->texture_bindings()) {
      const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
      if (!first_fetch) file << ','; first_fetch = false;
      file << "{\"stage\":\"" << (program == active_vertex_shader_ ? "vs" : "ps")
           << "\",\"slot\":" << binding.fetch_constant << ",\"base\":" << (uint32_t(fetch.base_address) << 12)
           << ",\"format\":" << uint32_t(fetch.format) << ",\"dimension\":" << uint32_t(fetch.dimension)
           << ",\"width_2d\":" << uint32_t(fetch.size_2d.width) + 1 << ",\"height_2d\":" << uint32_t(fetch.size_2d.height) + 1;
      const auto* resolved = resolver_.Find(uint32_t(fetch.base_address) << 12);
      file << ",\"resolved_candidate\":";
      if (resolved) {
        file << "{\"id\":" << resolved->id << ",\"serial\":" << resolved->serial << ",\"from_depth\":"
             << (resolved->from_depth ? "true" : "false") << ",\"depth_float24\":"
             << (resolved->depth_float24 ? "true" : "false") << ",\"host_width\":" << resolved->host_width
             << ",\"host_height\":" << resolved->host_height << '}';
      } else file << "null";
      file << '}';
    }
  }
  file << ']';
  file << ",\"last_recorded_draw\":{\"frame\":" << scene_last_draw_frame_
       << ",\"ordinal\":" << scene_last_draw_ordinal_ << ",\"vs_hash\":" << scene_last_vs_hash_
       << ",\"ps_hash\":" << scene_last_ps_hash_ << ",\"system_flags\":" << scene_last_system_flags_
       << ",\"ndc_scale\":"; Array(file, scene_last_ndc_scale_);
  file << ",\"ndc_offset\":"; Array(file, scene_last_ndc_offset_);
  file << ",\"jittered\":" << (scene_last_jittered_ ? "true" : "false");
  file << ",\"viewport\":"; Array(file, scene_last_viewport_);
  file << '}';
  file << ",\"color_mask\":" << regs[XE_GPU_REG_RB_COLOR_MASK]
       << ",\"depth_control\":" << regs[XE_GPU_REG_RB_DEPTHCONTROL] << ",\"colors\":[";
  for (size_t i = 0; i < 4; ++i) { if (i) file << ','; Target(file, colors[i]); }
  file << "],\"depth\":"; Target(file, depth_target);
  bool color_exported = false, depth_exported = false;
  if (REXCVAR_GET(native_scene_probe_images)) {
    if (colors[0] && marker.point != title::ScenePoint::kView)
      color_exported = ExportSceneTarget(*colors[0], marker.width, marker.height, base.string() + ".color.rgba32f");
    if (depth_target && (marker.point == title::ScenePoint::kSceneEnd || marker.point == title::ScenePoint::kProbe))
      depth_exported = ExportSceneTarget(*depth_target, marker.width, marker.height, base.string() + ".depth.r32f");
  }
  file << ",\"color_exported\":" << (color_exported ? "true" : "false")
       << ",\"depth_exported\":" << (depth_exported ? "true" : "false")
       << ",\"export_width\":" << (colors[0] ? ScaleSize(marker.width, colors[0]->scale4) : 0)
       << ",\"export_height\":" << (colors[0] ? ScaleSize(marker.height, colors[0]->scale4) : 0)
       << ",\"depth_export_width\":" << (depth_target ? ScaleSize(marker.width, depth_target->scale4) : 0)
       << ",\"depth_export_height\":" << (depth_target ? ScaleSize(marker.height, depth_target->scale4) : 0)
       << ",\"image_encoding\":\"float32 little endian, row-major, color RGBA / depth R\""
       << ",\"temporal_inputs_ready\":false,\"qualification\":\"ordered diagnostic snapshot; "
          "target existence does not prove semantic ownership or complete depth; no async image lease\"}\n";
  file.flush();
  REXGPU_INFO("Scene probe: title frame {} native frame {} point {}; VS {:016X}, camera match {}; output {}",
      marker.frame, frame_ + 1, uint32_t(marker.point), active_vertex_shader_ ? active_vertex_shader_->ucode_data_hash() : 0,
      pv_match, base.string());
  return bool(file) && shader_io_ok && frame_ok;
}

bool NativeCommandProcessor::ProcessSceneFrameMarker(const title::SceneMarker& marker) {
  const auto directory = std::filesystem::path(REXCVAR_GET(native_scene_probe_dir));
  if (!SceneFramesEnabled()) return true;
  if (scene_frames_init_failed_) return false;
  if (marker.view_id) return true;  // One main view per frame for now; others keep their original execution.
  if (!scene_frames_) {
    if (!directory.empty()) {
      std::error_code error; std::filesystem::create_directories(directory, error);
      if (error) {
        scene_frames_init_failed_ = true;
        REXGPU_ERROR("Native scene frames directory: {}", error.message()); return false;
      }
    }
    auto frames = std::make_unique<SceneFrames>();
    if (!frames->Initialize(submitter_, render_targets_)) {
      scene_frames_init_failed_ = true; REXGPU_ERROR("Native scene frames initialization failed"); return false;
    }
    scene_frames_ = std::move(frames);
    scene_frames_->SetObjectMotion(object_motion_.get());
  }
  const auto& regs = *register_file_;
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto color_info = regs.Get<reg::RB_COLOR_INFO>();
  const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto* color = render_targets_.Find(color_info.color_base, uint32_t(color_info.color_format), false, surface.surface_pitch, surface.msaa_samples);
  auto* depth = render_targets_.Find(depth_info.depth_base, uint32_t(depth_info.depth_format), true, surface.surface_pitch, surface.msaa_samples);
  // Frame generation: the frame's rectangle and flags from any of its markers.
  scene_frames_->NoteFrameMarker(marker);
  scene_marker_session_ = marker.session; scene_marker_frame_ = marker.frame;
  switch (marker.point) {
    case title::ScenePoint::kView: {
      // A later kView of the same frame replaces an accepted one; a refused
      // one keeps it (a producer may annotate several draws of a frame, one
      // of them a draw the backend skipped, e.g. its pipeline still building).
      const bool frame_view = scene_frames_->Matches(marker.session, marker.frame);
      if (!(marker.flags & title::kSceneCamera)) { scene_frames_->InvalidateView(); break; }
      // The annotation must describe the draw that was just executed. With a
      // camera register the shader must be that draw's (it is checked);
      // without, a draw of this frame recorded with the same targets will do.
      const bool shader_proof = marker.camera_register != title::kNoRegister;
      const char* reject = !color || !depth ? "view-targets" :
          !active_vertex_shader_ || !active_vertex_shader_->is_ucode_analyzed() ? "view-shader" :
          scene_last_draw_frame_ != frame_ + 1 || !scene_last_draw_ordinal_ ||
          (shader_proof && scene_last_vs_hash_ != active_vertex_shader_->ucode_data_hash()) ? "view-last-draw" : nullptr;
      if (!reject && marker.camera_register != title::kNoRegister) {
        // Optional proof: the shader reads these registers and they hold the payload.
        const auto& map = active_vertex_shader_->constant_register_map();
        const uint32_t base = marker.camera_register;
        bool used = base <= 252 && !map.float_dynamic_addressing;
        for (uint32_t r = base; used && r < base + 4; ++r) used = (map.float_bitmap[r >> 6] >> (r & 63)) & 1;
        for (uint32_t i = 0; used && i < 16; ++i)
          used = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + base * 4 + i] ==
                 std::bit_cast<uint32_t>(marker.camera.view_projection[i]);
        if (!used) reject = "view-camera-register";
      }
      if (!reject && !scene::CameraConsistent(marker.camera.view, marker.camera.projection, marker.camera.view_projection))
        reject = "view-camera-inconsistent";
      // The viewport: the marker's size; anywhere in the target (letterboxed scenes) on whole pixels.
      const float origin_x = scene_last_viewport_[0], origin_y = scene_last_viewport_[1];
      if (!reject && (origin_x < 0 || origin_y < 0 || origin_x != std::floor(origin_x) ||
          origin_y != std::floor(origin_y) ||
          scene_last_viewport_[2] != float(ScaleSize(marker.width, color->scale4)) ||
          scene_last_viewport_[3] != float(ScaleSize(marker.height, color->scale4)) ||
          origin_x + scene_last_viewport_[2] > float(color->host_width) ||
          origin_y + scene_last_viewport_[3] > float(color->host_height))) reject = "view-viewport";
      scene::Matrix inverse;
      if (!reject && !scene::Invert(marker.camera.view_projection, inverse)) reject = "view-singular";
      if (reject) {
        if (frame_view) scene_frames_->NoteRejected(reject); else scene_frames_->RejectView(reject);
        break;
      }
      scene::View view; view.session = marker.session; view.frame = marker.frame; view.view_id = marker.view_id;
      view.guest_width = marker.width; view.guest_height = marker.height; view.scale4 = color->scale4;
      view.width = ScaleSize(marker.width, color->scale4); view.height = ScaleSize(marker.height, color->scale4);
      view.origin_x = uint32_t(scene_last_viewport_[0]); view.origin_y = uint32_t(scene_last_viewport_[1]);
      view.guest_origin_x = view.width ? uint32_t(std::lround(double(view.origin_x) * marker.width / view.width)) : 0;
      view.guest_origin_y = view.height ? uint32_t(std::lround(double(view.origin_y) * marker.height / view.height)) : 0;
      view.color_id = color->id; view.depth_id = depth->id; view.color_format = color->format; view.depth_format = depth->format;
      view.ndc_scale = scene_last_ndc_scale_; view.ndc_offset = scene_last_ndc_offset_;
      view.depth_range = {scene_last_viewport_[4], scene_last_viewport_[5]};
      view.jitter = scene_last_jittered_ ? scene_jitter_.pixels : std::array<float, 2>{};
      view.view = marker.camera.view; view.projection = marker.camera.projection; view.pv = marker.camera.view_projection;
      view.delta_time = marker.delta_time; view.locked_distance = marker.locked_distance; view.meters_per_unit = marker.meters_per_unit;
      view.title_reset = marker.flags & title::kSceneReset; view.title_reset_reason = marker.reset_reason;
      view.auto_opaque = marker.flags & title::kSceneAutoOpaque;
      scene_frames_->BeginView(view);
      NoteTitleSceneViewAccepted(marker.source_hash);
      scene_swaps_since_view_ = 0;
      if (REXCVAR_GET(native_scene_upscale)) {
        // The scene's surfaces: drawn at 1x in the next frames' scene interval.
        render_targets_.MarkScene(RenderTargets::SurfaceKey(*color));
        render_targets_.MarkScene(RenderTargets::SurfaceKey(*depth));
      }
      break;
    }
    case title::ScenePoint::kOpaqueEnd:
      if (scene_frames_->Matches(marker.session, marker.frame) && color) scene_frames_->SnapshotOpaque(*color);
      break;
    case title::ScenePoint::kSceneEnd:
      // Jitter stops here: post-processing and the HUD are not jittered; nor
      // captured for object motion (Retain draws this frame's captures).
      scene_jitter_.armed = false;
      if (object_motion_) object_motion_->Disarm();
      if (scene_frames_->Matches(marker.session, marker.frame) && color && depth) {
        // A title may resolve a float target through an alias in another format
        // at the same EDRAM base, bound by the time a late kSceneEnd (after the
        // resolve) arrives: the scene is the view's surface there, else a format
        // flip resets the history twice.
        RenderTarget* scene_color = color;
        if (const auto* view = scene_frames_->PendingView();
            view && color->id != view->color_id && color->format != view->color_format) {
          RenderTarget* original = render_targets_.Find(color_info.color_base, view->color_format, false,
                                                        surface.surface_pitch, surface.msaa_samples);
          if (original && original->id == view->color_id && original->layout != VK_IMAGE_LAYOUT_UNDEFINED)
            scene_color = original;
        }
        scene_frames_->Retain(*scene_color, *depth);
      }
      // Post-processing and the HUD at the scale (the upscaler's output is there).
      render_targets_.SetSceneInterval(false);
      break;
    case title::ScenePoint::kPostEnd: {
      render_targets_.SetSceneInterval(false);  // kSceneEnd was lost: the HUD at the scale.
      if (scene_frames_->Matches(marker.session, marker.frame) && color)
        scene_frames_->PresentDebug(*color, ScaleSize(marker.width, color->scale4), ScaleSize(marker.height, color->scale4),
                                   regs[XE_GPU_REG_RB_COLOR_MASK] & 15u);
      // The frame without the HUD, for frame generation while the presenter runs it.
      const ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
      if (color && presenter && presenter->IsFrameGenerationActive() &&
          scene_frames_->Matches(marker.session, marker.frame))
        scene_frames_->SnapshotHudless(*color, marker.width, marker.height);
      break;
    }
    case title::ScenePoint::kProbe:
      break;  // Diagnostic snapshot only (OnTitleSceneMarker).
    case title::ScenePoint::kLockedVolume:
      if (scene_frames_->Matches(marker.session, marker.frame)) scene_frames_->AddLockedVolume(marker.locked_box);
      break;
    case title::ScenePoint::kFrameEnd: {
      if ((marker.flags & title::kSceneCapture) && !directory.empty()) {
        const auto base = directory / fmt::format("frame-inputs-{}-{}", marker.session, marker.frame);
        if (scene_frames_->Matches(marker.session, marker.frame) && REXCVAR_GET(native_scene_frame_images)) scene_frames_->Export(base);
        scene_frames_->Report(base.string() + ".json");
      }
      // The host may hard-exit after title termination; persist during the
      // stream too rather than relying on ShutdownContext being reached.
      scene_frames_->NoteJitterDraws(scene_jitter_draws_);
      if (!directory.empty() && ((marker.flags & title::kSceneCapture) || marker.frame == 1 || marker.frame % 300 == 0))
        scene_frames_->Report(directory / "frame-consumer.json");
      // Jitter the next frame's scene draws into the targets this frame retained.
      scene_jitter_ = {};
      // Explicit phase count, else what the temporal upscaler asks for.
      uint32_t phases = uint32_t(std::max(REXCVAR_GET(native_scene_jitter), 0));
      if (!phases) phases = scene_frames_->UpscalerJitterPhases();
      if (const auto* last = scene_frames_->LastView();
          phases && last && last->session == marker.session && last->frame == marker.frame) {
        scene_jitter_.armed = true; scene_jitter_.depth_id = last->depth_id;
        scene_jitter_.width = last->width; scene_jitter_.height = last->height;
        scene_jitter_.origin_x = last->origin_x; scene_jitter_.origin_y = last->origin_y;
        scene_jitter_.pixels = scene::Jitter(marker.frame + 1, phases);
      }
      // Object motion: captures this frame did not retain are dropped; the next
      // frames' scene draws (also after frames without a scene) are captured.
      if (object_motion_) {
        object_motion_->DropFrame();
        object_motion_->Disarm();
        if (const auto* last = scene_frames_->LastView();
            last && last->session == marker.session && last->frame <= marker.frame && last->frame + 2 >= marker.frame)
          object_motion_->Arm(last->depth_id, last->origin_x, last->origin_y, last->width, last->height);
      }
      // Reopened at the swap; after it (a late kFrameEnd) the next frame's is open.
      if (marker.session != scene_swapped_session_ || marker.frame != scene_swapped_frame_)
        render_targets_.SetSceneInterval(false);
      scene_frames_->EndFrame(marker.session, marker.frame);
      scene_frames_->InvalidateView();
      break;
    }
  }
  return true;  // Skips/failures have their own frame report; never discard guest work.
}

bool NativeCommandProcessor::ExportSceneTarget(RenderTarget& source, uint32_t width, uint32_t height,
                                               const std::filesystem::path& path) {
  // Owned synchronous debug copy, not an async lease of mutable EDRAM.
  if (!width || !height || width > source.surface.width || height > source.surface.height ||
      source.layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
  if (!scene_resolve_pass_) {
    auto pass = std::make_unique<ResolvePass>();
    if (!pass->Initialize(submitter_, render_targets_)) return false;
    scene_resolve_pass_ = std::move(pass);
  }
  ResolvedTexture dest;
  dest.vk_format = source.depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
  dest.host_width = ScaleSize(width, source.scale4); dest.host_height = ScaleSize(height, source.scale4);
  dest.attachment = true;
  const auto* device = GetVulkanDevice(); const auto& fn = device->functions(); const VkDevice vk = device->device();
  VkBuffer staging = VK_NULL_HANDLE; VkDeviceMemory staging_memory = VK_NULL_HANDLE; void* mapping = nullptr;
  const VkDeviceSize byte_count = VkDeviceSize(dest.host_width) * dest.host_height * (source.depth ? 4 : 16);
  if (byte_count > (VkDeviceSize(256) << 20)) return false;
  bool success = false, recorded = false;
  do {
    if (!scene_resolve_pass_->CanDrawInto(dest.vk_format)) break;
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((render_targets_.OptimalTilingFeatures(dest.vk_format) & required) != required) break;
    VkImageCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; info.imageType = VK_IMAGE_TYPE_2D;
    info.format = dest.vk_format; info.extent = {dest.host_width, dest.host_height, 1};
    info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                         dest.image, dest.memory)) break;
    VkImageViewCreateInfo view{}; view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; view.image = dest.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = dest.vk_format; view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
    if (fn.vkCreateImageView(vk, &view, nullptr, &dest.view) != VK_SUCCESS) break;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(device, byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        ui::vulkan::util::MemoryPurpose::kReadback, staging, staging_memory)) break;
    if (fn.vkMapMemory(vk, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) break;
    const Rect rect{0, 0, int32_t(dest.host_width), int32_t(dest.host_height)};
    recorded = true;  // Draw may record transitions even if it later refuses.
    if (!scene_resolve_pass_->Draw(source, dest, rect, source.fixed16 ? 32.f : 1.f,
                                  source.depth ? 1 : ((1u << uint32_t(source.samples)) - 1))) break;
    auto* commands = submitter_.OpenOutsideRenderPass();
    if (!commands) break;
    TransitionTrackedImage(fn, commands, dest.image, VK_IMAGE_ASPECT_COLOR_BIT, dest.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* region = commands->CmdCopyImageToBufferEmplace(dest.image, dest.layout, staging, 1);
    *region = {}; region->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region->imageSubresource.layerCount = 1;
    region->imageExtent = {dest.host_width, dest.host_height, 1};
    VkMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    commands->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (!submitter_.Await(submitter_.current())) break;
    VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = staging_memory; range.size = VK_WHOLE_SIZE;
    if (fn.vkInvalidateMappedMemoryRanges(vk, 1, &range) != VK_SUCCESS) break;
    std::ofstream output(path, std::ios::binary); output.write(static_cast<const char*>(mapping), std::streamsize(byte_count));
    output.flush(); success = bool(output);
  } while (false);
  // On any post-record failure, retain every resource until recorded uses finish.
  if (recorded) submitter_.AwaitAll();
  if (mapping) fn.vkUnmapMemory(vk, staging_memory);
  if (staging) fn.vkDestroyBuffer(vk, staging, nullptr);
  if (staging_memory) fn.vkFreeMemory(vk, staging_memory, nullptr);
  if (dest.framebuffer) fn.vkDestroyFramebuffer(vk, dest.framebuffer, nullptr);
  if (dest.view) fn.vkDestroyImageView(vk, dest.view, nullptr);
  if (dest.image) fn.vkDestroyImage(vk, dest.image, nullptr);
  if (dest.memory) fn.vkFreeMemory(vk, dest.memory, nullptr);
  return success;
}
}  // namespace rex::graphics::native
