/**
 * @file        graphics/d3d11/pipeline_cache.cpp
 * @brief       Shaders and fixed-function state of the experimental Direct3D 11
 *              backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/pipeline_cache.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <xxhash.h>

REXCVAR_DEFINE_INT32(d3d11_shader_creation_threads, -1, "GPU/D3D11",
                     "Threads that create the game's shaders in the background with "
                     "async_shader_compilation (Direct3D 11 compiles them for the GPU then). -1: "
                     "3/4 of the logical processors; 0: none, shaders are created when drawn")
    .range(-1, 64);
REXCVAR_DEFINE_BOOL(d3d11_tessellation_wireframe, false, "GPU/D3D11",
                    "Render tessellation as wireframe");

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/adaptive_quad_hs.h"
#include "../shaders/bytecode/d3d11_5_0/adaptive_triangle_hs.h"
#include "../shaders/bytecode/d3d11_5_0/continuous_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/continuous_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/continuous_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/continuous_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/discrete_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/discrete_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/discrete_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/discrete_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d11_5_0/float24_round_ps.h"
#include "../shaders/bytecode/d3d11_5_0/float24_truncate_ps.h"
#include "../shaders/bytecode/d3d11_5_0/tessellation_adaptive_vs.h"
#include "../shaders/bytecode/d3d11_5_0/tessellation_indexed_vs.h"
}  // namespace shaders

size_t PipelineCache::PipelineDescriptionHasher::operator()(
    const PipelineDescription& description) const {
  return size_t(XXH3_64bits(&description, sizeof(description)));
}

bool PipelineCache::PipelineDescriptionEqual::operator()(const PipelineDescription& a,
                                                         const PipelineDescription& b) const {
  return !std::memcmp(&a, &b, sizeof(a));
}

static_assert(uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory) == 0 &&
                  uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory) == 0,
              "GetBackendShaderBytecodeOptions aliases u0 to t0");
static_assert(xenos::kMaxColorRenderTargets == 4,
              "GetBackendShaderBytecodeOptions places graphics UAVs after 4 render targets");

ShaderBytecodeOptions PipelineCache::GetShaderBytecodeOptions(
    const ui::d3d11::D3D11Provider& provider, bool alias_shared_memory_uav) {
  uint64_t supported_feature_flags = 0;
  if (provider.AreUAVsAtEveryStageSupported()) {
    supported_feature_flags |=
        dxbc::kShaderFeature0_UAVsAtEveryStage | dxbc::kShaderFeature0_64UAVs;
  }
  if (provider.IsPSSpecifiedStencilReferenceSupported()) {
    supported_feature_flags |= dxbc::kShaderFeature0_StencilRef;
  }
  if (provider.AreRasterizerOrderedViewsSupported()) {
    supported_feature_flags |= dxbc::kShaderFeature0_ROVs;
  }
  if (provider.AreTypedUAVLoadAdditionalFormatsSupported()) {
    supported_feature_flags |= dxbc::kShaderFeature0_TypedUAVLoadAdditionalFormats;
  }
  return GetBackendShaderBytecodeOptions(supported_feature_flags, provider.GetUAVSlotCount(),
                                         alias_shared_memory_uav);
}

PipelineCache::PipelineCache(D3D11CommandProcessor& command_processor,
                             const RegisterFile& register_file,
                             const D3D11RenderTargetCache& render_target_cache)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache) {}

PipelineCache::~PipelineCache() { Shutdown(); }

std::unique_ptr<DxbcShaderTranslator> PipelineCache::CreateTranslator() const {
  // Bindful (Direct3D 11 has no bindless resources), RTV path.
  return std::make_unique<DxbcShaderTranslator>(
      command_processor_.GetD3D11Provider().GetAdapterVendorID(), false, false,
      !render_target_cache_.gamma_render_target_as_unorm16(),
      render_target_cache_.msaa_2x_supported(), render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(), render_target_cache_.edram_tile_count());
}

bool PipelineCache::CreateTranslatorAndDepthOnlyPixelShader() {
  shader_translator_ = CreateTranslator();

  auto create_depth_only_pixel_shader =
      [this](Microsoft::WRL::ComPtr<ID3D11PixelShader>& shader_out, bool zpd_total,
             DxbcShaderTranslator::Modification::DepthStencilMode depth_stencil_mode,
             const char* name) {
        std::vector<uint8_t> code =
            shader_translator_->CreateDepthOnlyPixelShader(zpd_total, depth_stencil_mode);
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> native =
            CreateNativeShader(code.data(), code.size(), false, name);
        return native && SUCCEEDED(native.As(&shader_out));
      };
  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (!create_depth_only_pixel_shader(depth_only_pixel_shader_, false,
                                      DepthStencilMode::kNoModifiers,
                                      "the depth-only pixel shader")) {
    return false;
  }
  // As D3D12's for hybrid occlusion queries. Without them, the queries count
  // passed samples only.
  if (command_processor_.IsZpdHybridSupported()) {
    if (!create_depth_only_pixel_shader(zpd_total_depth_only_pixel_shader_, true,
                                        DepthStencilMode::kNoModifiers,
                                        "the ZPD counting depth-only pixel shader") ||
        (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
         (!create_depth_only_pixel_shader(zpd_total_float24_truncate_pixel_shader_, true,
                                          DepthStencilMode::kFloat24Truncating,
                                          "the ZPD counting float24 truncating pixel shader") ||
          !create_depth_only_pixel_shader(zpd_total_float24_round_pixel_shader_, true,
                                          DepthStencilMode::kFloat24Rounding,
                                          "the ZPD counting float24 rounding pixel shader")))) {
      zpd_total_depth_only_pixel_shader_.Reset();
      zpd_total_float24_truncate_pixel_shader_.Reset();
      zpd_total_float24_round_pixel_shader_.Reset();
    }
  }
  return true;
}

void PipelineCache::DestroyDrawResolutionScaleDependentObjects() {
  // As in D3D12: the scale is baked into the translated shaders (pixel
  // position and resolved texture coordinate scaling), which aren't keyed by
  // it, and the pipelines refer to the translations. The guest shaders
  // themselves (microcode, analysis, bindings) don't depend on the scale and
  // are kept; only their translations (with their native variants) go.
  DrainCreationQueue();
  pipelines_.clear();
  std::vector<uint64_t> modifications;
  for (const auto& shader_pair : shaders_) {
    D3D11Shader& shader = *shader_pair.second;
    modifications.clear();
    for (const auto& translation_pair : shader.translations()) {
      modifications.push_back(translation_pair.first);
    }
    for (uint64_t modification : modifications) {
      shader.DestroyTranslation(modification);
    }
  }
  depth_only_pixel_shader_.Reset();
  zpd_total_depth_only_pixel_shader_.Reset();
  zpd_total_float24_truncate_pixel_shader_.Reset();
  zpd_total_float24_round_pixel_shader_.Reset();
  shader_translator_.reset();
}

bool PipelineCache::CreateDrawResolutionScaleDependentObjects() {
  return CreateTranslatorAndDepthOnlyPixelShader();
}

bool PipelineCache::Initialize() {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();

  if (!CreateTranslatorAndDepthOnlyPixelShader()) {
    Shutdown();
    return false;
  }
  if (FAILED(device->CreatePixelShader(shaders::float24_truncate_ps,
                                       sizeof(shaders::float24_truncate_ps), nullptr,
                                       &float24_truncate_pixel_shader_)) ||
      FAILED(device->CreatePixelShader(shaders::float24_round_ps, sizeof(shaders::float24_round_ps),
                                       nullptr, &float24_round_pixel_shader_))) {
    REXGPU_ERROR("D3D11 pipeline cache: Failed to create the float24 depth pixel shaders");
    Shutdown();
    return false;
  }

  // Tessellation.
  if (FAILED(device->CreateVertexShader(shaders::tessellation_indexed_vs,
                                        sizeof(shaders::tessellation_indexed_vs), nullptr,
                                        &tessellation_indexed_vertex_shader_)) ||
      FAILED(device->CreateVertexShader(shaders::tessellation_adaptive_vs,
                                        sizeof(shaders::tessellation_adaptive_vs), nullptr,
                                        &tessellation_adaptive_vertex_shader_))) {
    REXGPU_ERROR("D3D11 pipeline cache: Failed to create the tessellation vertex shaders");
    Shutdown();
    return false;
  }
  struct HullShaderCode {
    xenos::TessellationMode mode;
    uint32_t type;
    const void* code;
    size_t size;
  };
#define REX_D3D11_HULL_SHADER(mode, type, name) \
  {xenos::TessellationMode::mode, type, shaders::name, sizeof(shaders::name)}
  const HullShaderCode hull_shader_code[] = {
      REX_D3D11_HULL_SHADER(kDiscrete, 0, discrete_triangle_3cp_hs),
      REX_D3D11_HULL_SHADER(kDiscrete, 1, discrete_triangle_1cp_hs),
      REX_D3D11_HULL_SHADER(kDiscrete, 2, discrete_quad_4cp_hs),
      REX_D3D11_HULL_SHADER(kDiscrete, 3, discrete_quad_1cp_hs),
      REX_D3D11_HULL_SHADER(kContinuous, 0, continuous_triangle_3cp_hs),
      REX_D3D11_HULL_SHADER(kContinuous, 1, continuous_triangle_1cp_hs),
      REX_D3D11_HULL_SHADER(kContinuous, 2, continuous_quad_4cp_hs),
      REX_D3D11_HULL_SHADER(kContinuous, 3, continuous_quad_1cp_hs),
      REX_D3D11_HULL_SHADER(kAdaptive, 1, adaptive_triangle_hs),
      REX_D3D11_HULL_SHADER(kAdaptive, 3, adaptive_quad_hs),
  };
#undef REX_D3D11_HULL_SHADER
  for (const HullShaderCode& hull_shader : hull_shader_code) {
    if (FAILED(device->CreateHullShader(
            hull_shader.code, hull_shader.size, nullptr,
            &hull_shaders_[uint32_t(hull_shader.mode)][hull_shader.type]))) {
      REXGPU_ERROR("D3D11 pipeline cache: Failed to create the tessellation hull shaders");
      Shutdown();
      return false;
    }
  }

  StartCreationThreads();
  return true;
}

void PipelineCache::StartCreationThreads() {
  int32_t thread_count_setting = REXCVAR_GET(d3d11_shader_creation_threads);
  if (!thread_count_setting) {
    return;
  }
  uint32_t logical_processor_count = rex::thread::logical_processor_count();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  uint32_t thread_count =
      thread_count_setting < 0
          ? std::max(logical_processor_count * 3 / 4, uint32_t(1))
          : std::min(uint32_t(thread_count_setting), logical_processor_count);
  creation_threads_shutdown_ = false;
  for (uint32_t i = 0; i < thread_count; ++i) {
    std::unique_ptr<rex::thread::Thread> thread =
        rex::thread::Thread::Create({}, [this]() { CreationThread(); });
    if (!thread) {
      REXGPU_WARN("D3D11 pipeline cache: Failed to start a shader creation thread");
      break;
    }
    thread->set_name("D3D11 Shaders");
    creation_threads_.push_back(std::move(thread));
  }
}

void PipelineCache::StopCreationThreads() {
  if (creation_threads_.empty()) {
    return;
  }
  DrainCreationQueue();
  {
    std::lock_guard<std::mutex> lock(creation_mutex_);
    creation_threads_shutdown_ = true;
  }
  creation_request_cond_.notify_all();
  for (std::unique_ptr<rex::thread::Thread>& thread : creation_threads_) {
    rex::thread::Wait(thread.get(), false);
  }
  creation_threads_.clear();
}

void PipelineCache::DrainCreationQueue() {
  std::unique_lock<std::mutex> lock(creation_mutex_);
  for (const CreationJob& job : creation_queue_) {
    job.native->state.store(D3D11Shader::D3D11Translation::Native::kNotRequested,
                            std::memory_order_release);
  }
  creation_queue_.clear();
  creation_done_cond_.wait(lock, [this]() { return !creation_threads_busy_; });
}

void PipelineCache::AwaitCreationQueue() {
  std::unique_lock<std::mutex> lock(creation_mutex_);
  creation_done_cond_.wait(
      lock, [this]() { return creation_queue_.empty() && !creation_threads_busy_; });
}

void PipelineCache::CreationThread() {
  while (true) {
    CreationJob job;
    {
      std::unique_lock<std::mutex> lock(creation_mutex_);
      creation_request_cond_.wait(
          lock, [this]() { return creation_threads_shutdown_ || !creation_queue_.empty(); });
      if (creation_threads_shutdown_) {
        return;
      }
      job = std::move(creation_queue_.front());
      creation_queue_.pop_front();
      ++creation_threads_busy_;
    }
    job.native->shader = CreateNativeShader(job.sm51->data(), job.sm51->size(),
                                            job.native->shared_memory_is_uav, job.name,
                                            job.native->texture_swizzles, false);
    {
      std::lock_guard<std::mutex> lock(creation_mutex_);
      job.native->state.store(D3D11Shader::D3D11Translation::Native::kDone,
                              std::memory_order_release);
      --creation_threads_busy_;
    }
    creation_done_cond_.notify_all();
  }
}

ID3D11HullShader* PipelineCache::GetHullShader(
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  uint32_t type;
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      type = 0;
      break;
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      type = 1;
      break;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      type = 2;
      break;
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      type = 3;
      break;
    default:
      return nullptr;
  }
  if (uint32_t(tessellation_mode) >= rex::countof(hull_shaders_)) {
    return nullptr;
  }
  return hull_shaders_[uint32_t(tessellation_mode)][type].Get();
}

void PipelineCache::Shutdown() {
  // The storage writer refers to the shaders until its queue is written.
  ShutdownShaderStorage();
  StopCreationThreads();
  ClearCache();
  for (auto& mode_hull_shaders : hull_shaders_) {
    for (Microsoft::WRL::ComPtr<ID3D11HullShader>& hull_shader : mode_hull_shaders) {
      hull_shader.Reset();
    }
  }
  tessellation_adaptive_vertex_shader_.Reset();
  tessellation_indexed_vertex_shader_.Reset();
  float24_round_pixel_shader_.Reset();
  float24_truncate_pixel_shader_.Reset();
  depth_only_pixel_shader_.Reset();
  zpd_total_depth_only_pixel_shader_.Reset();
  zpd_total_float24_truncate_pixel_shader_.Reset();
  zpd_total_float24_round_pixel_shader_.Reset();
  shader_translator_.reset();
}

void PipelineCache::ClearCache() {
  DrainCreationQueue();
  pipelines_.clear();
  geometry_shaders_.clear();
  shaders_.clear();
  failed_pipeline_count_ = 0;
}

D3D11Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                                       uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<D3D11Shader>(shader_type, hash, host_address, dword_count);
  D3D11Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

void PipelineCache::AnalyzeShaderUcode(Shader& shader) {
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }
}

const PipelineCache::Pipeline* PipelineCache::ConfigurePipeline(
    D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    bool apply_polygon_offset_in_shader, bool shared_memory_is_uav, bool zpd_total,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    const std::vector<TextureSwizzle>& vertex_texture_swizzles,
    const std::vector<TextureSwizzle>& pixel_texture_swizzles, bool& pending_out) {
  pending_out = false;
  PipelineDescription description;
  GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_processing_result,
                             normalized_depth_control, normalized_color_mask,
                             apply_polygon_offset_in_shader, shared_memory_is_uav, zpd_total,
                             bound_depth_and_color_render_target_bits,
                             bound_depth_and_color_render_target_formats, description);
  description.vertex_shader_native =
      FindOrAddNative(*vertex_shader, shared_memory_is_uav, vertex_texture_swizzles);
  if (description.pixel_shader) {
    description.pixel_shader_native =
        FindOrAddNative(*pixel_shader, shared_memory_is_uav, pixel_texture_swizzles);
  }
  auto it = pipelines_.find(description);
  if (it == pipelines_.end()) {
    // The guest shaders on the creation threads, as D3D12's pipelines: not
    // for depth-only pipelines (skipping them leaves holes in later passes)
    // unless async_depth_only_pipelines, and never with memexport (a skipped
    // draw would lose its data).
    bool async = REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty() &&
                 !shared_memory_is_uav &&
                 (description.pixel_shader || REXCVAR_GET(async_depth_only_pipelines));
    // Both requested before checking, so they're created at the same time.
    bool vertex_shader_done =
        RequestNativeShader(*vertex_shader, description.vertex_shader_native, async);
    bool pixel_shader_done =
        !description.pixel_shader ||
        RequestNativeShader(*pixel_shader, description.pixel_shader_native, async);
    if (!vertex_shader_done || !pixel_shader_done) {
      pending_out = true;
      return nullptr;
    }
    std::unique_ptr<Pipeline> pipeline = CreatePipeline(description);
    if (pipeline) {
      StorePipeline(description, vertex_texture_swizzles,
                    description.pixel_shader ? pixel_texture_swizzles
                                             : std::vector<TextureSwizzle>());
    } else {
      ++failed_pipeline_count_;
    }
    it = pipelines_.emplace(description, std::move(pipeline)).first;
  }
  return it->second.get();
}

bool PipelineCache::EnsureTranslated(D3D11Shader::D3D11Translation& translation) {
  if (!translation.translation_attempted_ &&
      TranslateWith(*shader_translator_, ucode_disasm_buffer_, translation)) {
    StoreShader(translation.shader());
  }
  return translation.is_valid();
}

bool PipelineCache::TranslateWith(DxbcShaderTranslator& translator,
                                  string::StringBuffer& ucode_disasm_buffer,
                                  D3D11Shader::D3D11Translation& translation) {
  translation.translation_attempted_ = true;
  Shader& shader = translation.shader();
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer);
  }
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("Shader {:016X} translation failed; marking as ignored",
                 shader.ucode_data_hash());
    return false;
  }
  return translation.is_valid();
}

void PipelineCache::StoreShader(const Shader& shader) {
  if (storage_writer_.is_active() &&
      const_cast<Shader&>(shader).try_set_ucode_storage_index(storage_writer_.storage_index())) {
    shader_storage_flush_needed_ = true;
    storage_writer_.QueueShaderWrite(&shader);
  }
}

void PipelineCache::StorePipeline(const PipelineDescription& description,
                                  const std::vector<TextureSwizzle>& vertex_texture_swizzles,
                                  const std::vector<TextureSwizzle>& pixel_texture_swizzles) {
  if (!storage_writer_.is_active() ||
      vertex_texture_swizzles.size() > kMaxStoredTextureSwizzles ||
      pixel_texture_swizzles.size() > kMaxStoredTextureSwizzles) {
    return;
  }
  PipelineStoredDescription stored;
  std::memset(&stored, 0, sizeof(stored));
  PipelineStoredData& data = stored.description;
  data.vertex_shader_hash = description.vertex_shader->shader().ucode_data_hash();
  data.vertex_shader_modification = description.vertex_shader->modification();
  if (description.pixel_shader) {
    data.pixel_shader_hash = description.pixel_shader->shader().ucode_data_hash();
    data.pixel_shader_modification = description.pixel_shader->modification();
  }
  data.vertex_texture_swizzle_count = uint32_t(vertex_texture_swizzles.size());
  std::copy(vertex_texture_swizzles.begin(), vertex_texture_swizzles.end(),
            data.vertex_texture_swizzles);
  if (description.pixel_shader) {
    data.pixel_texture_swizzle_count = uint32_t(pixel_texture_swizzles.size());
    std::copy(pixel_texture_swizzles.begin(), pixel_texture_swizzles.end(),
              data.pixel_texture_swizzles);
  }
  // The rest is copied bytewise, padding included (zero, as descriptions are
  // compared bytewise).
  std::memcpy(&data.state, &description, sizeof(description));
  data.state.vertex_shader = nullptr;
  data.state.pixel_shader = nullptr;
  data.state.vertex_shader_native = 0;
  data.state.pixel_shader_native = 0;
  stored.description_hash = XXH3_64bits(&stored.description, sizeof(stored.description));
  if (!stored_pipeline_hashes_.insert(stored.description_hash).second) {
    return;
  }
  pipeline_storage_flush_needed_ = true;
  storage_writer_.QueuePipelineWrite(stored);
}

void PipelineCache::EndFrame() {
  if (shader_storage_flush_needed_ || pipeline_storage_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_flush_needed_, pipeline_storage_flush_needed_);
    shader_storage_flush_needed_ = false;
    pipeline_storage_flush_needed_ = false;
  }
}

void PipelineCache::ShutdownShaderStorage() {
  storage_writer_.ShutdownShaderStorage();
  stored_pipeline_hashes_.clear();
  shader_storage_flush_needed_ = false;
  pipeline_storage_flush_needed_ = false;
}

void PipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
  std::vector<D3D11Shader*> shaders_to_translate;
  for (const auto& shader_pair : shaders_) {
    uint64_t hash = shader_pair.first;
    auto it = translations_needed.lower_bound(std::make_pair(hash, uint64_t(0)));
    if (it != translations_needed.end() && it->first == hash) {
      shaders_to_translate.push_back(shader_pair.second.get());
    }
  }
  if (shaders_to_translate.empty()) {
    return;
  }
  // Translations of one shader on one thread: the first one sets up the
  // shader's bindings.
  std::atomic<size_t> next_shader{0};
  auto translate = [&]() {
    std::unique_ptr<DxbcShaderTranslator> translator = CreateTranslator();
    string::StringBuffer ucode_disasm_buffer;
    while (true) {
      size_t index = next_shader.fetch_add(1);
      if (index >= shaders_to_translate.size()) {
        break;
      }
      D3D11Shader& shader = *shaders_to_translate[index];
      uint64_t hash = shader.ucode_data_hash();
      for (auto it = translations_needed.lower_bound(std::make_pair(hash, uint64_t(0)));
           it != translations_needed.end() && it->first == hash; ++it) {
        auto& translation =
            *static_cast<D3D11Shader::D3D11Translation*>(shader.GetOrCreateTranslation(it->second));
        // One that failed in this session already isn't tried again.
        if (!translation.translation_attempted_) {
          TranslateWith(*translator, ucode_disasm_buffer, translation);
        }
      }
    }
  };
  uint32_t logical_processor_count = std::max(rex::thread::logical_processor_count(), uint32_t(1));
  size_t thread_count =
      std::min(size_t(logical_processor_count), shaders_to_translate.size()) - 1;
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  for (size_t i = 0; i < thread_count; ++i) {
    std::unique_ptr<rex::thread::Thread> thread = rex::thread::Thread::Create({}, translate);
    if (!thread) {
      break;
    }
    thread->set_name("D3D11 Shader Storage");
    threads.push_back(std::move(thread));
  }
  // This thread too.
  translate();
  for (std::unique_ptr<rex::thread::Thread>& thread : threads) {
    rex::thread::Wait(thread.get(), false);
  }
}

void PipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                            uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig pipeline_config;
  pipeline_config.file_suffix = ".rtv.d3d11.xpso";
  // 'DBRT'.
  pipeline_config.api_magic = 0x54524244;
  pipeline_config.version =
      std::max(PipelineDescription::kVersion, DxbcShaderTranslator::Modification::kVersion);
  uint32_t storage_index = storage_writer_.storage_index() + 1;
  std::vector<PipelineStoredDescription> stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords, uint32_t ucode_dword_count,
              uint64_t /*ucode_data_hash*/) {
            D3D11Shader* shader = LoadShader(type, ucode_dwords, ucode_dword_count);
            // Already in the file.
            shader->set_ucode_storage_index(storage_index);
            return true;
          },
          [this](const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
            TranslateShadersForStorage(translations_needed);
          },
          stored_descriptions)) {
    return;
  }

  // The native shaders of the stored pipelines, on the creation threads if
  // there are any.
  bool async = !creation_threads_.empty();
  std::vector<PipelineDescription> descriptions;
  descriptions.reserve(stored_descriptions.size());
  size_t shaders_missing = 0;
  auto find_translation = [this](uint64_t hash,
                                 uint64_t modification) -> D3D11Shader::D3D11Translation* {
    auto it = shaders_.find(hash);
    if (it == shaders_.end()) {
      return nullptr;
    }
    auto* translation =
        static_cast<D3D11Shader::D3D11Translation*>(it->second->GetTranslation(modification));
    return translation && translation->is_valid() ? translation : nullptr;
  };
  std::vector<TextureSwizzle> vertex_texture_swizzles, pixel_texture_swizzles;
  size_t duplicates = 0;
  for (const PipelineStoredDescription& stored : stored_descriptions) {
    // Files written before duplicates were checked may have them.
    if (!stored_pipeline_hashes_.insert(stored.description_hash).second) {
      ++duplicates;
      continue;
    }
    const PipelineStoredData& data = stored.description;
    D3D11Shader::D3D11Translation* vertex_shader =
        find_translation(data.vertex_shader_hash, data.vertex_shader_modification);
    D3D11Shader::D3D11Translation* pixel_shader =
        data.pixel_shader_hash
            ? find_translation(data.pixel_shader_hash, data.pixel_shader_modification)
            : nullptr;
    if (!vertex_shader || (data.pixel_shader_hash && !pixel_shader) ||
        data.vertex_texture_swizzle_count > kMaxStoredTextureSwizzles ||
        data.pixel_texture_swizzle_count > kMaxStoredTextureSwizzles) {
      ++shaders_missing;
      continue;
    }
    PipelineDescription& description = descriptions.emplace_back();
    std::memcpy(&description, &data.state, sizeof(description));
    description.vertex_shader = vertex_shader;
    description.pixel_shader = pixel_shader;
    vertex_texture_swizzles.assign(data.vertex_texture_swizzles,
                                   data.vertex_texture_swizzles + data.vertex_texture_swizzle_count);
    pixel_texture_swizzles.assign(data.pixel_texture_swizzles,
                                  data.pixel_texture_swizzles + data.pixel_texture_swizzle_count);
    bool shared_memory_is_uav = description.shared_memory_is_uav != 0;
    description.vertex_shader_native =
        FindOrAddNative(*vertex_shader, shared_memory_is_uav, vertex_texture_swizzles);
    RequestNativeShader(*vertex_shader, description.vertex_shader_native, async);
    if (pixel_shader) {
      description.pixel_shader_native =
          FindOrAddNative(*pixel_shader, shared_memory_is_uav, pixel_texture_swizzles);
      RequestNativeShader(*pixel_shader, description.pixel_shader_native, async);
    }
  }
  REXGPU_INFO(
      "D3D11 pipeline cache: {} stored pipelines ({} with missing or failed shaders), their "
      "shaders being created{}",
      stored_descriptions.size() - duplicates, shaders_missing,
      blocking ? "" : " in the background, the pipelines at their first draw");
  if (!blocking) {
    return;
  }
  AwaitCreationQueue();
  size_t pipelines_created = 0;
  for (const PipelineDescription& description : descriptions) {
    if (pipelines_.find(description) != pipelines_.end()) {
      continue;
    }
    std::unique_ptr<Pipeline> pipeline = CreatePipeline(description);
    if (pipeline) {
      ++pipelines_created;
    } else {
      ++failed_pipeline_count_;
    }
    pipelines_.emplace(description, std::move(pipeline));
  }
  REXGPU_INFO("D3D11 pipeline cache: {} pipelines created from the storage", pipelines_created);
}

