/**
 * @file        graphics/native/shaders.cpp
 * @brief       Native renderer: guest shaders translated to SPIR-V
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The modifications follow the SDK's VulkanPipelineCache
 *              (GetCurrentVertexShaderModification, GetCurrentPixelShaderModification;
 *              Xenia, Copyright 2022 Ben Vanik, BSD license) for host render targets.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/shaders.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>
#include <rex/math.h>

namespace rex::graphics::native {

Shaders::~Shaders() {
  Shutdown();
}

bool Shaders::Initialize(const ui::vulkan::VulkanDevice* vulkan_device, uint32_t scale4) {
  vulkan_device_ = vulkan_device;
  scale4_ = scale4;
  translator_ = CreateTranslator();
  worker_exit_ = false;
  worker_ = std::thread([this]() { WorkerMain(); });
  return true;
}

std::unique_ptr<SpirvShaderTranslator> Shaders::CreateTranslator() const {
  // Host render targets: no interlock. 2x MSAA is native (render targets of
  // 2 samples), with and without attachments. The scale makes pixel positions
  // (param gen) guest pixels and resolution-scaled textures' sizes host ones;
  // given in quarters, the whole scale of the constructor (for interlock's
  // EDRAM layout only) is just rounded up.
  uint32_t whole_scale = (scale4_ + 3) / 4;
  auto translator = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device_), true, true, false, whole_scale, whole_scale,
      xenos::kEdramTileCount);
  translator->SetDrawResolutionScale4(scale4_, scale4_);
  translator->SetMotionCapture(motion_capture_);
  return translator;
}

bool Shaders::TranslateNow(SpirvShaderTranslator& translator,
                           vulkan::VulkanShader::VulkanTranslation& translation) {
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("Native renderer: shader {:016X} (modification {:016X}) failed to translate",
                 translation.shader().ucode_data_hash(), translation.modification());
    return false;
  }
  return translation.is_valid() && translation.GetOrCreateShaderModule() != VK_NULL_HANDLE;
}

void Shaders::WorkerMain() {
  std::unique_ptr<SpirvShaderTranslator> translator = CreateTranslator();
  while (true) {
    TranslationJob job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      jobs_cv_.wait(lock, [this]() { return worker_exit_ || !jobs_.empty(); });
      if (worker_exit_) {
        return;
      }
      job = jobs_.front();
      jobs_.pop_front();
      worker_busy_ = true;
    }
    job.state->succeeded = TranslateNow(*translator, *job.translation);
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      job.state->done.store(true, std::memory_order_release);
      worker_busy_ = false;
    }
    done_cv_.notify_all();
  }
}

void Shaders::StopWorker() {
  if (!worker_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    worker_exit_ = true;
  }
  jobs_cv_.notify_all();
  worker_.join();
  // Not started: their translations stay not made (the shaders go next).
  jobs_.clear();
  worker_busy_ = false;
}

void Shaders::AwaitIdle() {
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  done_cv_.wait(lock, [this]() { return !worker_.joinable() || (jobs_.empty() && !worker_busy_); });
}

void Shaders::TranslateBatch(
    const std::vector<std::pair<vulkan::VulkanShader*, uint64_t>>& translations) {
  // Nothing of the background thread's in flight: its translations would
  // share shaders with these.
  AwaitIdle();
  // By shader: one thread translates all of a shader's (its first one sets
  // the bindings), creating the translation objects too.
  std::unordered_map<vulkan::VulkanShader*, std::vector<uint64_t>> by_shader;
  for (const auto& [shader, modification] : translations) {
    by_shader[shader].push_back(modification);
  }
  std::vector<std::pair<vulkan::VulkanShader*, std::vector<uint64_t>>> work(by_shader.begin(),
                                                                           by_shader.end());
  if (work.empty()) {
    return;
  }
  std::atomic<size_t> next{0};
  std::atomic<size_t> translated{0};
  auto translate_function = [&]() {
    std::unique_ptr<SpirvShaderTranslator> translator = CreateTranslator();
    while (true) {
      size_t index = next.fetch_add(1);
      if (index >= work.size()) {
        return;
      }
      vulkan::VulkanShader& shader = *work[index].first;
      for (uint64_t modification : work[index].second) {
        auto* translation = static_cast<vulkan::VulkanShader::VulkanTranslation*>(
            shader.GetOrCreateTranslation(modification));
        if (translation && !translation->is_translated() && TranslateNow(*translator, *translation)) {
          translated.fetch_add(1);
        }
      }
    }
  };
  uint32_t thread_count = std::clamp(rex::thread::logical_processor_count() * 3 / 4, uint32_t(1),
                                     uint32_t(std::min(work.size(), size_t(16))));
  std::vector<std::thread> threads;
  for (uint32_t i = 1; i < thread_count; ++i) {
    threads.emplace_back(translate_function);
  }
  // This thread too.
  translate_function();
  for (std::thread& thread : threads) {
    thread.join();
  }
}

VkShaderModule Shaders::GetDepthOnlyFloat24Shader(bool round) {
  uint32_t index = round ? 1 : 0;
  if (depth_only_float24_attempted_[index] || !translator_) {
    return depth_only_float24_[index];
  }
  depth_only_float24_attempted_[index] = true;
  using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
  // Resets the translator's state: the command processor thread's own.
  std::vector<uint8_t> code = translator_->CreateDepthOnlyFragmentShader(
      round ? DepthStencilMode::kFloat24Rounding : DepthStencilMode::kFloat24Truncating);
  depth_only_float24_[index] = ui::vulkan::util::CreateShaderModule(
      vulkan_device_, reinterpret_cast<const uint32_t*>(code.data()), code.size());
  if (depth_only_float24_[index] == VK_NULL_HANDLE) {
    REXGPU_ERROR("Native renderer: failed to create the float24 depth-only fragment shader");
  }
  return depth_only_float24_[index];
}

void Shaders::Shutdown() {
  // Before the translations it writes are destroyed.
  StopWorker();
  if (vulkan_device_) {
    for (uint32_t i = 0; i < 2; ++i) {
      ui::vulkan::util::DestroyAndNullHandle(vulkan_device_->functions().vkDestroyShaderModule,
                                             vulkan_device_->device(), depth_only_float24_[i]);
      depth_only_float24_attempted_[i] = false;
    }
  }
  async_translations_.clear();
  // The caller has waited for the GPU: the translations destroy their modules.
  for (auto& shaders : shaders_) {
    shaders.clear();
  }
  translator_.reset();
  vulkan_device_ = nullptr;
}

vulkan::VulkanShader* Shaders::Load(xenos::ShaderType shader_type, const uint32_t* host_address,
                                    uint32_t dword_count) {
  const uint64_t hash = XXH3_64bits(host_address, size_t(dword_count) * sizeof(uint32_t));
  auto& shaders = shaders_[shader_type == xenos::ShaderType::kPixel ? 1 : 0];
  auto it = shaders.find(hash);
  if (it != shaders.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<vulkan::VulkanShader>(vulkan_device_, shader_type, hash,
                                                       host_address, dword_count);
  // Analysed now: the base command processor reads the memory export use and
  // frame traces the microcode.
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  vulkan::VulkanShader* result = shader.get();
  shaders.emplace(hash, std::move(shader));
  return result;
}

vulkan::VulkanShader* Shaders::Find(xenos::ShaderType shader_type,
                                    uint64_t ucode_data_hash) const {
  const auto& shaders = shaders_[shader_type == xenos::ShaderType::kPixel ? 1 : 0];
  auto it = shaders.find(ucode_data_hash);
  return it != shaders.end() ? it->second.get() : nullptr;
}

SpirvShaderTranslator::Modification Shaders::GetVertexModification(
    const RegisterFile& regs, const Shader& shader,
    Shader::HostVertexShaderType host_vertex_shader_type, uint32_t interpolator_mask,
    bool ps_param_gen_used) const {
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device_->properties();
  SpirvShaderTranslator::Modification modification(translator_->GetDefaultVertexShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;
  modification.vertex.tessellation_mode =
      Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)
          ? uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode)
          : 0;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_cull = 0;
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && device_properties.shaderCullDistance) {
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance) {
      // Cull distances when there are no clip distances, else none.
      if (device_properties.shaderCullDistance) {
        modification.vertex.user_clip_plane_cull = 1;
      } else {
        user_clip_planes = 0;
      }
    }
  }
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;

  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and = uint32_t(
      device_properties.shaderCullDistance &&
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);
  return modification;
}

SpirvShaderTranslator::Modification Shaders::GetPixelModification(
    const RegisterFile& regs, const Shader& shader, uint32_t interpolator_mask,
    uint32_t param_gen_pos, reg::RB_DEPTHCONTROL normalized_depth_control,
    bool polygon_offset_in_shader, bool depth_float24_convert) const {
  SpirvShaderTranslator::Modification modification(translator_->GetDefaultPixelShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.contraction_allowed = !REXCVAR_GET(spirv_pixel_shader_no_contraction);
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  // Depth as the host computes it (no float24 conversion), with the polygon
  // offset added by the shader where the caller chose that
  // (draw_util::GetHostDepthPolygonOffsetIfNeeded). Early tests when nothing
  // in the shader or alpha-dependent coverage forbids them.
  using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
  if (depth_float24_convert && normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    // The guest's float24 depth exactly, as the SDK's Vulkan backend does it
    // with depth_float24_convert_in_pixel_shader (no early depth test then).
    bool round = REXCVAR_GET(depth_float24_round);
    modification.pixel.depth_stencil_mode =
        polygon_offset_in_shader
            ? (round ? DepthStencilMode::kFloat24RoundingPolygonOffset
                     : DepthStencilMode::kFloat24TruncatingPolygonOffset)
            : (round ? DepthStencilMode::kFloat24Rounding : DepthStencilMode::kFloat24Truncating);
  } else if (polygon_offset_in_shader) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kPolygonOffset;
  } else if (shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Host MIN / MAX blending ignores the factors, the Xenos applies them: a
  // source factor of source alpha (with destination one) is multiplied into
  // the output instead (render target 0, as the SDK).
  modification.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
  if (shader.writes_color_target(0)) {
    auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.color_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
    }
    if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
    }
  }
  return modification;
}

vulkan::VulkanShader::VulkanTranslation* Shaders::Translate(vulkan::VulkanShader& shader,
                                                           uint64_t modification,
                                                           bool allow_async, uint32_t wait_ms,
                                                           bool* pending_out) {
  if (pending_out) {
    *pending_out = false;
  }
  auto* translation = static_cast<vulkan::VulkanShader::VulkanTranslation*>(
      shader.GetOrCreateTranslation(modification));
  if (!translation) {
    return nullptr;
  }
  // Queued to the background thread (now or before): only read once done.
  auto async_it = async_translations_.find(translation);
  if (async_it == async_translations_.end() && !translation->is_translated() && allow_async &&
      worker_.joinable()) {
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer_);
    }
    async_it = async_translations_.emplace(translation, std::make_unique<AsyncTranslation>()).first;
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back(TranslationJob{translation, async_it->second.get()});
    }
    jobs_cv_.notify_one();
  }
  if (async_it != async_translations_.end()) {
    AsyncTranslation& state = *async_it->second;
    if (!state.done.load(std::memory_order_acquire)) {
      bool done = false;
      if (wait_ms) {
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        done = done_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&state]() {
          return state.done.load(std::memory_order_acquire);
        });
      }
      if (!done) {
        if (pending_out) {
          *pending_out = true;
        }
        return nullptr;
      }
    }
    return state.succeeded ? translation : nullptr;
  }
  if (!translation->is_translated()) {
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer_);
    }
    if (!translator_->TranslateAnalyzedShader(*translation)) {
      REXGPU_ERROR("Native renderer: shader {:016X} (modification {:016X}) failed to translate",
                   shader.ucode_data_hash(), modification);
      return nullptr;
    }
  }
  if (!translation->is_valid() || translation->GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return nullptr;
  }
  return translation;
}

}  // namespace rex::graphics::native