uint32_t PipelineCache::FindOrAddNative(D3D11Shader::D3D11Translation& translation,
                                        bool shared_memory_is_uav,
                                        const std::vector<TextureSwizzle>& texture_swizzles) {
  std::deque<D3D11Shader::D3D11Translation::Native>& natives = translation.natives_;
  for (size_t i = 0; i < natives.size(); ++i) {
    if (natives[i].shared_memory_is_uav == shared_memory_is_uav &&
        natives[i].texture_swizzles == texture_swizzles) {
      return uint32_t(i);
    }
  }
  D3D11Shader::D3D11Translation::Native& native = natives.emplace_back();
  native.shared_memory_is_uav = shared_memory_is_uav;
  native.texture_swizzles = texture_swizzles;
  return uint32_t(natives.size() - 1);
}

std::string PipelineCache::GetNativeShaderName(const D3D11Shader::D3D11Translation& translation) {
  return fmt::format("{} shader {:016X} (modification {:016X})",
                     translation.shader().type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
                     translation.shader().ucode_data_hash(), translation.modification());
}

bool PipelineCache::RequestNativeShader(D3D11Shader::D3D11Translation& translation,
                                        uint32_t native_index, bool async) {
  using Native = D3D11Shader::D3D11Translation::Native;
  if (!EnsureTranslated(translation)) {
    // Done, with no shader.
    return true;
  }
  Native& native = translation.natives_[native_index];
  uint32_t state = native.state.load(std::memory_order_acquire);
  if (state == Native::kDone) {
    return true;
  }
  if (state == Native::kNotRequested) {
    if (async) {
      std::string name = GetNativeShaderName(translation);
      {
        std::lock_guard<std::mutex> lock(creation_mutex_);
        native.state.store(Native::kPending, std::memory_order_relaxed);
        creation_queue_.push_back({&native, &translation.translated_binary(), std::move(name)});
      }
      creation_request_cond_.notify_one();
      return false;
    }
    const std::vector<uint8_t>& binary = translation.translated_binary();
    native.shader = CreateNativeShader(binary.data(), binary.size(), native.shared_memory_is_uav,
                                       GetNativeShaderName(translation), native.texture_swizzles);
    native.state.store(Native::kDone, std::memory_order_release);
    return true;
  }
  // Pending on a creation thread.
  if (async) {
    return false;
  }
  std::unique_lock<std::mutex> lock(creation_mutex_);
  creation_done_cond_.wait(lock, [&native]() {
    return native.state.load(std::memory_order_acquire) != Native::kPending;
  });
  if (native.state.load(std::memory_order_acquire) == Native::kDone) {
    return true;
  }
  // Dropped from the queue meanwhile (not expected while drawing).
  lock.unlock();
  return RequestNativeShader(translation, native_index, false);
}

Microsoft::WRL::ComPtr<ID3D11DeviceChild> PipelineCache::CreateNativeShader(
    const void* sm51, size_t size, bool shared_memory_is_uav, const std::string& name,
    const std::vector<TextureSwizzle>& texture_swizzles, bool log_debug_messages) {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();

  ShaderBytecodeOptions options = GetShaderBytecodeOptions(provider, !shared_memory_is_uav);
  options.texture_swizzles = texture_swizzles;
  ShaderBytecode bytecode;
  std::string error;
  if (!ConvertShaderBytecode(std::span<const uint8_t>(static_cast<const uint8_t*>(sm51), size),
                             options, bytecode, error)) {
    REXGPU_ERROR("Direct3D 11: Failed to convert {} to Shader Model 5.0: {}", name, error);
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
  HRESULT result = E_INVALIDARG;
  switch (dxbc::ProgramType(bytecode.program_type)) {
    case dxbc::ProgramType::kVertexShader: {
      Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
      result = device->CreateVertexShader(bytecode.data.data(), bytecode.data.size(), nullptr,
                                          &vertex_shader);
      shader = vertex_shader;
    } break;
    case dxbc::ProgramType::kDomainShader: {
      Microsoft::WRL::ComPtr<ID3D11DomainShader> domain_shader;
      result = device->CreateDomainShader(bytecode.data.data(), bytecode.data.size(), nullptr,
                                          &domain_shader);
      shader = domain_shader;
    } break;
    case dxbc::ProgramType::kGeometryShader: {
      Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometry_shader;
      result = device->CreateGeometryShader(bytecode.data.data(), bytecode.data.size(), nullptr,
                                            &geometry_shader);
      shader = geometry_shader;
    } break;
    case dxbc::ProgramType::kPixelShader: {
      Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
      result = device->CreatePixelShader(bytecode.data.data(), bytecode.data.size(), nullptr,
                                         &pixel_shader);
      shader = pixel_shader;
    } break;
    default:
      break;
  }
  if (FAILED(result)) {
    REXGPU_ERROR("Direct3D 11: Failed to create {}: 0x{:08X}", name, uint32_t(result));
    if (log_debug_messages) {
      provider.LogD3D11DebugMessages();
    }
    return nullptr;
  }
  return shader;
}

ID3D11GeometryShader* PipelineCache::GetOrCreateGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key.key);
  if (it != geometry_shaders_.end()) {
    return it->second.Get();
  }
  std::vector<uint32_t> code;
  CreateDxbcGeometryShader(key, code);
  Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader =
      CreateNativeShader(code.data(), code.size() * sizeof(uint32_t), false,
                         fmt::format("geometry shader {:08X}", key.key));
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometry_shader;
  if (shader) {
    shader.As(&geometry_shader);
  }
  return geometry_shaders_.emplace(key.key, std::move(geometry_shader)).first->second.Get();
}

// Follows D3D12's PipelineCache::GetCurrentStateDescription for the RTV path.
void PipelineCache::GetCurrentStateDescription(
    D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    bool apply_polygon_offset_in_shader, bool shared_memory_is_uav, bool zpd_total,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    PipelineDescription& description_out) {
  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  std::memset(&description_out, 0, sizeof(description_out));

  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);

  description_out.vertex_shader = vertex_shader;
  description_out.shared_memory_is_uav = uint32_t(shared_memory_is_uav);
  description_out.zpd_total = uint32_t(zpd_total);
  bool tessellated = primitive_processing_result.IsTessellated();
  if (tessellated) {
    description_out.tessellated = 1;
    description_out.tessellation_mode = primitive_processing_result.tessellation_mode;
  }

  // No geometry shader with tessellation, as on Direct3D 12.
  GeometryShaderType geometry_shader_type = GeometryShaderType::kNone;
  switch (tessellated ? xenos::PrimitiveType::kTrianglePatch
                      : primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      geometry_shader_type = GeometryShaderType::kPointList;
      break;
    case xenos::PrimitiveType::kRectangleList:
      geometry_shader_type = GeometryShaderType::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      geometry_shader_type = GeometryShaderType::kQuadList;
      break;
    default:
      geometry_shader_type = GeometryShaderType::kNone;
      break;
  }
  GeometryShaderKey geometry_shader_key;
  if (GetGeometryShaderKey(
          geometry_shader_type, DxbcShaderTranslator::Modification(vertex_shader->modification()),
          DxbcShaderTranslator::Modification(pixel_shader ? pixel_shader->modification() : 0),
          geometry_shader_key)) {
    description_out.geometry_shader_key = geometry_shader_key.key;
  }

  if (!rasterization_enabled) {
    description_out.cull_mode = CullMode::kDisableRasterization;
    return;
  }

  description_out.pixel_shader = pixel_shader;

  // Rasterizer state (see the D3D12 code for the per-side choices).
  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      description_out.cull_mode = CullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = CullMode::kBack;
    } else {
      description_out.cull_mode = CullMode::kNone;
    }
    if (!cull_front && pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles) {
      description_out.fill_mode_wireframe = 1;
    }
    if (!cull_back && pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles) {
      description_out.fill_mode_wireframe = 1;
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    cull_front = false;
    cull_back = false;
  }
  if (!apply_polygon_offset_in_shader) {
    float polygon_offset, polygon_offset_scale;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                             polygon_offset);
    description_out.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    // In host pixels, with the scale of this draw (1x1 with
    // draw_resolution_scale_threshold), as D3D12 at pipeline creation.
    description_out.depth_bias_slope_scaled =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
        float(std::max(render_target_cache_.GetDrawScaleX(), render_target_cache_.GetDrawScaleY()));
  }
  description_out.depth_clip =
      !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable && !REXCVAR_GET(force_depth_clamp);
  // As d3d12_tessellation_wireframe.
  if (tessellated && REXCVAR_GET(d3d11_tessellation_wireframe)) {
    description_out.fill_mode_wireframe = 1;
  }

  // Depth / stencil.
  bool depth_stencil_bound_and_used = false;
  if (bound_depth_and_color_render_target_bits & 1) {
    if (normalized_depth_control.z_enable) {
      description_out.depth_func = normalized_depth_control.zfunc;
      description_out.depth_write = normalized_depth_control.z_write_enable;
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }
    if (normalized_depth_control.stencil_enable) {
      description_out.stencil_enable = 1;
      bool stencil_backface_enable = primitive_polygonal && normalized_depth_control.backface_enable;
      // Per-face masks are not supported by Direct3D 11, choose the back face
      // ones only if drawing only back faces.
      Register stencil_ref_mask_reg = (stencil_backface_enable && cull_front)
                                          ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                          : XE_GPU_REG_RB_STENCILREFMASK;
      auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
      description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
      description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
      description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
      description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
      description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
      description_out.stencil_front_func = normalized_depth_control.stencilfunc;
      if (stencil_backface_enable) {
        description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
        description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
        description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
        description_out.stencil_back_func = normalized_depth_control.stencilfunc_bf;
      } else {
        description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
        description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
        description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
        description_out.stencil_back_func = description_out.stencil_front_func;
      }
    }
    if (description_out.depth_func != xenos::CompareFunction::kAlways ||
        description_out.depth_write || description_out.stencil_enable) {
      description_out.depth_format =
          xenos::DepthRenderTargetFormat(bound_depth_and_color_render_target_formats[0]);
      depth_stencil_bound_and_used = true;
    }
  } else {
    description_out.depth_func = xenos::CompareFunction::kAlways;
  }

  // Render targets and blending.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i)))) {
      continue;
    }
    PipelineDescription::RenderTarget& rt = description_out.render_targets[i];
    rt.used = 1;
    rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    if (rt.write_mask) {
      auto blendcontrol =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      rt.src_blend = blendcontrol.color_srcblend;
      rt.dest_blend = blendcontrol.color_destblend;
      rt.blend_op = blendcontrol.color_comb_fcn;
      rt.src_blend_alpha = blendcontrol.alpha_srcblend;
      rt.dest_blend_alpha = blendcontrol.alpha_destblend;
      rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
    } else {
      rt.src_blend = xenos::BlendFactor::kOne;
      rt.dest_blend = xenos::BlendFactor::kZero;
      rt.blend_op = xenos::BlendOp::kAdd;
      rt.src_blend_alpha = xenos::BlendFactor::kOne;
      rt.dest_blend_alpha = xenos::BlendFactor::kZero;
      rt.blend_op_alpha = xenos::BlendOp::kAdd;
    }
  }

  xenos::MsaaSamples host_msaa_samples = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  if (!(bound_depth_and_color_render_target_bits & ~uint32_t(1)) &&
      !depth_stencil_bound_and_used) {
    host_msaa_samples = xenos::MsaaSamples::k1X;
  }
  description_out.host_msaa_samples = host_msaa_samples;
}

namespace {

// Like D3D12's kBlendFactorMap and kBlendFactorAlphaMap. Direct3D 11 has no
// separate alpha blend factor, the command processor replicates the constant
// alpha when only it is used.
D3D11_BLEND GetD3D11Blend(xenos::BlendFactor factor, bool alpha) {
  switch (factor) {
    case xenos::BlendFactor::kOne:
      return D3D11_BLEND_ONE;
    case xenos::BlendFactor::kSrcColor:
      return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha:
      return D3D11_BLEND_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return D3D11_BLEND_INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha:
      return D3D11_BLEND_DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return D3D11_BLEND_INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return D3D11_BLEND_BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return D3D11_BLEND_INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return D3D11_BLEND_SRC_ALPHA_SAT;
    default:
      return D3D11_BLEND_ZERO;
  }
}

D3D11_BLEND_OP GetD3D11BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return D3D11_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin:
      return D3D11_BLEND_OP_MIN;
    case xenos::BlendOp::kMax:
      return D3D11_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract:
      return D3D11_BLEND_OP_REV_SUBTRACT;
    default:
      return D3D11_BLEND_OP_ADD;
  }
}

}  // namespace

// Follows D3D12's PipelineCache::CreateD3D12Pipeline for the RTV path.
std::unique_ptr<PipelineCache::Pipeline> PipelineCache::CreatePipeline(
    const PipelineDescription& description) {
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  auto pipeline = std::make_unique<Pipeline>();

  // The native shaders are done (RequestNativeShader).
  auto* vertex_translation = const_cast<D3D11Shader::D3D11Translation*>(description.vertex_shader);
  ID3D11DeviceChild* vertex_shader =
      vertex_translation->is_valid()
          ? vertex_translation->natives_[description.vertex_shader_native].shader.Get()
          : nullptr;
  if (!vertex_shader) {
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      DxbcShaderTranslator::Modification(vertex_translation->modification())
          .vertex.host_vertex_shader_type;
  if (description.tessellated) {
    // The guest vertex shader was translated as the domain shader.
    if (!Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      return nullptr;
    }
    pipeline->vertex_shader = description.tessellation_mode == xenos::TessellationMode::kAdaptive
                                  ? tessellation_adaptive_vertex_shader_.Get()
                                  : tessellation_indexed_vertex_shader_.Get();
    pipeline->hull_shader = GetHullShader(description.tessellation_mode, host_vertex_shader_type);
    if (!pipeline->hull_shader) {
      REXGPU_ERROR(
          "Direct3D 11: No hull shader for tessellation mode {} and host vertex shader type {}",
          uint32_t(description.tessellation_mode), uint32_t(host_vertex_shader_type));
      return nullptr;
    }
    pipeline->domain_shader = static_cast<ID3D11DomainShader*>(vertex_shader);
  } else {
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      return nullptr;
    }
    pipeline->vertex_shader = static_cast<ID3D11VertexShader*>(vertex_shader);
  }

  if (description.geometry_shader_key) {
    GeometryShaderKey geometry_shader_key;
    geometry_shader_key.key = description.geometry_shader_key;
    pipeline->geometry_shader = GetOrCreateGeometryShader(geometry_shader_key);
    if (!pipeline->geometry_shader) {
      return nullptr;
    }
  }

  bool depth_used =
      description.depth_func != xenos::CompareFunction::kAlways || description.depth_write;
  if (description.pixel_shader) {
    auto* pixel_translation = const_cast<D3D11Shader::D3D11Translation*>(description.pixel_shader);
    ID3D11DeviceChild* pixel_shader =
        pixel_translation->is_valid()
            ? pixel_translation->natives_[description.pixel_shader_native].shader.Get()
            : nullptr;
    if (!pixel_shader) {
      return nullptr;
    }
    pipeline->pixel_shader = static_cast<ID3D11PixelShader*>(pixel_shader);
  } else if (description.cull_mode != CullMode::kDisableRasterization) {
    bool float24_convert = render_target_cache_.depth_float24_convert_in_pixel_shader() &&
                           depth_used &&
                           description.depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
    if (description.zpd_total && zpd_total_depth_only_pixel_shader_) {
      // Coverage still has to be counted without a guest pixel shader.
      if (float24_convert) {
        pipeline->pixel_shader = render_target_cache_.depth_float24_round()
                                     ? zpd_total_float24_round_pixel_shader_.Get()
                                     : zpd_total_float24_truncate_pixel_shader_.Get();
      } else {
        pipeline->pixel_shader = zpd_total_depth_only_pixel_shader_.Get();
      }
    } else if (float24_convert) {
      pipeline->pixel_shader = render_target_cache_.depth_float24_round()
                                   ? float24_round_pixel_shader_.Get()
                                   : float24_truncate_pixel_shader_.Get();
    } else if (!description.depth_write && !description.stencil_write_mask) {
      // An empty pixel shader forces rasterization, for occlusion queries.
      pipeline->pixel_shader = depth_only_pixel_shader_.Get();
    }
  }

  // Rasterizer state.
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode =
      description.fill_mode_wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  switch (description.cull_mode) {
    case CullMode::kFront:
      rasterizer_desc.CullMode = D3D11_CULL_FRONT;
      break;
    case CullMode::kBack:
      rasterizer_desc.CullMode = D3D11_CULL_BACK;
      break;
    default:
      rasterizer_desc.CullMode = D3D11_CULL_NONE;
      break;
  }
  rasterizer_desc.FrontCounterClockwise = description.front_counter_clockwise ? TRUE : FALSE;
  rasterizer_desc.DepthBias = description.depth_bias;
  rasterizer_desc.SlopeScaledDepthBias = description.depth_bias_slope_scaled;
  rasterizer_desc.DepthClipEnable = description.depth_clip ? TRUE : FALSE;
  rasterizer_desc.ScissorEnable = TRUE;
  uint32_t msaa_sample_count = uint32_t(1) << uint32_t(description.host_msaa_samples);
  // Quadrilateral lines with MSAA, as on Direct3D 12.
  rasterizer_desc.MultisampleEnable = msaa_sample_count > 1 ? TRUE : FALSE;
  if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &pipeline->rasterizer_state))) {
    REXGPU_ERROR("Direct3D 11: Failed to create a rasterizer state");
    return nullptr;
  }
  if (msaa_sample_count == 2 && !render_target_cache_.msaa_2x_supported()) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead.
    pipeline->sample_mask = 0b1001;
  }
  pipeline->host_msaa_samples = description.host_msaa_samples;

  // Depth / stencil.
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  depth_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  depth_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil_desc.BackFace = depth_stencil_desc.FrontFace;
  if (description.cull_mode != CullMode::kDisableRasterization) {
    if (depth_used) {
      depth_stencil_desc.DepthEnable = TRUE;
      depth_stencil_desc.DepthWriteMask =
          description.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions and stencil operations are the same in Direct3D
      // but plus one.
      depth_stencil_desc.DepthFunc = D3D11_COMPARISON_FUNC(uint32_t(D3D11_COMPARISON_NEVER) +
                                                           uint32_t(description.depth_func));
    }
    if (description.stencil_enable) {
      depth_stencil_desc.StencilEnable = TRUE;
      depth_stencil_desc.StencilReadMask = UINT8(description.stencil_read_mask);
      depth_stencil_desc.StencilWriteMask = UINT8(description.stencil_write_mask);
      auto stencil_op = [](xenos::StencilOp op) {
        return D3D11_STENCIL_OP(uint32_t(D3D11_STENCIL_OP_KEEP) + uint32_t(op));
      };
      auto compare = [](xenos::CompareFunction func) {
        return D3D11_COMPARISON_FUNC(uint32_t(D3D11_COMPARISON_NEVER) + uint32_t(func));
      };
      depth_stencil_desc.FrontFace.StencilFailOp = stencil_op(description.stencil_front_fail_op);
      depth_stencil_desc.FrontFace.StencilDepthFailOp =
          stencil_op(description.stencil_front_depth_fail_op);
      depth_stencil_desc.FrontFace.StencilPassOp = stencil_op(description.stencil_front_pass_op);
      depth_stencil_desc.FrontFace.StencilFunc = compare(description.stencil_front_func);
      depth_stencil_desc.BackFace.StencilFailOp = stencil_op(description.stencil_back_fail_op);
      depth_stencil_desc.BackFace.StencilDepthFailOp =
          stencil_op(description.stencil_back_depth_fail_op);
      depth_stencil_desc.BackFace.StencilPassOp = stencil_op(description.stencil_back_pass_op);
      depth_stencil_desc.BackFace.StencilFunc = compare(description.stencil_back_func);
    }
  }
  if (FAILED(device->CreateDepthStencilState(&depth_stencil_desc,
                                             &pipeline->depth_stencil_state))) {
    REXGPU_ERROR("Direct3D 11: Failed to create a depth/stencil state");
    return nullptr;
  }

  // Blending.
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.IndependentBlendEnable = TRUE;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const PipelineDescription::RenderTarget& rt = description.render_targets[i];
    D3D11_RENDER_TARGET_BLEND_DESC& blend = blend_desc.RenderTarget[i];
    blend.SrcBlend = D3D11_BLEND_ONE;
    blend.DestBlend = D3D11_BLEND_ZERO;
    blend.BlendOp = D3D11_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (!rt.used || description.cull_mode == CullMode::kDisableRasterization) {
      continue;
    }
    if (rt.src_blend != xenos::BlendFactor::kOne || rt.dest_blend != xenos::BlendFactor::kZero ||
        rt.blend_op != xenos::BlendOp::kAdd || rt.src_blend_alpha != xenos::BlendFactor::kOne ||
        rt.dest_blend_alpha != xenos::BlendFactor::kZero ||
        rt.blend_op_alpha != xenos::BlendOp::kAdd) {
      blend.BlendEnable = TRUE;
      blend.SrcBlend = GetD3D11Blend(rt.src_blend, false);
      blend.DestBlend = GetD3D11Blend(rt.dest_blend, false);
      blend.BlendOp = GetD3D11BlendOp(rt.blend_op);
      blend.SrcBlendAlpha = GetD3D11Blend(rt.src_blend_alpha, true);
      blend.DestBlendAlpha = GetD3D11Blend(rt.dest_blend_alpha, true);
      blend.BlendOpAlpha = GetD3D11BlendOp(rt.blend_op_alpha);
    }
    blend.RenderTargetWriteMask = UINT8(rt.write_mask);
  }
  if (FAILED(device->CreateBlendState(&blend_desc, &pipeline->blend_state))) {
    REXGPU_ERROR("Direct3D 11: Failed to create a blend state");
    return nullptr;
  }

  // Disable rasterization the way it's disabled in Direct3D by design.
  if (description.cull_mode == CullMode::kDisableRasterization) {
    pipeline->pixel_shader = nullptr;
  }

  return pipeline;
}

}  // namespace rex::graphics::d3d11
