/**
 * @file        graphics/d3d11/command_processor.cpp
 * @brief       Command processor of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/command_processor.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/float_constants.h>
#include <rex/graphics/util/pipeline_wait.h>
#include <rex/graphics/util/smaa_textures.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/perf/counter.h>
#include <rex/thread/mutex.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

REXCVAR_DEFINE_BOOL(d3d11_debug_white_textures, false, "GPU/D3D11",
                    "Bind white 1x1 textures in place of all guest textures, to tell texture "
                    "problems from drawing problems of the Direct3D 11 backend")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d11_gpu_time, true, "GPU/D3D11",
                    "Time the graphics card's work a frame with timestamp queries, in total and "
                    "by kind (draws, render target transfers, resolves, texture loads, uploads, "
                    "swap), for the debug overlay and the perf CSV. Builds with perf counters "
                    "only. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(d3d11_log_frames, 0, "GPU/D3D11",
                     "Log what the Direct3D 11 backend did in each of the first this many "
                     "frames (draws, resolves, textures, the render target shown), for "
                     "diagnosing a wrong or black picture")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../../ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_ps.h"
#include "../../ui/shaders/bytecode/d3d11_5_0/guest_output_triangle_strip_rect_vs.h"
#include "../shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"
#include "../shaders/bytecode/d3d11_5_0/fxaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_downscale_cs.h"
#include "../shaders/bytecode/d3d11_5_0/smaa_blending_weight_calculation_cs.h"
#include "../shaders/bytecode/d3d11_5_0/smaa_edge_detection_cs.h"
#include "../shaders/bytecode/d3d11_5_0/smaa_neighborhood_blending_cs.h"
}  // namespace shaders
// Converted at run time with the front buffer's channel mapping.
namespace shaders_sm51 {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_ps.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_ps.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_ps.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_ps.h"
}  // namespace shaders_sm51

namespace {
// Float constants of one stage: 256 float4 at most.
constexpr uint32_t kFloatConstantBufferSize = 256 * 4 * sizeof(float);
constexpr uint32_t kBoolLoopConstantBufferSize = (8 + 32) * sizeof(uint32_t);
constexpr uint32_t kFetchConstantBufferSize = 32 * 6 * sizeof(uint32_t);
}  // namespace

D3D11CommandProcessor::D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

D3D11CommandProcessor::~D3D11CommandProcessor() = default;

const ui::d3d11::D3D11Provider& D3D11CommandProcessor::GetD3D11Provider() const {
  return *static_cast<const ui::d3d11::D3D11Provider*>(graphics_system_->provider());
}

bool D3D11CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  provider.context_mutex().SetOwnerChangeCallback([this]() { OnContextOwnerChange(); });

  // The scaled resolve memory is made of ordinary buffers (no tiled resources
  // needed), so only the emulator's limit applies.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  if (!TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                  draw_resolution_scale_y)) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the emulator, reducing to "
        "{}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D11SharedMemory>(provider, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }
  shared_memory_->SetCommandProcessor(this);
  primitive_processor_ = std::make_unique<D3D11PrimitiveProcessor>(*register_file_, *memory_,
                                                                   *shared_memory_, provider);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }
  if (!CreateDrawResolutionScaleDependentCaches(draw_resolution_scale_x,
                                                draw_resolution_scale_y)) {
    if (draw_resolution_scale_x <= 1 && draw_resolution_scale_y <= 1) {
      REXGPU_ERROR("Failed to initialize the render target cache or the texture cache");
      return false;
    }
    REXGPU_WARN("Failed to set up the {}x{} draw resolution scale, drawing at 1x",
                draw_resolution_scale_x, draw_resolution_scale_y);
    draw_resolution_scale_x = 1;
    draw_resolution_scale_y = 1;
    if (!CreateDrawResolutionScaleDependentCaches(1, 1)) {
      REXGPU_ERROR("Failed to initialize the render target cache or the texture cache");
      return false;
    }
  }
  // Occlusion query sample counts are normalized by the scale.
  zpd_draw_resolution_scale_x_ = draw_resolution_scale_x;
  zpd_draw_resolution_scale_y_ = draw_resolution_scale_y;
  // Read by the pipeline cache for its depth-only pixel shaders. The counter
  // buffer is created with the queries.
  zpd_hybrid_supported_ = REXCVAR_GET(occlusion_query_full_counters);
  pipeline_cache_ = std::make_unique<PipelineCache>(*this, *register_file_, *render_target_cache_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  // Constant buffers, rewritten with WRITE_DISCARD when their data changes.
  D3D11_BUFFER_DESC constant_buffer_desc = {};
  constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  const std::pair<Microsoft::WRL::ComPtr<ID3D11Buffer>*, uint32_t> constant_buffers[] = {
      {&constant_buffer_system_, uint32_t(rex::align(sizeof(system_constants_), size_t(16)))},
      {&constant_buffer_float_vertex_, kFloatConstantBufferSize},
      {&constant_buffer_float_pixel_, kFloatConstantBufferSize},
      {&constant_buffer_bool_loop_, kBoolLoopConstantBufferSize},
      {&constant_buffer_fetch_, kFetchConstantBufferSize},
  };
  for (const auto& constant_buffer : constant_buffers) {
    constant_buffer_desc.ByteWidth = constant_buffer.second;
    if (FAILED(device->CreateBuffer(&constant_buffer_desc, nullptr,
                                    constant_buffer.first->GetAddressOf()))) {
      REXGPU_ERROR("Failed to create a {} byte constant buffer", constant_buffer.second);
      return false;
    }
  }
  std::memset(&system_constants_, 0, sizeof(system_constants_));
  cbuffer_binding_system_.up_to_date = false;
  cbuffer_binding_float_vertex_.up_to_date = false;
  cbuffer_binding_float_pixel_.up_to_date = false;
  cbuffer_binding_bool_loop_.up_to_date = false;
  cbuffer_binding_fetch_.up_to_date = false;

  if (!InitializeTemporaryGuestOutput() || !InitializeSwap()) {
    return false;
  }
  if (REXCVAR_GET(d3d11_debug_white_textures) && !InitializeDebugWhiteTextures()) {
    return false;
  }

  draw_state_valid_ = false;
  device_removed_ = false;
  command_processor_thread_id_ = std::this_thread::get_id();
  gpu_section_ = GpuSection::kDraws;
  gpu_section_other_thread_ = false;
  InitializeGpuTime();
  BeginGpuTimeFrame();
  REXGPU_WARN(
      "Direct3D 11 backend: experimental{}",
      REXCVAR_GET(d3d11_debug_white_textures) ? "; textures are replaced with white" : "");
  return true;
}

void D3D11CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking,
                                                    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking, nullptr);
  if (pipeline_cache_) {
    pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
  }
  if (completion_callback) {
    completion_callback();
  }
}

bool D3D11CommandProcessor::CreateDrawResolutionScaleDependentCaches(
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y) {
  // The render target cache object is kept (shut down) across scale changes:
  // the pipeline cache refers to it.
  if (render_target_cache_) {
    render_target_cache_->SetDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);
  } else {
    render_target_cache_ = std::make_unique<D3D11RenderTargetCache>(
        *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  }
  if (!render_target_cache_->Initialize()) {
    REXGPU_WARN("Failed to initialize the render target cache for {}x{} draw resolution scale",
                draw_resolution_scale_x, draw_resolution_scale_y);
    return false;
  }
  texture_cache_ = std::make_unique<D3D11TextureCache>(
      *register_file_, *shared_memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!texture_cache_->Initialize()) {
    REXGPU_WARN("Failed to initialize the texture cache for {}x{} draw resolution scale",
                draw_resolution_scale_x, draw_resolution_scale_y);
    texture_cache_.reset();
    render_target_cache_->Shutdown();
    return false;
  }
  texture_cache_->BeginSubmission(frame_current_);
  texture_cache_->SetLogMissingTextures(frames_logged_ <
                                        uint32_t(std::max(REXCVAR_GET(d3d11_log_frames), 0)));
  return true;
}

void D3D11CommandProcessor::ApplyDrawResolutionScaleRequest() {
  uint32_t requested_scale_x, requested_scale_y;
  bool force_rebuild;
  if (!TakeDrawResolutionScaleRequest(requested_scale_x, requested_scale_y, force_rebuild)) {
    return;
  }
  uint32_t old_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t old_scale_y = texture_cache_->draw_resolution_scale_y();
  if (!force_rebuild && requested_scale_x == old_scale_x && requested_scale_y == old_scale_y) {
    return;
  }
  // Direct3D 11 keeps the objects the GPU still uses alive, so they can be
  // destroyed right away. Everything with the old scale baked in goes before
  // anything is created for the new one, not to need memory for both.
  InvalidateDrawState();
  pipeline_cache_->DestroyDrawResolutionScaleDependentObjects();
  texture_cache_->InvalidateScaledResolveRangesInSharedMemory();
  texture_cache_.reset();
  render_target_cache_->Shutdown();

  // Recreate for the new scale, or for 1x if that fails (not enough memory).
  uint32_t new_scale_x = requested_scale_x;
  uint32_t new_scale_y = requested_scale_y;
  bool caches_created = CreateDrawResolutionScaleDependentCaches(new_scale_x, new_scale_y);
  if (!caches_created && (new_scale_x > 1 || new_scale_y > 1)) {
    new_scale_x = 1;
    new_scale_y = 1;
    caches_created = CreateDrawResolutionScaleDependentCaches(new_scale_x, new_scale_y);
  }
  if (!caches_created || !pipeline_cache_->CreateDrawResolutionScaleDependentObjects()) {
    // This has also succeeded at startup, though.
    rex::FatalError("Failed to recreate the GPU emulation state for a draw resolution scale");
  }
  render_target_cache_->InvalidateBoundRenderTargets();

  // Occlusion query sample counts are normalized by the scale.
  zpd_draw_resolution_scale_x_ = new_scale_x;
  zpd_draw_resolution_scale_y_ = new_scale_y;
  // Only for scaled resolves, recreated when needed.
  if (new_scale_x <= 1 && new_scale_y <= 1) {
    resolve_downscale_uav_.Reset();
    resolve_downscale_buffer_.Reset();
    resolve_downscale_buffer_size_ = 0;
  }

  if (new_scale_x != requested_scale_x || new_scale_y != requested_scale_y) {
    REXGPU_WARN(
        "GPU: draw resolution scale changed to {}x{} - failed to set up {}x{} (not enough GPU "
        "memory?)",
        new_scale_x, new_scale_y, requested_scale_x, requested_scale_y);
  } else {
    REXGPU_INFO("GPU: draw resolution scale changed to {}x{}", new_scale_x, new_scale_y);
  }
}

void D3D11CommandProcessor::ShutdownContext() {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ShutdownQueryResources();
  {
    std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
    provider.context_mutex().SetOwnerChangeCallback(nullptr);
  }
  {
    std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
    ShutdownGpuTime();
    if (ID3D11DeviceContext* context = provider.GetImmediateContext()) {
      context->ClearState();
    }
  }
  current_pipeline_ = nullptr;
  draw_state_valid_ = false;

  debug_white_sampler_.Reset();
  for (auto& srv : debug_white_srvs_) {
    srv.Reset();
  }
  temporary_output_srv_.Reset();
  temporary_output_resolved_.Reset();
  temporary_output_rasterizer_state_.Reset();
  temporary_output_sampler_.Reset();
  temporary_output_ps_constants_.Reset();
  temporary_output_vs_constants_.Reset();
  temporary_output_ps_.Reset();
  temporary_output_vs_.Reset();
  if (readback_buffer_mapped_) {
    provider.GetImmediateContext()->Unmap(readback_buffer_.Get(), 0);
    readback_buffer_mapped_ = false;
  }
  readback_buffer_.Reset();
  readback_buffer_size_ = 0;
  for (auto* buffer_map : {&readback_buffers_, &memexport_readback_buffers_}) {
    for (auto& buffer_pair : *buffer_map) {
      for (ReadbackSlot& slot : buffer_pair.second.slots) {
        if (slot.mapping) {
          provider.GetImmediateContext()->Unmap(slot.buffer.Get(), 0);
        }
      }
    }
    buffer_map->clear();
  }
  deferred_readback_slots_.clear();
  resolve_downscale_uav_.Reset();
  resolve_downscale_buffer_.Reset();
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_constants_.Reset();
  resolve_downscale_cs_.Reset();
  smaa_search_srv_.Reset();
  smaa_area_srv_.Reset();
  for (PostEffectTexture& smaa_texture : smaa_textures_) {
    smaa_texture = PostEffectTexture();
  }
  for (Microsoft::WRL::ComPtr<ID3D11ComputeShader>& smaa_cs : smaa_cs_) {
    smaa_cs.Reset();
  }
  fxaa_source_texture_ = PostEffectTexture();
  fxaa_extreme_cs_.Reset();
  fxaa_cs_.Reset();
  post_effect_sampler_.Reset();
  post_effect_constants_.Reset();
  swap_pixel_shaders_.clear();
  swap_vs_.Reset();
  gamma_ramp_pwl_srv_.Reset();
  gamma_ramp_table_srv_.Reset();
  gamma_ramp_buffer_.Reset();
  scratch_index_buffer_.Reset();
  scratch_index_buffer_size_ = 0;
  constant_buffer_fetch_.Reset();
  constant_buffer_bool_loop_.Reset();
  constant_buffer_float_pixel_.Reset();
  constant_buffer_float_vertex_.Reset();
  constant_buffer_system_.Reset();

  pipeline_cache_.reset();
  texture_cache_.reset();
  render_target_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();
  CommandProcessor::ShutdownContext();
}

void D3D11CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  if (render_target_cache_) {
    std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
    render_target_cache_->ClearCache();
    texture_cache_->ClearCache();
  }
}

void D3D11CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D11CommandProcessor::InvalidateDrawState() {
  draw_state_valid_ = false;
  current_pipeline_ = nullptr;
}

void D3D11CommandProcessor::BeginContextUse() {
  uint64_t epoch = GetD3D11Provider().context_mutex().epoch();
  if (epoch != context_epoch_) {
    context_epoch_ = epoch;
    InvalidateDrawState();
    render_target_cache_->InvalidateBoundRenderTargets();
  }
}

void D3D11CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // Only the shader constant blocks are handled here
  // (CommandProcessor::IsPlainRegister relies on that for the other registers).
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (float_constant_index >= 256) {
      float_constant_index -= 256;
      if (current_float_constant_map_pixel_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    } else {
      if (current_float_constant_map_vertex_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
  }
}

void D3D11CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    // Coarser than D3D12 (which checks the used constants of the range).
    if (start_index < XE_GPU_REG_SHADER_CONSTANT_256_X) {
      cbuffer_binding_float_vertex_.up_to_date = false;
    }
    if (end_index >= XE_GPU_REG_SHADER_CONSTANT_256_X) {
      cbuffer_binding_float_pixel_.up_to_date = false;
    }
    return;
  }
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(
          (start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6,
          (end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D11CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  EndZPDFrame();
  ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
  if (presenter) {
    system::X_VIDEO_MODE video_mode;
    kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
    uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

    // The front buffer (texture fetch constant 0), as in D3D12; the largest
    // render target of the frame is shown if it's unavailable.
    ID3D11ShaderResourceView* swap_srv;
    uint32_t swap_host_swizzle = 0;
    xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
    uint32_t swap_width = 0, swap_height = 0;
    bool swap_scaled = false;
    {
      std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
      BeginContextUse();
      swap_srv = texture_cache_->RequestSwapTexture(swap_host_swizzle, swap_format, swap_width,
                                                    swap_height, swap_scaled);
    }
    uint32_t guest_output_width, guest_output_height;
    if (swap_srv) {
      // The active region in guest pixels, then in the texture's.
      guest_output_width = frontbuffer_width ? std::min(frontbuffer_width, swap_width) : swap_width;
      guest_output_height =
          frontbuffer_height ? std::min(frontbuffer_height, swap_height) : swap_height;
      if (swap_scaled) {
        guest_output_width *= texture_cache_->draw_resolution_scale_x();
        guest_output_height *= texture_cache_->draw_resolution_scale_y();
      } else if (texture_cache_->IsDrawResolutionScaled() && !swap_unscaled_logged_) {
        swap_unscaled_logged_ = true;
        REXGPU_WARN(
            "Direct3D 11 backend: draw resolution scaling is enabled, but the front buffer is "
            "unscaled ({}x{}); the title may present from a resolve that isn't scaled",
            guest_output_width, guest_output_height);
      }
    } else {
      if (!swap_texture_failure_logged_) {
        swap_texture_failure_logged_ = true;
        xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
        REXGPU_WARN(
            "Direct3D 11 backend: no front buffer texture (fetch constant 0: {:08X} {:08X} "
            "{:08X} {:08X} {:08X} {:08X}), showing the largest render target of the frame",
            fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
            fetch.dword_5);
      }
      guest_output_width = frontbuffer_width ? frontbuffer_width : display_width;
      guest_output_height = frontbuffer_height ? frontbuffer_height : display_height;
    }
    // This is according to D3D::InitializePresentationParameters from a game
    // executable (see D3D12CommandProcessor::IssueSwap).
    bool use_pwl_gamma_ramp = swap_format == xenos::TextureFormat::k_2_10_10_10 ||
                              swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

    presenter->RefreshGuestOutput(
        guest_output_width, guest_output_height, display_width, display_height,
        [this, guest_output_width, guest_output_height, swap_srv, swap_host_swizzle,
         use_pwl_gamma_ramp, swap_format](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& d3d11_context =
              static_cast<ui::d3d11::D3D11Presenter::D3D11GuestOutputRefreshContext&>(context);
          const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
          std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
          BeginContextUse();
          ScopedGpuSection swap_section(*this, GpuSection::kSwap);
          if (frames_logged_ < uint32_t(std::max(REXCVAR_GET(d3d11_log_frames), 0))) {
            ++frames_logged_;
            D3D11TextureCache::Stats& texture_stats = texture_cache_->stats();
            DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
            uint32_t output_width = 0, output_height = 0;
            bool has_output = render_target_cache_->GetTemporaryGuestOutput(
                                  output_format, output_width, output_height) != nullptr;
            std::string shown =
                swap_srv ? fmt::format("front buffer {}x{}, {}", guest_output_width,
                                       guest_output_height, FormatInfo::Get(swap_format)->name)
                : has_output ? fmt::format("{}x{} render target, DXGI format {}", output_width,
                                           output_height, uint32_t(output_format))
                             : std::string("nothing (black)");
            REXGPU_INFO(
                "Direct3D 11 frame {}: {} draws ({} submitted, {} with failed pipelines, {} skipped "
                "while their shaders were created), "
                "{} resolves ({} wrote memory), {} textures bound, {} fetches without a "
                "texture (fetch constant {}, dimension {}, not created {}, view {}), {} "
                "texture loads ({} failed); shown: {}",
                frame_current_, frame_stats_.draws_issued, frame_stats_.draws_submitted,
                frame_stats_.draws_failed_pipeline, frame_stats_.draws_pending_pipeline,
                frame_stats_.copies,
                frame_stats_.copies_written, frame_stats_.textures_bound,
                frame_stats_.textures_missing - texture_stats.sign_unused,
                texture_stats.no_fetch_constant, texture_stats.dimension_mismatch,
                texture_stats.no_texture, texture_stats.no_view, texture_stats.loads,
                texture_stats.load_failures, shown);
          }
          bool post_effect;
          if (swap_srv &&
              DrawSwapTexture(d3d11_context.rtv(), d3d11_context.uav(), guest_output_width,
                              guest_output_height, swap_srv, swap_host_swizzle,
                              use_pwl_gamma_ramp, post_effect)) {
            context.SetIs8bpc(!use_pwl_gamma_ramp && !post_effect);
            return true;
          }
          DrawTemporaryGuestOutput(d3d11_context.rtv(), guest_output_width, guest_output_height);
          context.SetIs8bpc(true);
          return true;
        });
  }

  {
    std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
    EndGpuTimeFrame();
    ReadGpuTimes();
    CheckDeviceRemoved();
    ++frame_current_;
    frame_stats_ = FrameStats();
    if (texture_cache_) {
      texture_cache_->stats() = D3D11TextureCache::Stats();
      texture_cache_->SetLogMissingTextures(
          frames_logged_ < uint32_t(std::max(REXCVAR_GET(d3d11_log_frames), 0)));
    }
    if (primitive_processor_) {
      primitive_processor_->EndFrame();
    }
    if (texture_cache_) {
      // Direct3D 11 keeps resources the GPU still uses alive, so the previous
      // frame's textures can be destroyed already.
      texture_cache_->CompletedSubmissionUpdated(frame_current_ - 1);
      texture_cache_->BeginSubmission(frame_current_);
      texture_cache_->BeginFrame();
    }
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
      render_target_cache_->AdvanceRenderTargetUseFrame();
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);
    if (pipeline_cache_) {
      pipeline_cache_->EndFrame();
    }
    // Between frames: a resolution scale change while the game runs.
    if (texture_cache_ && render_target_cache_ && pipeline_cache_) {
      ApplyDrawResolutionScaleRequest();
    }
    BeginGpuTimeFrame();
  }
}

Shader* D3D11CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

// Follows D3D12CommandProcessor::IssueDraw for the RTV path, with native
// occlusion queries only.
bool D3D11CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  const RegisterFile& regs = *register_file_;
  ++frame_stats_.draws_issued;

  // VIZ survey geometry associated with a specific 0:63 ID.
  // draw_util keeps it normalized, so it only counts coverage here.
  const bool viz_survey = draw_util::IsVIZSurveyDraw(regs);

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    if (viz_survey) {
      OnVIZSurveyDraw(false);
      return true;
    }
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D11Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    return true;
  }
  D3D11Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D11Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  // A draw contributing to its VIZ ID without the kill bit counts
  // at hi-Z before the pixel shader can reject anything.
  if (!viz_survey && pixel_shader &&
      (pixel_shader->kills_pixels() ||
       (pixel_shader->writes_color_target(0) &&
        draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>())))) {
    OnVIZSurveyDraw(false);
  }

  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  BeginContextUse();
  if (device_removed_) {
    return false;
  }
  ID3D11DeviceContext* context = provider.GetImmediateContext();

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
  bool apply_host_depth_polygon_offset =
      pixel_shader && !pixel_shader->writes_depth() &&
      draw_util::GetHostDepthPolygonOffsetIfNeeded(regs, primitive_polygonal,
                                                   normalized_depth_control, normalized_color_mask,
                                                   host_depth_polygon_offset);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control, apply_host_depth_polygon_offset)
                   : DxbcShaderTranslator::Modification(0);
  // Hybrid occlusion query draw, counting coverage into the Total counter, as
  // D3D12: only depth or stencil tested draws without depth writes, so scene
  // geometry keeps early depth rejection, and nothing can fail without a test
  // anyway. Surveys never count for a report.
  bool zpd_hybrid = zpd_hybrid_supported_ && zpd_counter_uav_ &&
                    active_segment_.report_measuring() && !viz_survey &&
                    !normalized_depth_control.z_write_enable &&
                    (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable);
  // For drawing without the counting while the shaders are being created.
  DxbcShaderTranslator::Modification pixel_shader_modification_without_zpd =
      pixel_shader_modification;
  if (zpd_hybrid && pixel_shader) {
    pixel_shader_modification.pixel.zpd_total = 1;
    // The counter UAV write disables early depth/stencil.
    if (pixel_shader_modification.pixel.depth_stencil_mode ==
        DxbcShaderTranslator::Modification::DepthStencilMode::kEarlyHint) {
      pixel_shader_modification.pixel.depth_stencil_mode =
          DxbcShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
    }
  }

  // Put the window offset into the EDRAM bases when possible.
  int32_t window_offset_tiles = render_target_cache_->GetWindowOffsetTiles(
      regs, normalized_depth_control, normalized_color_mask, frame_current_);

  // Set up the render targets - this may perform draws (clears).
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader, window_offset_tiles)) {
    return false;
  }

  auto vertex_shader_translation = static_cast<D3D11Shader::D3D11Translation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  auto pixel_shader_translation =
      pixel_shader ? static_cast<D3D11Shader::D3D11Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  // The texture and sampler bindings don't depend on zpd_total (only its
  // translation is needed if the counting one isn't ready yet).
  auto pixel_shader_translation_without_zpd =
      (pixel_shader && zpd_hybrid)
          ? static_cast<D3D11Shader::D3D11Translation*>(pixel_shader->GetOrCreateTranslation(
                pixel_shader_modification_without_zpd.value))
          : pixel_shader_translation;

  // Load the textures before the pipeline is chosen: its shaders apply the
  // textures' channel mappings. The bindings are known after the translation.
  uint32_t used_texture_mask = 0;
  vertex_texture_bindings_.srvs.clear();
  vertex_texture_bindings_.swizzles.clear();
  vertex_texture_bindings_.samplers.clear();
  pixel_texture_bindings_.srvs.clear();
  pixel_texture_bindings_.swizzles.clear();
  pixel_texture_bindings_.samplers.clear();
  if (!debug_white_srvs_[0]) {
    pipeline_cache_->EnsureTranslated(*vertex_shader_translation);
    if (pixel_shader_translation) {
      pipeline_cache_->EnsureTranslated(*pixel_shader_translation);
    }
    used_texture_mask =
        vertex_shader->GetUsedTextureMaskAfterTranslation() |
        (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
    texture_cache_->RequestTextures(used_texture_mask);
    GetStageTextureBindings(*vertex_shader, vertex_texture_bindings_);
    if (pixel_shader) {
      GetStageTextureBindings(*pixel_shader, pixel_texture_bindings_);
    }
  }

  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets];
  uint32_t bound_depth_and_color_render_target_bits =
      render_target_cache_->GetLastUpdateBoundRenderTargets(
          bound_depth_and_color_render_target_formats);
  bool pipeline_pending;
  auto configure_pipeline = [&]() {
    return pipeline_cache_->ConfigurePipeline(
        vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
        normalized_depth_control, normalized_color_mask, apply_host_depth_polygon_offset,
        memexport_used, zpd_hybrid, bound_depth_and_color_render_target_bits,
        bound_depth_and_color_render_target_formats, vertex_texture_bindings_.swizzles,
        pixel_texture_bindings_.swizzles, pipeline_pending);
  };
  const PipelineCache::Pipeline* pipeline = configure_pipeline();
  if (!pipeline && pipeline_pending && zpd_hybrid) {
    // As D3D12: draw without the counting while the counting variant is
    // being created (the query counts passed samples only then).
    zpd_hybrid = false;
    pixel_shader_translation = pixel_shader_translation_without_zpd;
    pipeline = configure_pipeline();
  }
  if (!pipeline && pipeline_pending) {
    // Shaders still being created on the creation threads: wait up to
    // async_pipeline_wait_ms, then skip the draw (pop-in), as D3D12.
    WaitForPipelineCreation(
        [&]() {
          pipeline = configure_pipeline();
          return pipeline || !pipeline_pending;
        },
        REXCVAR_GET(async_pipeline_wait_ms));
    if (!pipeline && pipeline_pending) {
      ++frame_stats_.draws_pending_pipeline;
      OnVIZSurveyDraw(false);
      return true;
    }
  }
  if (!pipeline) {
    // Logged by the pipeline cache.
    ++frame_stats_.draws_failed_pipeline;
    if (!pipeline_failure_logged_) {
      pipeline_failure_logged_ = true;
      REXGPU_WARN("Direct3D 11 backend: draws with pipelines that failed are skipped");
    }
    OnVIZSurveyDraw(false);
    return true;
  }

  // The resolution scale of this draw, which may be 1x1 because of
  // draw_resolution_scale_threshold.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Segments can't mix scales, hybrid Total counting, or VIZ IDs: the
  // resolved sample count is divided by one scale area per segment. Before
  // the counter index goes into the system constants. A segment whose query
  // another thread's context use ended goes on in a new one.
  if (active_query_interrupted_) {
    CloseQuerySegment();
  }
  UpdateQuerySegment(draw_resolution_scale_x * draw_resolution_scale_y, zpd_hybrid, viz_survey);

  bool convert_z_to_float24 = render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                 D3D11_VIEWPORT_BOUNDS_MAX, D3D11_VIEWPORT_BOUNDS_MAX, false,
                                 normalized_depth_control, convert_z_to_float24, true,
                                 ps_writes_depth, viewport_info, window_offset_tiles != 0, false);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true, window_offset_tiles != 0);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  if (!draw_state_valid_) {
    context->IASetInputLayout(nullptr);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    current_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::memset(&current_viewport_, 0, sizeof(current_viewport_));
    std::memset(&current_scissor_, 0, sizeof(current_scissor_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_vertex_texture_bindings_ = StageTextureBindings();
    current_pixel_texture_bindings_ = StageTextureBindings();
  }

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control,
                           normalized_color_mask, bound_depth_and_color_render_target_bits);

  if (!draw_state_valid_ || current_pipeline_ != pipeline) {
    context->VSSetShader(pipeline->vertex_shader, nullptr, 0);
    context->HSSetShader(pipeline->hull_shader, nullptr, 0);
    context->DSSetShader(pipeline->domain_shader, nullptr, 0);
    context->GSSetShader(pipeline->geometry_shader, nullptr, 0);
    context->PSSetShader(pipeline->pixel_shader, nullptr, 0);
    context->RSSetState(pipeline->rasterizer_state.Get());
    context->OMSetDepthStencilState(pipeline->depth_stencil_state.Get(), current_stencil_ref_);
    context->OMSetBlendState(pipeline->blend_state.Get(), current_blend_factor_,
                             pipeline->sample_mask);
    current_pipeline_ = pipeline;
    if (debug_white_srvs_[0]) {
      BindDebugWhiteTextures(vertex_shader, pixel_shader);
    }
  }

  // Update system constants before uploading them.
  UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask,
                             apply_host_depth_polygon_offset ? &host_depth_polygon_offset : nullptr,
                             window_offset_tiles);

  // Update constant buffers and resource bindings.
  if (!UpdateBindings(vertex_shader, pixel_shader, zpd_hybrid, memexport_used)) {
    return false;
  }
  BindStageTextures(false, vertex_texture_bindings_);
  BindStageTextures(true, pixel_texture_bindings_);
  draw_state_valid_ = true;

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  {
    uint32_t vfetch_addresses[96];
    uint32_t vfetch_sizes[96];
    uint32_t vfetch_current_queued = 0;
    for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
        switch (vfetch_constant.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
              break;
            }
            REXGPU_WARN(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
                "This is incorrect behavior, but you can try bypassing this by "
                "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
          default:
            if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
              break;
            }
            REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                        vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
        }
        // Mask to physical like the shader - the guest may use a mirror window.
        vfetch_addresses[vfetch_current_queued] = xenos::CpuToGpu(vfetch_constant.address << 2) >> 2;
        vfetch_sizes[vfetch_current_queued++] = vfetch_constant.size;
      }
    }
    if (vfetch_current_queued) {
      // Pre-acquire the critical region so it's not repeatedly re-acquired in
      // RequestRange.
      auto shared_memory_request_range_hoisted =
          rex::thread::global_critical_region::AcquireDirect();
      for (uint32_t i = 0; i < vfetch_current_queued; ++i) {
        if (!shared_memory_->RequestRange(vfetch_addresses[i] << 2, vfetch_sizes[i] << 2)) {
          REXGPU_ERROR("Failed to request vertex buffer at 0x{:08X} (size {}) in the shared memory",
                       vfetch_addresses[i] << 2, vfetch_sizes[i] << 2);
          return false;
        }
      }
    }
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  std::vector<draw_util::MemExportRange> memexport_ranges;
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges);
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges) {
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR("Failed to request memexport stream at 0x{:08X} (size {}) in the shared memory",
                   memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return false;
    }
  }
  if (memexport_used && memexport_ranges.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved memexport destinations");
      return false;
    }
  }

  // Primitive topology.
  D3D11_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    // As on Direct3D 12.
    bool adaptive =
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology = adaptive ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                      : D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology = adaptive ? D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                      : D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 11 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not supported by the "
            "Direct3D 11 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  }
  if (current_topology_ != primitive_topology) {
    current_topology_ = primitive_topology;
    context->IASetPrimitiveTopology(primitive_topology);
  }

  // Snapshot the export ranges before the draw so the readback after it only
  // copies the bytes the shaders actually changed. Copying the whole range
  // back would overwrite anything the CPU wrote there while the draw was in
  // flight, since the ranges are usually much larger than what gets exported.
  // Before the predication, which applies to copies too.
  bool memexport_snapshot_taken = false;
  if (!memexport_ranges.empty() && !REXCVAR_GET(readback_memexport_fast) &&
      IsReadbackMemexportEnabled(false)) {
    readback_ranges_.clear();
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges) {
      ReadbackRange& range = readback_ranges_.emplace_back();
      range.address = memexport_range.base_address_dwords << 2;
      range.length = memexport_range.size_bytes;
    }
    std::vector<const uint8_t*> data;
    ReadBackSharedMemory(readback_ranges_.data(), readback_ranges_.size(), data);
    memexport_snapshot_taken = true;
    memexport_snapshot_.clear();
    for (size_t i = 0; i < readback_ranges_.size(); ++i) {
      if (!data[i]) {
        memexport_snapshot_taken = false;
        break;
      }
      memexport_snapshot_.insert(memexport_snapshot_.end(), data[i],
                                 data[i] + readback_ranges_[i].length);
    }
  }

  // Consumer VIZ draws run under the survey's predicate instead of blocking:
  // skipped only when the survey saw nothing.
  ID3D11Predicate* predicate = nullptr;
  if (IsVIZPredicateArmed()) {
    predicate = viz_predicates_[viz_draw_predicate_.id].Get();
  }
  // Reset on every way out of the draw.
  struct PredicationScope {
    ID3D11DeviceContext* context;
    ID3D11Predicate* predicate;
    ~PredicationScope() {
      if (predicate) {
        context->SetPredication(nullptr, FALSE);
      }
    }
  } predication_scope{context, predicate};
  if (predicate) {
    context->SetPredication(predicate, FALSE);
  }

  // Draw.
  ++frame_stats_.draws_submitted;
  OnVIZSurveyDraw(true);
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    context->Draw(primitive_processing_result.host_draw_vertex_count, 0);
  } else {
    DXGI_FORMAT index_format;
    uint32_t index_size;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_format = DXGI_FORMAT_R16_UINT;
      index_size = sizeof(uint16_t);
    } else {
      index_format = DXGI_FORMAT_R32_UINT;
      index_size = sizeof(uint32_t);
    }
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (memexport_used) {
          // The shared memory is bound as a UAV, which an index buffer binding
          // would conflict with.
          ID3D11Buffer* scratch_index_buffer = CopyIndicesToScratchBuffer(
              primitive_processing_result.guest_index_base,
              primitive_processing_result.host_draw_vertex_count * index_size);
          if (!scratch_index_buffer) {
            return false;
          }
          context->IASetIndexBuffer(scratch_index_buffer, index_format, 0);
        } else {
          context->IASetIndexBuffer(shared_memory_->GetBuffer(), index_format,
                                    primitive_processing_result.guest_index_base);
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        if (!primitive_processor_->BindConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle, index_format)) {
          return false;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        context->IASetIndexBuffer(primitive_processor_->GetBuiltinIndexBuffer(), index_format,
                                  primitive_processor_->GetBuiltinIndexBufferOffset(
                                      primitive_processing_result.host_index_buffer_handle));
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    context->DrawIndexed(primitive_processing_result.host_draw_vertex_count, 0, 0);
  }

  if (memexport_used) {
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    // readback_memexport: as D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath
    // or IssueDraw_MemexportReadbackFullPath.
    if (!memexport_ranges.empty() && IsReadbackMemexportEnabled(false) &&
        REXCVAR_GET(readback_memexport_fast)) {
      ReadBackMemexportFast(memexport_ranges);
    } else if (memexport_snapshot_taken) {
      std::vector<const uint8_t*> data;
      ReadBackSharedMemory(readback_ranges_.data(), readback_ranges_.size(), data);
      const uint8_t* before_bytes = memexport_snapshot_.data();
      for (size_t i = 0; i < readback_ranges_.size(); ++i) {
        uint8_t* guest = memory_->TranslatePhysical(readback_ranges_[i].address);
        if (data[i] && guest) {
          draw_util::WriteBackChangedBytes(guest, before_bytes, data[i],
                                           readback_ranges_[i].length);
        }
        before_bytes += readback_ranges_[i].length;
      }
    }
  }

  return true;
}

bool D3D11CommandProcessor::IssueCopy() {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  BeginContextUse();
  if (device_removed_) {
    return false;
  }
  ScopedGpuSection resolve_section(*this, GpuSection::kResolves);
  ++frame_stats_.copies;
  uint32_t written_address, written_length;
  reg::RB_COPY_DEST_INFO copy_dest_info;
  bool written_scaled;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length, copy_dest_info, written_scaled)) {
    return false;
  }
  if (written_length) {
    ++frame_stats_.copies_written;
    // readback_resolve: the guest's CPU may read what the resolve wrote.
    if (GetReadbackResolveMode(false) != ReadbackResolveMode::kDisabled &&
        memory_->TranslatePhysical(written_address)) {
      ReadBackResolve(written_address, written_length, copy_dest_info, written_scaled);
    }
  }
  return true;
}

void D3D11CommandProcessor::ReadBackResolve(uint32_t written_address, uint32_t written_length,
                                            reg::RB_COPY_DEST_INFO copy_dest_info,
                                            bool written_scaled) {
  // With resolution scaling, only the whole 32x32 tiles the downscale handles
  // are copied back.
  uint32_t pixel_size_log2;
  uint32_t readback_length = GetResolveReadbackLength(written_address, written_length,
                                                      copy_dest_info, written_scaled,
                                                      pixel_size_log2);
  if (!readback_length) {
    return;
  }
  uint64_t resolve_key = GetReadbackResolveKey(written_address, written_length);
  // Only what the guest's CPU accesses (readback_resolve_cpu_reads_only).
  if (!ShouldReadbackResolve(written_address, readback_length, resolve_key, pixel_size_log2,
                             written_scaled)) {
    return;
  }
  if (REXCVAR_GET(readback_resolve_log) && !readback_buffers_.count(resolve_key)) {
    REXGPU_INFO("Resolve readback: new destination {:08X}, {} bytes, format {}{}",
                written_address, readback_length, uint32_t(copy_dest_info.copy_dest_format),
                written_scaled ? ", downscaled" : "");
  }
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;
  uint32_t write_index = rb.current_index;
  ReadbackSlot& write_slot = rb.slots[write_index];
  if (!PrepareReadbackSlotForCopy(write_slot, readback_length)) {
    return;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(false);
  // "fast" and "some" hand the CPU the previous resolve's copy, which is done
  // by now, instead of waiting for this one.
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  // A full readback of a destination the guest reads in pieces
  // (readback_resolve_cpu_pages_only): only those cross from the GPU. Delayed
  // readbacks read the slot the previous resolve wrote, which must then hold
  // all of it.
  bool all_pieces = true;
  if (!use_delayed_sync && !written_scaled) {
    all_pieces = GetResolveCopyPieces(written_address, readback_length, readback_pieces_);
  }
  ReadbackRange range;
  range.address = written_address;
  range.length = readback_length;
  range.scaled = written_scaled;
  range.pixel_size_log2 = pixel_size_log2;
  if (!CopyReadbackRange(range, write_slot.buffer.Get(), 0,
                         all_pieces ? nullptr : &readback_pieces_)) {
    return;
  }
  write_slot.written_size = readback_length;
  write_slot.partial = !all_pieces;

  // readback_resolve_full_deferred: the guest gets this copy at its next
  // access to the destination (CompleteDeferredReadbacks maps the slot then),
  // so this thread doesn't wait for the GPU here.
  if (!use_delayed_sync &&
      DeferResolveReadback(written_address, readback_length, all_pieces, readback_pieces_)) {
    deferred_readback_slots_[(uint64_t(written_address & 0x1FFFFFFF) << 32) | readback_length] = {
        resolve_key, write_index};
    rb.current_index = 1 - rb.current_index;
    return;
  }

  uint32_t read_index = write_index;
  bool is_cache_miss = false;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
    const ReadbackSlot& read_slot = rb.slots[read_index];
    if (!read_slot.buffer || read_slot.written_size < readback_length || read_slot.partial) {
      // No previous copy of all of it: wait for this one.
      is_cache_miss = true;
      read_index = write_index;
    }
  }
  bool should_copy = readback_mode != ReadbackResolveMode::kSome || is_cache_miss;
  if (should_copy) {
    if (const uint8_t* data = MapReadbackSlot(rb.slots[read_index], true)) {
      if (all_pieces) {
        CopyResolveToGuest(written_address, readback_length, data);
      } else {
        CopyResolvePiecesToGuest(written_address, readback_length, data, false, readback_pieces_);
      }
    }
  }
  rb.current_index = 1 - rb.current_index;
}

void D3D11CommandProcessor::ReadBackResolvesNow(const ResolveFetch* fetches, size_t count,
                                                std::vector<const uint8_t*>& data_out) {
  data_out.assign(count, nullptr);
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
  BeginContextUse();
  // Each into its destination's own readback buffer, in the slot its next
  // resolve writes, so the double buffering of readback_resolve stays as it
  // is. All copies first, then the maps.
  std::vector<ReadbackSlot*> slots(count, nullptr);
  for (size_t i = 0; i < count; ++i) {
    const ResolveFetch& fetch = fetches[i];
    ReadbackBuffer& rb = readback_buffers_[fetch.buffer_key];
    rb.last_used_frame = frame_current_;
    ReadbackSlot& slot = rb.slots[rb.current_index];
    if (!PrepareReadbackSlotForCopy(slot, fetch.length)) {
      continue;
    }
    ReadbackRange range;
    range.address = fetch.address;
    range.length = fetch.length;
    range.scaled = fetch.scaled;
    range.pixel_size_log2 = fetch.pixel_size_log2;
    if (!CopyReadbackRange(range, slot.buffer.Get(), 0)) {
      continue;
    }
    slot.written_size = fetch.length;
    slot.partial = false;
    slots[i] = &slot;
  }
  for (size_t i = 0; i < count; ++i) {
    if (slots[i]) {
      data_out[i] = MapReadbackSlot(*slots[i], true);
    }
  }
}

void D3D11CommandProcessor::CompleteDeferredReadbacks(const ResolveFetch* fetches, size_t count,
                                                      std::vector<const uint8_t*>& data_out) {
  data_out.assign(count, nullptr);
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
  BeginContextUse();
  for (size_t i = 0; i < count; ++i) {
    auto slot_it = deferred_readback_slots_.find((uint64_t(fetches[i].address) << 32) |
                                                 uint64_t(fetches[i].length));
    if (slot_it == deferred_readback_slots_.end()) {
      continue;
    }
    auto buffer_it = readback_buffers_.find(slot_it->second.buffer_key);
    if (buffer_it == readback_buffers_.end()) {
      continue;
    }
    ReadbackSlot& slot = buffer_it->second.slots[slot_it->second.index];
    if (!slot.buffer || slot.written_size < fetches[i].length) {
      continue;
    }
    // Waits for the copy into this slot only.
    data_out[i] = MapReadbackSlot(slot, true);
  }
}

bool D3D11CommandProcessor::PrepareReadbackSlotForCopy(ReadbackSlot& slot, uint32_t size) {
  if (slot.mapping) {
    GetD3D11Provider().GetImmediateContext()->Unmap(slot.buffer.Get(), 0);
    slot.mapping = nullptr;
  }
  slot.written_size = 0;
  slot.partial = false;
  if (slot.buffer && slot.size >= size) {
    return true;
  }
  // As D3D12's AlignReadbackBufferSize.
  uint32_t aligned_size;
  if (size < (uint32_t(1) << 20)) {
    aligned_size = rex::align(size, uint32_t(256) << 10);
  } else if (size < (uint32_t(4) << 20)) {
    aligned_size = rex::align(size, uint32_t(1) << 20);
  } else {
    aligned_size = rex::align(size, uint32_t(16) << 20);
  }
  slot.buffer.Reset();
  slot.size = 0;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = aligned_size;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(GetD3D11Provider().GetDevice()->CreateBuffer(&desc, nullptr, &slot.buffer))) {
    REXGPU_ERROR("Failed to create a {} MB readback buffer", aligned_size >> 20);
    return false;
  }
  slot.size = aligned_size;
  return true;
}

const uint8_t* D3D11CommandProcessor::MapReadbackSlot(ReadbackSlot& slot, bool wait) {
  if (slot.mapping) {
    return slot.mapping;
  }
  if (!slot.buffer) {
    return nullptr;
  }
  D3D11_MAPPED_SUBRESOURCE mapping;
  HRESULT result = GetD3D11Provider().GetImmediateContext()->Map(
      slot.buffer.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapping);
  if (FAILED(result)) {
    if (result != DXGI_ERROR_WAS_STILL_DRAWING && !CheckDeviceRemoved()) {
      REXGPU_ERROR("Failed to map a readback buffer (0x{:08X})", uint32_t(result));
    }
    return nullptr;
  }
  slot.mapping = static_cast<const uint8_t*>(mapping.pData);
  return slot.mapping;
}

bool D3D11CommandProcessor::CheckDeviceRemoved() {
  if (device_removed_) {
    return true;
  }
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  HRESULT reason = provider.GetDevice()->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) {
    return false;
  }
  device_removed_ = true;
  const char* reason_name = "unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_name = "DEVICE_HUNG (a GPU command took too long, TDR)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_name = "DEVICE_REMOVED (driver internal error or the GPU was removed)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_name = "DEVICE_RESET (a bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_name = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_name = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR(
      "Direct3D 11 device removed: 0x{:08X} - {}; frame {}, {} draws and {} resolves in it; "
      "nothing more is drawn",
      uint32_t(reason), reason_name, frame_current_, frame_stats_.draws_issued,
      frame_stats_.copies);
  // Direct3D 11 has no DRED; the debug layer (d3d11_debug) may say more.
  provider.LogD3D11DebugMessages();
  rex::FlushLogging();
  if (graphics_system_) {
    graphics_system_->OnHostGpuLossFromAnyThread(reason != DXGI_ERROR_DEVICE_REMOVED);
  }
  return true;
}

void D3D11CommandProcessor::InitializeGpuTime() {
  gpu_time_enabled_ = false;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  gpu_time_enabled_ = REXCVAR_GET(d3d11_gpu_time);
#endif
}

void D3D11CommandProcessor::ShutdownGpuTime() {
  gpu_time_recording_ = false;
  gpu_time_current_ = GpuTimeFrame();
  gpu_time_submitted_.clear();
  gpu_time_free_disjoint_.clear();
  gpu_time_free_queries_.clear();
  gpu_time_queries_.clear();
  gpu_time_enabled_ = false;
}

bool D3D11CommandProcessor::AllocateGpuTimeQuery(uint32_t& index_out) {
  if (!gpu_time_free_queries_.empty()) {
    index_out = gpu_time_free_queries_.back();
    gpu_time_free_queries_.pop_back();
    return true;
  }
  if (gpu_time_queries_.size() >= kGpuTimeMaxQueries) {
    return false;
  }
  D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP, 0};
  Microsoft::WRL::ComPtr<ID3D11Query> query;
  if (FAILED(GetD3D11Provider().GetDevice()->CreateQuery(&desc, &query))) {
    return false;
  }
  index_out = uint32_t(gpu_time_queries_.size());
  gpu_time_queries_.push_back(std::move(query));
  return true;
}

void D3D11CommandProcessor::FreeGpuTimeFrame(GpuTimeFrame& frame) {
  gpu_time_free_queries_.push_back(frame.start_query);
  gpu_time_free_queries_.push_back(frame.end_query);
  for (const GpuTimeMark& mark : frame.marks) {
    gpu_time_free_queries_.push_back(mark.query);
  }
  if (frame.disjoint) {
    gpu_time_free_disjoint_.push_back(std::move(frame.disjoint));
  }
}

void D3D11CommandProcessor::BeginGpuTimeFrame() {
  if (!gpu_time_enabled_ || gpu_time_recording_ || device_removed_) {
    return;
  }
  gpu_time_current_ = GpuTimeFrame();
  if (!gpu_time_free_disjoint_.empty()) {
    gpu_time_current_.disjoint = std::move(gpu_time_free_disjoint_.back());
    gpu_time_free_disjoint_.pop_back();
  } else {
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(GetD3D11Provider().GetDevice()->CreateQuery(&desc,
                                                           &gpu_time_current_.disjoint))) {
      REXGPU_WARN("Direct3D 11: Failed to create a timestamp query, d3d11_gpu_time is off");
      gpu_time_enabled_ = false;
      return;
    }
  }
  if (!AllocateGpuTimeQuery(gpu_time_current_.start_query)) {
    gpu_time_free_disjoint_.push_back(std::move(gpu_time_current_.disjoint));
    return;
  }
  if (!AllocateGpuTimeQuery(gpu_time_current_.end_query)) {
    gpu_time_free_queries_.push_back(gpu_time_current_.start_query);
    gpu_time_free_disjoint_.push_back(std::move(gpu_time_current_.disjoint));
    return;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  context->Begin(gpu_time_current_.disjoint.Get());
  context->End(gpu_time_queries_[gpu_time_current_.start_query].Get());
  gpu_time_recording_ = true;
}

void D3D11CommandProcessor::EndGpuTimeFrame() {
  if (!gpu_time_recording_) {
    return;
  }
  gpu_time_recording_ = false;
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  context->End(gpu_time_queries_[gpu_time_current_.end_query].Get());
  context->End(gpu_time_current_.disjoint.Get());
  gpu_time_current_.last = gpu_section_;
  gpu_time_submitted_.push_back(std::move(gpu_time_current_));
  gpu_time_current_ = GpuTimeFrame();
  // Not read for a long time (the GPU is far behind or a read failed): drop
  // the oldest.
  while (gpu_time_submitted_.size() > kGpuTimeMaxFramesInFlight) {
    FreeGpuTimeFrame(gpu_time_submitted_.front());
    gpu_time_submitted_.pop_front();
  }
}

D3D11CommandProcessor::GpuSection D3D11CommandProcessor::SetGpuSection(GpuSection section) {
  GpuSection ended = gpu_section_;
  if (section == ended) {
    return ended;
  }
  gpu_section_ = section;
  // The work since the last timestamp is the ended section's.
  uint32_t query;
  if (gpu_time_recording_ && gpu_time_current_.marks.size() < kGpuTimeMaxMarksPerFrame &&
      AllocateGpuTimeQuery(query)) {
    GetD3D11Provider().GetImmediateContext()->End(gpu_time_queries_[query].Get());
    gpu_time_current_.marks.push_back({query, ended});
  }
  return ended;
}

void D3D11CommandProcessor::ReadGpuTimes() {
  if (gpu_time_submitted_.empty()) {
    return;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  uint64_t ticks[size_t(GpuSection::kCount)] = {};
  double ns_per_tick = 0.0;
  auto get_stamp = [&](uint32_t query, uint64_t& stamp_out) {
    return context->GetData(gpu_time_queries_[query].Get(), &stamp_out, sizeof(stamp_out),
                            D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
  };
  while (!gpu_time_submitted_.empty()) {
    GpuTimeFrame& frame = gpu_time_submitted_.front();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    HRESULT result = context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result == S_FALSE) {
      break;
    }
    if (result == S_OK && !disjoint.Disjoint && disjoint.Frequency) {
      // Written before the disjoint query ended, so done.
      uint64_t start, end;
      bool read = get_stamp(frame.start_query, start) && get_stamp(frame.end_query, end);
      gpu_time_marks_.clear();
      for (size_t i = 0; read && i < frame.marks.size(); ++i) {
        uint64_t stamp;
        read = get_stamp(frame.marks[i].query, stamp);
        gpu_time_marks_.push_back({stamp, frame.marks[i].ended});
      }
      if (read) {
        uint64_t frame_ticks[size_t(GpuSection::kCount)] = {};
        vulkan::AddGpuSectionTicks(start, gpu_time_marks_, end, frame.last, ~uint64_t(0),
                                   frame_ticks);
        double frame_ns_per_tick = 1.0e9 / double(disjoint.Frequency);
        // The frequency is constant while not disjoint; ticks of frames read
        // together are added up with the last one's.
        ns_per_tick = frame_ns_per_tick;
        for (size_t i = 0; i < size_t(GpuSection::kCount); ++i) {
          ticks[i] += frame_ticks[i];
        }
      }
    }
    FreeGpuTimeFrame(frame);
    gpu_time_submitted_.pop_front();
  }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr rex::perf::CounterId kCounters[size_t(GpuSection::kCount)] = {
      rex::perf::CounterId::kGpuDrawUs,    rex::perf::CounterId::kGpuTransferUs,
      rex::perf::CounterId::kGpuResolveUs, rex::perf::CounterId::kGpuTextureLoadUs,
      rex::perf::CounterId::kGpuUploadUs,  rex::perf::CounterId::kGpuSwapUs,
      rex::perf::CounterId::kGpuOtherUs};
  uint64_t total_ticks = 0;
  for (size_t i = 0; i < size_t(GpuSection::kCount); ++i) {
    if (ticks[i]) {
      total_ticks += ticks[i];
      rex::perf::IncrementCounter(kCounters[i], int64_t(double(ticks[i]) * ns_per_tick / 1000.0));
    }
  }
  if (total_ticks) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kGpuTimeUs,
                                int64_t(double(total_ticks) * ns_per_tick / 1000.0));
  }
#endif
}

void D3D11CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  uint64_t eviction_frame_floor = frame_current_ > kReadbackBufferEvictionAgeFrames
                                      ? frame_current_ - kReadbackBufferEvictionAgeFrames
                                      : 0;
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    if (buffer_map.size() <= kMaxReadbackBuffers &&
        it->second.last_used_frame >= eviction_frame_floor) {
      ++it;
      continue;
    }
    for (ReadbackSlot& slot : it->second.slots) {
      if (slot.mapping) {
        context->Unmap(slot.buffer.Get(), 0);
      }
    }
    it = buffer_map.erase(it);
  }
}

void D3D11CommandProcessor::ReadBackMemexportFast(
    const std::vector<draw_util::MemExportRange>& ranges) {
  uint32_t total_size = 0;
  for (const draw_util::MemExportRange& range : ranges) {
    total_size += range.size_bytes;
  }
  if (!total_size) {
    return;
  }
  ReadbackBuffer& readback =
      memexport_readback_buffers_[(uint64_t(ranges.front().base_address_dwords) << 32) |
                                  total_size];
  readback.last_used_frame = frame_current_;
  uint32_t write_index = readback.current_index;
  uint32_t read_index = 1 - write_index;
  ReadbackSlot& write_slot = readback.slots[write_index];
  if (!PrepareReadbackSlotForCopy(write_slot, total_size)) {
    return;
  }
  uint32_t offset = 0;
  for (const draw_util::MemExportRange& memexport_range : ranges) {
    ReadbackRange range;
    range.address = memexport_range.base_address_dwords << 2;
    range.length = memexport_range.size_bytes;
    CopyReadbackRange(range, write_slot.buffer.Get(), offset);
    offset += memexport_range.size_bytes;
  }
  write_slot.written_size = total_size;

  // The previous copy if the GPU is done with it, else wait for this one.
  ReadbackSlot& read_slot = readback.slots[read_index];
  const uint8_t* data = nullptr;
  if (read_slot.written_size >= total_size) {
    data = MapReadbackSlot(read_slot, false);
  }
  if (!data) {
    data = MapReadbackSlot(write_slot, true);
  }
  if (data) {
    for (const draw_util::MemExportRange& memexport_range : ranges) {
      if (void* guest = memory_->TranslatePhysical(memexport_range.base_address_dwords << 2)) {
        std::memcpy(guest, data, memexport_range.size_bytes);
      }
      data += memexport_range.size_bytes;
    }
  }
  readback.current_index = read_index;
}

bool D3D11CommandProcessor::CopyReadbackRange(
    const ReadbackRange& range, ID3D11Buffer* target, uint32_t target_offset,
    const std::vector<std::pair<uint32_t, uint32_t>>* pieces) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  uint32_t address = range.address & (SharedMemory::kBufferSize - 1);
  uint32_t length = std::min(range.length, SharedMemory::kBufferSize - address);
  if (range.scaled) {
    if (!DownscaleResolveForReadback(range)) {
      return false;
    }
    D3D11_BOX box = {0, 0, 0, length, 1, 1};
    context->CopySubresourceRegion(target, 0, target_offset, 0, 0,
                                   resolve_downscale_buffer_.Get(), 0, &box);
    return true;
  }
  ID3D11Buffer* source = shared_memory_->GetBuffer();
  if (pieces) {
    // Only the pieces the guest reads, each where the whole copy would put it.
    for (const auto& [piece_offset, piece_length] : *pieces) {
      if (piece_offset >= length) {
        continue;
      }
      uint32_t piece_end = std::min(piece_offset + piece_length, length);
      D3D11_BOX box = {address + piece_offset, 0, 0, address + piece_end, 1, 1};
      context->CopySubresourceRegion(target, 0, target_offset + piece_offset, 0, 0, source, 0,
                                     &box);
    }
  } else {
    D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
    context->CopySubresourceRegion(target, 0, target_offset, 0, 0, source, 0, &box);
  }
  return true;
}

bool D3D11CommandProcessor::DownscaleResolveForReadback(const ReadbackRange& range) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  uint32_t tile_count = range.length / ((32 * 32) << range.pixel_size_log2);
  if (!tile_count || !texture_cache_->MakeScaledResolveRangeCurrent(range.address, range.length)) {
    return false;
  }
  if (!resolve_downscale_cs_) {
    if (FAILED(device->CreateComputeShader(shaders::resolve_downscale_cs,
                                           sizeof(shaders::resolve_downscale_cs), nullptr,
                                           &resolve_downscale_cs_))) {
      REXGPU_ERROR("Failed to create the resolve downscale shader");
      return false;
    }
    D3D11_BUFFER_DESC constants_desc = {};
    constants_desc.ByteWidth = UINT(rex::align(sizeof(ResolveDownscaleConstants), size_t(16)));
    constants_desc.Usage = D3D11_USAGE_DEFAULT;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &resolve_downscale_constants_))) {
      REXGPU_ERROR("Failed to create the resolve downscale constant buffer");
      resolve_downscale_cs_.Reset();
      return false;
    }
  }
  uint32_t size = rex::align(range.length, uint32_t(16));
  if (resolve_downscale_buffer_size_ < size) {
    resolve_downscale_uav_.Reset();
    resolve_downscale_buffer_.Reset();
    resolve_downscale_buffer_size_ = 0;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = std::max(uint32_t(1) << 20, rex::next_pow2(size));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = desc.ByteWidth >> 2;
    uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &resolve_downscale_buffer_)) ||
        FAILED(device->CreateUnorderedAccessView(resolve_downscale_buffer_.Get(), &uav_desc,
                                                 &resolve_downscale_uav_))) {
      REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer", desc.ByteWidth >> 20);
      resolve_downscale_uav_.Reset();
      resolve_downscale_buffer_.Reset();
      return false;
    }
    resolve_downscale_buffer_size_ = desc.ByteWidth;
  }
  ResolveDownscaleConstants constants;
  constants.scale_x = texture_cache_->draw_resolution_scale_x();
  constants.scale_y = texture_cache_->draw_resolution_scale_y();
  constants.pixel_size_log2 = range.pixel_size_log2;
  constants.tile_count = tile_count;
  // The whole buffer is bound, unlike in D3D12.
  constants.source_offset_bytes = texture_cache_->GetCurrentScaledResolveRangeOffset();
  constants.half_pixel_offset = uint32_t(REXCVAR_GET(readback_resolve_half_pixel_offset));
  context->UpdateSubresource(resolve_downscale_constants_.Get(), 0, nullptr, &constants, 0, 0);
  ID3D11Buffer* constant_buffer = resolve_downscale_constants_.Get();
  context->CSSetConstantBuffers(0, 1, &constant_buffer);
  ID3D11ShaderResourceView* source = texture_cache_->GetCurrentScaledResolveBufferSRV();
  context->CSSetShaderResources(0, 1, &source);
  ID3D11UnorderedAccessView* dest = resolve_downscale_uav_.Get();
  context->CSSetUnorderedAccessViews(0, 1, &dest, nullptr);
  context->CSSetShader(resolve_downscale_cs_.Get(), nullptr, 0);
  // One thread group per 32x32 tile.
  context->Dispatch(tile_count, 1, 1);
  ID3D11ShaderResourceView* null_srv = nullptr;
  ID3D11UnorderedAccessView* null_uav = nullptr;
  context->CSSetShaderResources(0, 1, &null_srv);
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  InvalidateDrawState();
  return true;
}

void D3D11CommandProcessor::ReadBackSharedMemory(const ReadbackRange* ranges, size_t count,
                                                 std::vector<const uint8_t*>& data_out) {
  data_out.assign(count, nullptr);
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  if (readback_buffer_mapped_) {
    context->Unmap(readback_buffer_.Get(), 0);
    readback_buffer_mapped_ = false;
  }
  uint64_t total_size = 0;
  for (size_t i = 0; i < count; ++i) {
    total_size += rex::align(uint64_t(ranges[i].length), uint64_t(16));
  }
  if (!total_size || total_size > SharedMemory::kBufferSize) {
    return;
  }
  if (readback_buffer_size_ < total_size) {
    readback_buffer_.Reset();
    readback_buffer_size_ = 0;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = std::max(uint32_t(1) << 20, rex::next_pow2(uint32_t(total_size)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(provider.GetDevice()->CreateBuffer(&desc, nullptr, &readback_buffer_))) {
      REXGPU_ERROR("Failed to create a {} byte resolve readback buffer", desc.ByteWidth);
      return;
    }
    readback_buffer_size_ = desc.ByteWidth;
  }
  uint32_t offset = 0;
  std::vector<bool> range_copied(count, false);
  for (size_t i = 0; i < count; ++i) {
    range_copied[i] = CopyReadbackRange(ranges[i], readback_buffer_.Get(), offset);
    offset += uint32_t(rex::align(uint64_t(ranges[i].length), uint64_t(16)));
  }
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(context->Map(readback_buffer_.Get(), 0, D3D11_MAP_READ, 0, &mapping))) {
    REXGPU_ERROR("Failed to map the resolve readback buffer");
    return;
  }
  readback_buffer_mapped_ = true;
  offset = 0;
  for (size_t i = 0; i < count; ++i) {
    if (range_copied[i]) {
      data_out[i] = static_cast<const uint8_t*>(mapping.pData) + offset;
    }
    offset += uint32_t(rex::align(uint64_t(ranges[i].length), uint64_t(16)));
  }
}

void D3D11CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, uint32_t bound_depth_and_color_render_target_bits) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  const RegisterFile& regs = *register_file_;

  // Viewport.
  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  if (std::memcmp(&current_viewport_, &viewport, sizeof(viewport))) {
    current_viewport_ = viewport;
    context->RSSetViewports(1, &viewport);
  }

  // Scissor.
  D3D11_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  if (std::memcmp(&current_scissor_, &scissor_rect, sizeof(scissor_rect))) {
    current_scissor_ = scissor_rect;
    context->RSSetScissorRects(1, &scissor_rect);
  }

  // Blend factor. Direct3D 11 has only a four-component constant-color factor:
  // if the draw needs only the scalar constant-alpha factor, emulate it by
  // replicating A (as D3D12 without the alpha blend factor).
  float blend_factor[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  bool color_uses_constant_color = false;
  bool color_uses_constant_alpha = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i))) ||
        !((normalized_color_mask >> (i * 4)) & 0b0111)) {
      continue;
    }
    auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    if (blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
        blend_control.color_comb_fcn == xenos::BlendOp::kMax) {
      continue;
    }
    for (xenos::BlendFactor factor :
         {blend_control.color_srcblend, blend_control.color_destblend}) {
      switch (factor) {
        case xenos::BlendFactor::kConstantColor:
        case xenos::BlendFactor::kOneMinusConstantColor:
          color_uses_constant_color = true;
          break;
        case xenos::BlendFactor::kConstantAlpha:
        case xenos::BlendFactor::kOneMinusConstantAlpha:
          color_uses_constant_alpha = true;
          break;
        default:
          break;
      }
    }
  }
  if (color_uses_constant_alpha && !color_uses_constant_color) {
    blend_factor[0] = blend_factor[3];
    blend_factor[1] = blend_factor[3];
    blend_factor[2] = blend_factor[3];
  }

  // Stencil reference value. Per-face reference is not supported by Direct3D
  // 11, choose the back face one only if drawing only back faces.
  Register stencil_ref_mask_reg;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal && normalized_depth_control.backface_enable &&
      pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
  } else {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
  }
  uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;

  // Set with the pipeline's state objects. std::memcmp instead of != so in
  // case of NaN, every draw won't be invalidating it.
  if (std::memcmp(current_blend_factor_, blend_factor, sizeof(blend_factor)) ||
      current_stencil_ref_ != stencil_ref) {
    std::memcpy(current_blend_factor_, blend_factor, sizeof(blend_factor));
    current_stencil_ref_ = stencil_ref;
    current_pipeline_ = nullptr;
  }
}

bool D3D11CommandProcessor::UpdateConstantBuffer(ID3D11Buffer* buffer, const void* data,
                                                 size_t size) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping))) {
    REXGPU_ERROR("Failed to map a constant buffer");
    return false;
  }
  std::memcpy(mapping.pData, data, size);
  context->Unmap(buffer, 0);
  return true;
}

bool D3D11CommandProcessor::UpdateBindings(const D3D11Shader* vertex_shader,
                                           const D3D11Shader* pixel_shader, bool zpd_counter_used,
                                           bool shared_memory_is_uav) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  const RegisterFile& regs = *register_file_;

  // Invalidate the float constants if the used ones changed.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      cbuffer_binding_float_vertex_.up_to_date = false;
    }
  }
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Upload the changed constants.
  if (!cbuffer_binding_system_.up_to_date) {
    if (!UpdateConstantBuffer(constant_buffer_system_.Get(), &system_constants_,
                              sizeof(system_constants_))) {
      return false;
    }
    cbuffer_binding_system_.up_to_date = true;
  }
  float_constant_staging_.resize(kFloatConstantBufferSize);
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    uint8_t* end = CopyFloatConstantRuns(float_constant_staging_.data(),
                                         &regs[XE_GPU_REG_SHADER_CONSTANT_000_X],
                                         current_float_constant_map_vertex_);
    if (!UpdateConstantBuffer(constant_buffer_float_vertex_.Get(), float_constant_staging_.data(),
                              size_t(end - float_constant_staging_.data()))) {
      return false;
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* end = CopyFloatConstantRuns(float_constant_staging_.data(),
                                         &regs[XE_GPU_REG_SHADER_CONSTANT_256_X],
                                         current_float_constant_map_pixel_);
    if (!UpdateConstantBuffer(constant_buffer_float_pixel_.Get(), float_constant_staging_.data(),
                              size_t(end - float_constant_staging_.data()))) {
      return false;
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    if (!UpdateConstantBuffer(constant_buffer_bool_loop_.Get(),
                              &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                              kBoolLoopConstantBufferSize)) {
      return false;
    }
    cbuffer_binding_bool_loop_.up_to_date = true;
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    if (!UpdateConstantBuffer(constant_buffer_fetch_.Get(),
                              &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                              kFetchConstantBufferSize)) {
      return false;
    }
    cbuffer_binding_fetch_.up_to_date = true;
  }

  if (!draw_state_valid_ || current_shared_memory_is_uav_ != shared_memory_is_uav) {
    // The registers of DxbcShaderTranslator::CbufferRegister.
    ID3D11Buffer* vertex_constant_buffers[] = {
        constant_buffer_system_.Get(), constant_buffer_float_vertex_.Get(),
        constant_buffer_bool_loop_.Get(), constant_buffer_fetch_.Get()};
    ID3D11Buffer* pixel_constant_buffers[] = {
        constant_buffer_system_.Get(), constant_buffer_float_pixel_.Get(),
        constant_buffer_bool_loop_.Get(), constant_buffer_fetch_.Get()};
    // With tessellation, the guest vertex shader is the domain shader, and the
    // built-in vertex and hull shaders read the system constants.
    context->VSSetConstantBuffers(0, UINT(rex::countof(vertex_constant_buffers)),
                                  vertex_constant_buffers);
    context->HSSetConstantBuffers(0, 1, vertex_constant_buffers);
    context->DSSetConstantBuffers(0, UINT(rex::countof(vertex_constant_buffers)),
                                  vertex_constant_buffers);
    context->GSSetConstantBuffers(0, 1, vertex_constant_buffers);
    context->PSSetConstantBuffers(0, UINT(rex::countof(pixel_constant_buffers)),
                                  pixel_constant_buffers);

    // The shared memory: t0 when read only, u0 (offset past the render
    // targets) when written, which also requires it not to be bound as an
    // input.
    ID3D11ShaderResourceView* shared_memory_srv =
        shared_memory_is_uav ? nullptr : shared_memory_->GetRawSRV();
    if (!shared_memory_is_uav) {
      ID3D11UnorderedAccessView* null_uav = nullptr;
      context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                                                         nullptr, nullptr,
                                                         xenos::kMaxColorRenderTargets, 1,
                                                         &null_uav, nullptr);
    }
    context->VSSetShaderResources(0, 1, &shared_memory_srv);
    context->DSSetShaderResources(0, 1, &shared_memory_srv);
    context->PSSetShaderResources(0, 1, &shared_memory_srv);
    current_shared_memory_is_uav_ = shared_memory_is_uav;
  }
  if (shared_memory_is_uav || zpd_counter_used) {
    // Render target updates may unbind them. The registers of
    // DxbcShaderTranslator::UAVRegister after the render targets.
    ID3D11UnorderedAccessView* uavs[] = {
        shared_memory_is_uav ? shared_memory_->GetRawUAV() : nullptr, nullptr,
        zpd_counter_used ? zpd_counter_uav_.Get() : nullptr};
    static_assert(uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory) == 0 &&
                  uint32_t(DxbcShaderTranslator::UAVRegister::kZpdCounter) == 2);
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
        xenos::kMaxColorRenderTargets, zpd_counter_used ? 3 : 1, uavs, nullptr);
  }
  return true;
}

void D3D11CommandProcessor::GetStageTextureBindings(const D3D11Shader& shader,
                                                    StageTextureBindings& bindings_out) {
  bindings_out.srvs.clear();
  bindings_out.swizzles.clear();
  bindings_out.samplers.clear();
  const std::vector<DxbcShader::TextureBinding>& texture_bindings =
      shader.GetTextureBindingsAfterTranslation();
  // Over the limit, the conversion to Shader Model 5.0 fails anyway.
  size_t texture_count =
      std::min(texture_bindings.size(),
               size_t(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) -
                   size_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart));
  for (size_t i = 0; i < texture_count; ++i) {
    uint32_t host_swizzle;
    ID3D11ShaderResourceView* srv =
        texture_cache_->GetActiveTextureSRV(texture_bindings[i], host_swizzle);
    bindings_out.srvs.push_back(srv);
    ++(srv ? frame_stats_.textures_bound : frame_stats_.textures_missing);
    // A null view reads zero in every component anyway.
    if (srv && host_swizzle != kIdentityTextureSwizzle) {
      bindings_out.swizzles.push_back(
          {uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart) + uint32_t(i),
           host_swizzle});
    }
  }
  const std::vector<DxbcShader::SamplerBinding>& sampler_bindings =
      shader.GetSamplerBindingsAfterTranslation();
  size_t sampler_count =
      std::min(sampler_bindings.size(), size_t(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT));
  for (size_t i = 0; i < sampler_count; ++i) {
    bindings_out.samplers.push_back(texture_cache_->GetOrCreateSampler(
        texture_cache_->GetSamplerParameters(sampler_bindings[i])));
  }
}

void D3D11CommandProcessor::BindStageTextures(bool pixel_stage,
                                              const StageTextureBindings& bindings) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  StageTextureBindings& current =
      pixel_stage ? current_pixel_texture_bindings_ : current_vertex_texture_bindings_;
  // Views and samplers of a previous draw in higher slots stay bound, unused.
  if (!bindings.srvs.empty() && current.srvs != bindings.srvs) {
    UINT start_slot = UINT(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    if (pixel_stage) {
      context->PSSetShaderResources(start_slot, UINT(bindings.srvs.size()), bindings.srvs.data());
    } else {
      // Also for the guest vertex shader as the domain shader.
      context->VSSetShaderResources(start_slot, UINT(bindings.srvs.size()), bindings.srvs.data());
      context->DSSetShaderResources(start_slot, UINT(bindings.srvs.size()), bindings.srvs.data());
    }
    current.srvs = bindings.srvs;
  }
  if (!bindings.samplers.empty() && current.samplers != bindings.samplers) {
    if (pixel_stage) {
      context->PSSetSamplers(0, UINT(bindings.samplers.size()), bindings.samplers.data());
    } else {
      context->VSSetSamplers(0, UINT(bindings.samplers.size()), bindings.samplers.data());
      context->DSSetSamplers(0, UINT(bindings.samplers.size()), bindings.samplers.data());
    }
    current.samplers = bindings.samplers;
  }
}

ID3D11Buffer* D3D11CommandProcessor::CopyIndicesToScratchBuffer(uint32_t offset, uint32_t size) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  if (scratch_index_buffer_size_ < size) {
    scratch_index_buffer_.Reset();
    scratch_index_buffer_size_ = 0;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = std::max(uint32_t(1) << 20, rex::next_pow2(size));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(provider.GetDevice()->CreateBuffer(&desc, nullptr, &scratch_index_buffer_))) {
      REXGPU_ERROR("Failed to create a {} byte scratch index buffer", desc.ByteWidth);
      return nullptr;
    }
    scratch_index_buffer_size_ = desc.ByteWidth;
  }
  D3D11_BOX box = {};
  box.left = offset;
  box.right = offset + size;
  box.bottom = 1;
  box.back = 1;
  provider.GetImmediateContext()->CopySubresourceRegion(
      scratch_index_buffer_.Get(), 0, 0, 0, 0, shared_memory_->GetBuffer(), 0, &box);
  return scratch_index_buffer_.Get();
}

bool D3D11CommandProcessor::InitializeDebugWhiteTextures() {
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  // Six faces for the cube, one texel for the others.
  const uint32_t white[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                             UINT32_MAX, UINT32_MAX, UINT32_MAX};
  D3D11_SUBRESOURCE_DATA initial_data[6];
  for (uint32_t i = 0; i < 6; ++i) {
    initial_data[i].pSysMem = &white[i];
    initial_data[i].SysMemPitch = sizeof(uint32_t);
    initial_data[i].SysMemSlicePitch = sizeof(uint32_t);
  }

  D3D11_TEXTURE2D_DESC desc_2d = {};
  desc_2d.Width = 1;
  desc_2d.Height = 1;
  desc_2d.MipLevels = 1;
  desc_2d.ArraySize = 1;
  desc_2d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc_2d.SampleDesc.Count = 1;
  desc_2d.Usage = D3D11_USAGE_IMMUTABLE;
  desc_2d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_TEXTURE3D_DESC desc_3d = {};
  desc_3d.Width = 1;
  desc_3d.Height = 1;
  desc_3d.Depth = 1;
  desc_3d.MipLevels = 1;
  desc_3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc_3d.Usage = D3D11_USAGE_IMMUTABLE;
  desc_3d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_2d_array, texture_cube;
  Microsoft::WRL::ComPtr<ID3D11Texture3D> texture_3d;
  bool created = SUCCEEDED(device->CreateTexture2D(&desc_2d, initial_data, &texture_2d_array));
  desc_2d.ArraySize = 6;
  desc_2d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  created = created && SUCCEEDED(device->CreateTexture2D(&desc_2d, initial_data, &texture_cube));
  created = created && SUCCEEDED(device->CreateTexture3D(&desc_3d, initial_data, &texture_3d));

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  srv_desc.Texture2DArray.MipLevels = 1;
  srv_desc.Texture2DArray.ArraySize = 1;
  created = created && SUCCEEDED(device->CreateShaderResourceView(
                           texture_2d_array.Get(), &srv_desc, &debug_white_srvs_[0]));
  srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  srv_desc.Texture3D.MipLevels = 1;
  created = created && SUCCEEDED(device->CreateShaderResourceView(texture_3d.Get(), &srv_desc,
                                                                  &debug_white_srvs_[1]));
  srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
  srv_desc.TextureCube.MipLevels = 1;
  created = created && SUCCEEDED(device->CreateShaderResourceView(texture_cube.Get(), &srv_desc,
                                                                  &debug_white_srvs_[2]));

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  created = created && SUCCEEDED(device->CreateSamplerState(&sampler_desc, &debug_white_sampler_));
  if (!created) {
    REXGPU_ERROR("Failed to create the debug white textures");
    for (auto& srv : debug_white_srvs_) {
      srv.Reset();
    }
    debug_white_sampler_.Reset();
    return false;
  }
  return true;
}

void D3D11CommandProcessor::BindDebugWhiteTextures(const D3D11Shader* vertex_shader,
                                                   const D3D11Shader* pixel_shader) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  // Bindful, the translator places texture binding i at
  // t[SRVMainRegister::kBindfulTexturesStart + i], kept by the conversion to
  // Shader Model 5.0; 1D textures are 2D arrays there.
  const D3D11Shader* shaders[] = {vertex_shader, pixel_shader};
  for (size_t i = 0; i < rex::countof(shaders); ++i) {
    if (!shaders[i]) {
      continue;
    }
    const std::vector<DxbcShader::TextureBinding>& bindings =
        shaders[i]->GetTextureBindingsAfterTranslation();
    for (uint32_t j = 0; j < uint32_t(bindings.size()); ++j) {
      uint32_t slot = uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart) + j;
      if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        break;
      }
      ID3D11ShaderResourceView* srv;
      switch (bindings[j].dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          srv = debug_white_srvs_[1].Get();
          break;
        case xenos::FetchOpDimension::kCube:
          srv = debug_white_srvs_[2].Get();
          break;
        default:
          srv = debug_white_srvs_[0].Get();
          break;
      }
      if (i) {
        context->PSSetShaderResources(slot, 1, &srv);
      } else {
        context->VSSetShaderResources(slot, 1, &srv);
        context->DSSetShaderResources(slot, 1, &srv);
      }
    }
  }
  ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
  std::fill(std::begin(samplers), std::end(samplers), debug_white_sampler_.Get());
  context->VSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
  context->DSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
  context->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
}

void D3D11CommandProcessor::OnPrimaryBufferEnd() {
  // Pick up completed queries now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();
}

void D3D11CommandProcessor::EnsureQueryResources() {
  // VIZ surveys need the queries even when ZPD reports are faked.
  if ((zpd_mode_ == ZPDMode::kFake && !REXCVAR_GET(occlusion_query_viz)) ||
      !occlusion_queries_.empty() || occlusion_queries_failed_) {
    return;
  }
  // Query objects are cheap, but there's no need to create all of them
  // before they're used: a first batch here, more in OpenQuery.
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  D3D11_QUERY_DESC query_desc = {D3D11_QUERY_OCCLUSION, 0};
  for (uint32_t i = 0; i < 256; ++i) {
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&query_desc, &query))) {
      REXGPU_ERROR("Direct3D 11: Failed to create occlusion queries, using fake results");
      occlusion_queries_.clear();
      free_occlusion_queries_.clear();
      occlusion_queries_failed_ = true;
      return;
    }
    free_occlusion_queries_.push_back(uint32_t(occlusion_queries_.size()));
    occlusion_queries_.push_back(std::move(query));
  }
  if (zpd_hybrid_supported_ && !zpd_counter_buffer_) {
    // A slot per query, zeroed when its query begins.
    D3D11_BUFFER_DESC counter_desc = {};
    counter_desc.ByteWidth = kQueryPoolCapacity * XenosZPDReport::kCounterSizeBytes;
    counter_desc.Usage = D3D11_USAGE_DEFAULT;
    counter_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    counter_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC counter_uav_desc = {};
    counter_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    counter_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    counter_uav_desc.Buffer.NumElements = counter_desc.ByteWidth / sizeof(uint32_t);
    counter_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateBuffer(&counter_desc, nullptr, &zpd_counter_buffer_)) ||
        FAILED(device->CreateUnorderedAccessView(zpd_counter_buffer_.Get(), &counter_uav_desc,
                                                 &zpd_counter_uav_))) {
      REXGPU_ERROR(
          "Direct3D 11: Failed to create the occlusion query counter buffer, counting passed "
          "samples only");
      zpd_counter_uav_.Reset();
      zpd_counter_buffer_.Reset();
    } else {
      zpd_counter_readback_.resize(kQueryPoolCapacity);
    }
  }
}

void D3D11CommandProcessor::ShutdownQueryResources() {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  if (!active_query_interrupted_ &&
      (active_viz_predicate_ != UINT32_MAX || active_query_index_ != UINT32_MAX)) {
    if (ID3D11DeviceContext* context = provider.GetImmediateContext()) {
      if (active_viz_predicate_ != UINT32_MAX) {
        context->End(viz_predicates_[active_viz_predicate_].Get());
      }
      if (active_query_index_ != UINT32_MAX) {
        context->End(occlusion_queries_[active_query_index_].Get());
      }
    }
  }
  active_viz_predicate_ = UINT32_MAX;
  active_query_index_ = UINT32_MAX;
  active_query_interrupted_ = false;
  query_resolves_in_flight_.clear();
  free_occlusion_queries_.clear();
  occlusion_queries_.clear();
  active_query_hybrid_ = false;
  zpd_counter_readback_.clear();
  zpd_counter_uav_.Reset();
  zpd_counter_buffer_.Reset();
  for (Microsoft::WRL::ComPtr<ID3D11Predicate>& predicate : viz_predicates_) {
    predicate.Reset();
  }
}

CommandProcessor::QueryOpenResult D3D11CommandProcessor::OpenQuery(bool can_close_submission) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  if (free_occlusion_queries_.empty()) {
    PumpQueryResolves();
  }
  if (free_occlusion_queries_.empty() && occlusion_queries_.size() < kQueryPoolCapacity) {
    D3D11_QUERY_DESC query_desc = {D3D11_QUERY_OCCLUSION, 0};
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if (SUCCEEDED(provider.GetDevice()->CreateQuery(&query_desc, &query))) {
      free_occlusion_queries_.push_back(uint32_t(occlusion_queries_.size()));
      occlusion_queries_.push_back(std::move(query));
    }
  }
  if (free_occlusion_queries_.empty()) {
    if (zpd_mode_ != ZPDMode::kStrict) {
      return QueryOpenResult::kPoolExhausted;
    }
    // Strict mode can't guess, so wait for the oldest query.
    if (!TakeQueryResolve(true)) {
      return QueryOpenResult::kDeferred;
    }
  }
  active_query_index_ = free_occlusion_queries_.back();
  free_occlusion_queries_.pop_back();
  active_query_is_rov_ = false;
  active_query_interrupted_ = false;
  active_query_hybrid_ = active_segment_.count_total && zpd_counter_uav_;
  active_segment_.hybrid = active_query_hybrid_;

  ID3D11DeviceContext* context = provider.GetImmediateContext();
  if (active_query_hybrid_) {
    // Zero the slot so a recycled index never inherits old counts.
    static const uint32_t kZeroCounts[XenosZPDReport::kCount] = {};
    uint32_t slot_offset = active_query_index_ * XenosZPDReport::kCounterSizeBytes;
    D3D11_BOX slot_box = {slot_offset, 0, 0, slot_offset + XenosZPDReport::kCounterSizeBytes, 1,
                          1};
    context->UpdateSubresource(zpd_counter_buffer_.Get(), 0, &slot_box, kZeroCounts, 0, 0);
  }
  // The predicate stands for the survey only if it covers all of it, so only
  // for the first segment of the ID's generation.
  const VIZQueryHandle& viz = active_segment_.viz;
  if (viz.generation != kInvalidVIZGeneration && CanArmVIZPredicate(viz.id, viz.generation)) {
    Microsoft::WRL::ComPtr<ID3D11Predicate>& predicate = viz_predicates_[viz.id];
    if (!predicate) {
      D3D11_QUERY_DESC predicate_desc = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
      provider.GetDevice()->CreatePredicate(&predicate_desc, &predicate);
    }
    if (predicate) {
      context->Begin(predicate.Get());
      active_viz_predicate_ = viz.id;
    }
  }
  context->Begin(occlusion_queries_[active_query_index_].Get());
  return QueryOpenResult::kOpened;
}

bool D3D11CommandProcessor::CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                                       uint64_t& out_submission) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(provider.context_mutex());
  if (active_query_index_ == UINT32_MAX) {
    return false;
  }
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  // If interrupted, the query and the predicate were ended already, after the
  // segment's last draw.
  bool ended = active_query_interrupted_;
  active_query_interrupted_ = false;
  if (!ended) {
    context->End(occlusion_queries_[active_query_index_].Get());
  }
  if (viz.generation != kInvalidVIZGeneration) {
    if (active_viz_predicate_ == viz.id && CanArmVIZPredicate(viz.id, viz.generation)) {
      if (!ended) {
        context->End(viz_predicates_[viz.id].Get());
      }
      ArmVIZPredicate(viz.id, viz.generation);
    } else {
      BlockVIZPredicate(viz.id, viz.generation);
    }
  }
  if (!ended && active_viz_predicate_ != UINT32_MAX && active_viz_predicate_ != viz.id) {
    // Not expected, but a begun predicate must be ended.
    context->End(viz_predicates_[active_viz_predicate_].Get());
  }
  active_viz_predicate_ = UINT32_MAX;

  PendingQueryResolve resolve;
  resolve.hybrid = false;
  if (active_query_hybrid_) {
    Microsoft::WRL::ComPtr<ID3D11Buffer>& readback = zpd_counter_readback_[active_query_index_];
    if (!readback) {
      D3D11_BUFFER_DESC readback_desc = {};
      readback_desc.ByteWidth = XenosZPDReport::kCounterSizeBytes;
      readback_desc.Usage = D3D11_USAGE_STAGING;
      readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      provider.GetDevice()->CreateBuffer(&readback_desc, nullptr, &readback);
    }
    if (readback) {
      uint32_t slot_offset = active_query_index_ * XenosZPDReport::kCounterSizeBytes;
      D3D11_BOX slot_box = {slot_offset, 0, 0, slot_offset + XenosZPDReport::kCounterSizeBytes,
                            1, 1};
      context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, zpd_counter_buffer_.Get(), 0,
                                     &slot_box);
      resolve.hybrid = true;
    }
    active_query_hybrid_ = false;
  }
  resolve.number = ++query_resolves_closed_;
  resolve.query_index = active_query_index_;
  resolve.scale_area = GetZPDScaleArea();
  resolve.report_handle = report_handle;
  resolve.viz = viz;
  query_resolves_in_flight_.push_back(resolve);
  out_submission = resolve.number;
  active_query_index_ = UINT32_MAX;
  return true;
}

void D3D11CommandProcessor::OnContextOwnerChange() {
  // On the thread taking the context lock, with the lock held.
  // GPU time: another thread's work is swap work.
  if (std::this_thread::get_id() != command_processor_thread_id_) {
    if (!gpu_section_other_thread_) {
      gpu_section_interrupted_ = SetGpuSection(GpuSection::kSwap);
      gpu_section_other_thread_ = true;
    }
  } else if (gpu_section_other_thread_) {
    gpu_section_other_thread_ = false;
    SetGpuSection(gpu_section_interrupted_);
  }
  if (active_query_index_ == UINT32_MAX || active_query_interrupted_) {
    return;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  context->End(occlusion_queries_[active_query_index_].Get());
  if (active_viz_predicate_ != UINT32_MAX) {
    context->End(viz_predicates_[active_viz_predicate_].Get());
  }
  active_query_interrupted_ = true;
}

bool D3D11CommandProcessor::TakeQueryResolve(bool wait) {
  if (query_resolves_in_flight_.empty()) {
    return false;
  }
  const PendingQueryResolve& resolve = query_resolves_in_flight_.front();
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  ID3D11Query* query = occlusion_queries_[resolve.query_index].Get();
  UINT64 samples_passed = 0;
  HRESULT result;
  while ((result = context->GetData(query, &samples_passed, sizeof(samples_passed),
                                    wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
    if (!wait) {
      return false;
    }
    std::this_thread::yield();
  }
  if (FAILED(result)) {
    // Device removed: whatever was drawn stays visible.
    samples_passed = 1;
  }
  XenosZPDReport raw_counts = XenosZPDReport::FromNativeQuery(samples_passed);
  if (resolve.hybrid && SUCCEEDED(result)) {
    ID3D11Buffer* readback = zpd_counter_readback_[resolve.query_index].Get();
    D3D11_MAPPED_SUBRESOURCE mapping;
    HRESULT map_result =
        context->Map(readback, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapping);
    if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {
      return false;
    }
    if (SUCCEEDED(map_result)) {
      raw_counts = XenosZPDReport::FromNativeQueryAndTotal(
          samples_passed, static_cast<const uint32_t*>(mapping.pData)[XenosZPDReport::kTotal]);
      context->Unmap(readback, 0);
    }
  }
  PendingQueryResolve taken = resolve;
  query_resolves_in_flight_.pop_front();
  free_occlusion_queries_.push_back(taken.query_index);
  if (taken.report_handle != kInvalidReportHandle) {
    OnZPDQueryResolved(taken.report_handle, raw_counts, taken.scale_area);
  }
  if (taken.viz.generation != kInvalidVIZGeneration) {
    OnVIZQueryResolved(taken.viz.id, taken.viz.generation, raw_counts.z_pass != 0);
  }
  return true;
}

void D3D11CommandProcessor::PumpQueryResolves() {
  if (query_resolves_in_flight_.empty()) {
    return;
  }
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
  while (TakeQueryResolve(false)) {
  }
}

bool D3D11CommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                              uint64_t wait_for_submission) {
  {
    std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
    while (!query_resolves_in_flight_.empty() &&
           query_resolves_in_flight_.front().number <= wait_for_submission) {
      TakeQueryResolve(true);
    }
  }
  const ZPDReport* report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

void D3D11CommandProcessor::AwaitVIZQueryResolve(uint64_t wait_for_submission) {
  std::lock_guard<ui::d3d11::ContextMutex> context_lock(GetD3D11Provider().context_mutex());
  while (!query_resolves_in_flight_.empty() &&
         query_resolves_in_flight_.front().number <= wait_for_submission) {
    TakeQueryResolve(true);
  }
}

bool D3D11CommandProcessor::InitializeSwap() {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();
  static_assert(sizeof(reg::DC_LUT_30_COLOR) == sizeof(uint32_t));
  static_assert(sizeof(reg::DC_LUT_PWL_DATA) == sizeof(uint32_t));
  D3D11_BUFFER_DESC buffer_desc = {};
  buffer_desc.ByteWidth = (256 + 128 * 3) * sizeof(uint32_t);
  buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  buffer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SHADER_RESOURCE_VIEW_DESC table_desc = {};
  table_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  table_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  table_desc.Buffer.FirstElement = 0;
  table_desc.Buffer.NumElements = 256;
  D3D11_SHADER_RESOURCE_VIEW_DESC pwl_desc = {};
  pwl_desc.Format = DXGI_FORMAT_R16G16_UINT;
  pwl_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  pwl_desc.Buffer.FirstElement = 256;
  pwl_desc.Buffer.NumElements = 128 * 3;
  if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &gamma_ramp_buffer_)) ||
      FAILED(device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &table_desc,
                                              &gamma_ramp_table_srv_)) ||
      FAILED(device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &pwl_desc,
                                              &gamma_ramp_pwl_srv_)) ||
      FAILED(device->CreateVertexShader(shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs),
                                        nullptr, &swap_vs_))) {
    REXGPU_ERROR("Failed to create the swap objects");
    return false;
  }

  D3D11_BUFFER_DESC post_effect_constants_desc = {};
  post_effect_constants_desc.ByteWidth = 16;
  post_effect_constants_desc.Usage = D3D11_USAGE_DEFAULT;
  post_effect_constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  // SMAA requires linear filtering and clamping for all its samplers.
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateBuffer(&post_effect_constants_desc, nullptr,
                                  &post_effect_constants_)) ||
      FAILED(device->CreateSamplerState(&sampler_desc, &post_effect_sampler_)) ||
      FAILED(device->CreateComputeShader(shaders::fxaa_cs, sizeof(shaders::fxaa_cs), nullptr,
                                         &fxaa_cs_)) ||
      FAILED(device->CreateComputeShader(shaders::fxaa_extreme_cs,
                                         sizeof(shaders::fxaa_extreme_cs), nullptr,
                                         &fxaa_extreme_cs_)) ||
      FAILED(device->CreateComputeShader(shaders::smaa_edge_detection_cs,
                                         sizeof(shaders::smaa_edge_detection_cs), nullptr,
                                         &smaa_cs_[kSmaaPassEdgeDetection])) ||
      FAILED(device->CreateComputeShader(shaders::smaa_blending_weight_calculation_cs,
                                         sizeof(shaders::smaa_blending_weight_calculation_cs),
                                         nullptr, &smaa_cs_[kSmaaPassBlendingWeightCalculation])) ||
      FAILED(device->CreateComputeShader(shaders::smaa_neighborhood_blending_cs,
                                         sizeof(shaders::smaa_neighborhood_blending_cs), nullptr,
                                         &smaa_cs_[kSmaaPassNeighborhoodBlending]))) {
    REXGPU_ERROR("Failed to create the swap post effect objects");
    return false;
  }
  return true;
}

bool D3D11CommandProcessor::EnsurePostEffectTexture(PostEffectTexture& texture,
                                                    DXGI_FORMAT format, uint32_t width,
                                                    uint32_t height) {
  if (texture.texture) {
    D3D11_TEXTURE2D_DESC desc;
    texture.texture->GetDesc(&desc);
    if (desc.Format == format && desc.Width == width && desc.Height == height) {
      return true;
    }
    // Direct3D 11 keeps it alive while the GPU still uses it.
    texture = PostEffectTexture();
  }
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture.texture)) ||
      FAILED(device->CreateShaderResourceView(texture.texture.Get(), nullptr, &texture.srv)) ||
      FAILED(device->CreateUnorderedAccessView(texture.texture.Get(), nullptr, &texture.uav)) ||
      FAILED(device->CreateRenderTargetView(texture.texture.Get(), nullptr, &texture.rtv))) {
    REXGPU_ERROR("Failed to create a {}x{} swap post effect texture (DXGI format {})", width,
                 height, uint32_t(format));
    texture = PostEffectTexture();
    return false;
  }
  return true;
}

bool D3D11CommandProcessor::EnsureSmaaResources(uint32_t width, uint32_t height) {
  for (size_t i = 0; i < kSmaaTextureCount; ++i) {
    if (!EnsurePostEffectTexture(smaa_textures_[i], kSmaaTextureFormats[i], width, height)) {
      return false;
    }
  }
  if (smaa_area_srv_ && smaa_search_srv_) {
    return true;
  }
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  struct LookupTexture {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv;
    DXGI_FORMAT format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    const uint8_t* data;
  };
  const LookupTexture lookup_textures[] = {
      {&smaa_area_srv_, DXGI_FORMAT_R8G8_UNORM, smaa::kAreaTextureWidth, smaa::kAreaTextureHeight,
       smaa::kAreaTexturePitch, smaa::GetAreaTextureData()},
      {&smaa_search_srv_, DXGI_FORMAT_R8_UNORM, smaa::kSearchTextureWidth,
       smaa::kSearchTextureHeight, smaa::kSearchTexturePitch, smaa::GetSearchTextureData()},
  };
  for (const LookupTexture& lookup_texture : lookup_textures) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = lookup_texture.width;
    desc.Height = lookup_texture.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = lookup_texture.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {lookup_texture.data, lookup_texture.pitch};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr,
                                                lookup_texture.srv->ReleaseAndGetAddressOf()))) {
      REXGPU_ERROR("Failed to create the SMAA lookup textures");
      smaa_area_srv_.Reset();
      smaa_search_srv_.Reset();
      return false;
    }
  }
  return true;
}

ID3D11PixelShader* D3D11CommandProcessor::GetSwapPixelShader(bool pwl, bool fxaa_luma,
                                                             uint32_t host_swizzle) {
  uint64_t key = (uint64_t(fxaa_luma) << 33) | (uint64_t(pwl) << 32) | host_swizzle;
  auto it = swap_pixel_shaders_.find(key);
  if (it != swap_pixel_shaders_.end()) {
    return it->second.Get();
  }
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ShaderBytecodeOptions options = PipelineCache::GetShaderBytecodeOptions(provider, false);
  if (host_swizzle != kIdentityTextureSwizzle) {
    // The front buffer is at t1 (the gamma ramp at t0).
    options.texture_swizzles.push_back({1, host_swizzle});
  }
  std::span<const uint8_t> source;
  if (pwl) {
    source = fxaa_luma ? std::span<const uint8_t>(shaders_sm51::apply_gamma_pwl_fxaa_luma_ps,
                                                  sizeof(shaders_sm51::apply_gamma_pwl_fxaa_luma_ps))
                       : std::span<const uint8_t>(shaders_sm51::apply_gamma_pwl_ps,
                                                  sizeof(shaders_sm51::apply_gamma_pwl_ps));
  } else {
    source = fxaa_luma
                 ? std::span<const uint8_t>(shaders_sm51::apply_gamma_table_fxaa_luma_ps,
                                            sizeof(shaders_sm51::apply_gamma_table_fxaa_luma_ps))
                 : std::span<const uint8_t>(shaders_sm51::apply_gamma_table_ps,
                                            sizeof(shaders_sm51::apply_gamma_table_ps));
  }
  ShaderBytecode bytecode;
  std::string error;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
  if (!ConvertShaderBytecode(source, options, bytecode, error)) {
    REXGPU_ERROR("Direct3D 11: Failed to convert the swap gamma shader (swizzle {:03o}): {}",
                 host_swizzle, error);
  } else if (FAILED(provider.GetDevice()->CreatePixelShader(
                 bytecode.data.data(), bytecode.data.size(), nullptr, &pixel_shader))) {
    REXGPU_ERROR("Direct3D 11: Failed to create the swap gamma shader");
  }
  return swap_pixel_shaders_.emplace(key, std::move(pixel_shader)).first->second.Get();
}

bool D3D11CommandProcessor::DrawSwapTexture(ID3D11RenderTargetView* rtv,
                                            ID3D11UnorderedAccessView* uav, uint32_t width,
                                            uint32_t height, ID3D11ShaderResourceView* source,
                                            uint32_t host_swizzle, bool pwl,
                                            bool& post_effect_out) {
  post_effect_out = false;
  SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
  if (swap_post_effect != SwapPostEffect::kNone && !uav) {
    if (!post_effect_no_uav_logged_) {
      post_effect_no_uav_logged_ = true;
      REXGPU_WARN(
          "Direct3D 11: swap_post_effect is ignored, the guest output can't be written through "
          "an unordered access view on this device");
    }
    swap_post_effect = SwapPostEffect::kNone;
  }
  bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                  swap_post_effect == SwapPostEffect::kFxaaExtreme;
  bool use_smaa = swap_post_effect == SwapPostEffect::kSmaa;
  if (use_fxaa &&
      !EnsurePostEffectTexture(fxaa_source_texture_, kFxaaSourceTextureFormat, width, height)) {
    use_fxaa = false;
  }
  if (use_smaa && !EnsureSmaaResources(width, height)) {
    use_smaa = false;
  }
  ID3D11PixelShader* pixel_shader = GetSwapPixelShader(pwl, use_fxaa, host_swizzle);
  if (!pixel_shader) {
    return false;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  // The gamma ramp is applied into the source of the post effect if any.
  if (use_fxaa) {
    rtv = fxaa_source_texture_.rtv.Get();
  } else if (use_smaa) {
    rtv = smaa_textures_[kSmaaTextureColor].rtv.Get();
  }

  // Small enough to upload whole every frame.
  uint32_t gamma_ramp[256 + 128 * 3];
  std::memcpy(gamma_ramp, gamma_ramp_256_entry_table(), 256 * sizeof(uint32_t));
  std::memcpy(gamma_ramp + 256, gamma_ramp_pwl_rgb(), 128 * 3 * sizeof(uint32_t));
  context->UpdateSubresource(gamma_ramp_buffer_.Get(), 0, nullptr, gamma_ramp, 0, 0);

  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
  context->OMSetDepthStencilState(nullptr, 0);
  D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  context->RSSetViewports(1, &viewport);
  context->RSSetState(temporary_output_rasterizer_state_.Get());
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(swap_vs_.Get(), nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->PSSetShader(pixel_shader, nullptr, 0);
  ID3D11ShaderResourceView* srvs[] = {
      pwl ? gamma_ramp_pwl_srv_.Get() : gamma_ramp_table_srv_.Get(), source};
  context->PSSetShaderResources(0, UINT(rex::countof(srvs)), srvs);
  context->Draw(3, 0);
  ID3D11ShaderResourceView* null_srvs[] = {nullptr, nullptr, nullptr};
  context->PSSetShaderResources(0, 2, null_srvs);
  // The post effect reads the target, and the guest output is written as a
  // UAV.
  context->OMSetRenderTargets(0, nullptr, nullptr);

  if (use_fxaa || use_smaa) {
    uint32_t constants[4];
    constants[0] = width;
    constants[1] = height;
    float size_inv[2] = {1.0f / float(width), 1.0f / float(height)};
    std::memcpy(constants + 2, size_inv, sizeof(size_inv));
    context->UpdateSubresource(post_effect_constants_.Get(), 0, nullptr, constants, 0, 0);
    ID3D11Buffer* post_effect_constants = post_effect_constants_.Get();
    context->CSSetConstantBuffers(0, 1, &post_effect_constants);
    ID3D11SamplerState* post_effect_sampler = post_effect_sampler_.Get();
    context->CSSetSamplers(0, 1, &post_effect_sampler);
    uint32_t group_count_x = (width + 15) / 16;
    uint32_t group_count_y = (height + 7) / 8;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    if (use_fxaa) {
      context->CSSetShader(swap_post_effect == SwapPostEffect::kFxaaExtreme
                               ? fxaa_extreme_cs_.Get()
                               : fxaa_cs_.Get(),
                           nullptr, 0);
      ID3D11ShaderResourceView* fxaa_source = fxaa_source_texture_.srv.Get();
      context->CSSetShaderResources(0, 1, &fxaa_source);
      context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      context->Dispatch(group_count_x, group_count_y, 1);
    } else {
      ID3D11ShaderResourceView* smaa_color = smaa_textures_[kSmaaTextureColor].srv.Get();
      ID3D11ShaderResourceView* smaa_edges = smaa_textures_[kSmaaTextureEdges].srv.Get();
      ID3D11ShaderResourceView* smaa_weights = smaa_textures_[kSmaaTextureWeights].srv.Get();
      struct SmaaPassViews {
        ID3D11ShaderResourceView* sources[3];
        ID3D11UnorderedAccessView* dest;
      };
      const SmaaPassViews smaa_passes[kSmaaPassCount] = {
          {{smaa_color, nullptr, nullptr}, smaa_textures_[kSmaaTextureEdges].uav.Get()},
          {{smaa_edges, smaa_area_srv_.Get(), smaa_search_srv_.Get()},
           smaa_textures_[kSmaaTextureWeights].uav.Get()},
          {{smaa_color, smaa_weights, nullptr}, uav},
      };
      for (size_t i = 0; i < kSmaaPassCount; ++i) {
        const SmaaPassViews& smaa_pass = smaa_passes[i];
        // The previous pass's destination is unbound before it's a source.
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        context->CSSetShaderResources(0, 3, smaa_pass.sources);
        context->CSSetUnorderedAccessViews(0, 1, &smaa_pass.dest, nullptr);
        context->CSSetShader(smaa_cs_[i].Get(), nullptr, 0);
        context->Dispatch(group_count_x, group_count_y, 1);
      }
    }
    context->CSSetShaderResources(0, 3, null_srvs);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ID3D11Buffer* null_buffer = nullptr;
    context->CSSetConstantBuffers(0, 1, &null_buffer);
    ID3D11SamplerState* null_sampler = nullptr;
    context->CSSetSamplers(0, 1, &null_sampler);
    context->CSSetShader(nullptr, nullptr, 0);
    post_effect_out = true;
  }

  InvalidateDrawState();
  render_target_cache_->InvalidateBoundRenderTargets();
  return true;
}

bool D3D11CommandProcessor::InitializeTemporaryGuestOutput() {
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = 16;
  constants_desc.Usage = D3D11_USAGE_DEFAULT;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  if (FAILED(device->CreateVertexShader(shaders::guest_output_triangle_strip_rect_vs,
                                        sizeof(shaders::guest_output_triangle_strip_rect_vs),
                                        nullptr, &temporary_output_vs_)) ||
      FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_ps,
                                       sizeof(shaders::guest_output_bilinear_ps), nullptr,
                                       &temporary_output_ps_)) ||
      FAILED(device->CreateBuffer(&constants_desc, nullptr, &temporary_output_vs_constants_)) ||
      FAILED(device->CreateBuffer(&constants_desc, nullptr, &temporary_output_ps_constants_)) ||
      FAILED(device->CreateSamplerState(&sampler_desc, &temporary_output_sampler_)) ||
      FAILED(device->CreateRasterizerState(&rasterizer_desc,
                                           &temporary_output_rasterizer_state_))) {
    REXGPU_ERROR("Failed to create the guest output objects");
    return false;
  }
  return true;
}

void D3D11CommandProcessor::DrawTemporaryGuestOutput(ID3D11RenderTargetView* rtv, uint32_t width,
                                                     uint32_t height) {
  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  const float black[] = {0.0f, 0.0f, 0.0f, 1.0f};

  DXGI_FORMAT source_format;
  uint32_t source_width, source_height;
  ID3D11Texture2D* source = render_target_cache_
                                ? render_target_cache_->GetTemporaryGuestOutput(
                                      source_format, source_width, source_height)
                                : nullptr;
  if (!source) {
    context->ClearRenderTargetView(rtv, black);
    return;
  }

  // A single-sampled copy of the render target in its drawing format.
  D3D11_TEXTURE2D_DESC source_desc;
  source->GetDesc(&source_desc);
  D3D11_TEXTURE2D_DESC copy_desc = {};
  if (temporary_output_resolved_) {
    temporary_output_resolved_->GetDesc(&copy_desc);
  }
  if (!temporary_output_resolved_ || copy_desc.Width != source_desc.Width ||
      copy_desc.Height != source_desc.Height || copy_desc.Format != source_format) {
    temporary_output_srv_.Reset();
    temporary_output_resolved_.Reset();
    copy_desc = {};
    copy_desc.Width = source_desc.Width;
    copy_desc.Height = source_desc.Height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = source_format;
    copy_desc.SampleDesc.Count = 1;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(provider.GetDevice()->CreateTexture2D(&copy_desc, nullptr,
                                                     &temporary_output_resolved_)) ||
        FAILED(provider.GetDevice()->CreateShaderResourceView(
            temporary_output_resolved_.Get(), nullptr, &temporary_output_srv_))) {
      temporary_output_srv_.Reset();
      temporary_output_resolved_.Reset();
      context->ClearRenderTargetView(rtv, black);
      return;
    }
  }
  if (source_desc.SampleDesc.Count > 1) {
    context->ResolveSubresource(temporary_output_resolved_.Get(), 0, source, 0, source_format);
  } else {
    context->CopyResource(temporary_output_resolved_.Get(), source);
  }

  // Stretched to the guest output, like the presenter's bilinear effect: the
  // rectangle in NDC for guest_output_triangle_strip_rect_vs, the output
  // offset and inverse size for guest_output_bilinear_ps, each at its b0.
  struct {
    float offset[2];
    float size[2];
  } vs_constants = {{-1.0f, 1.0f}, {2.0f, -2.0f}};
  struct {
    int32_t output_offset[2];
    float output_size_inv[2];
  } ps_constants = {{0, 0},
                    {1.0f / float(std::max(width, uint32_t(1))),
                     1.0f / float(std::max(height, uint32_t(1)))}};
  context->UpdateSubresource(temporary_output_vs_constants_.Get(), 0, nullptr, &vs_constants, 0,
                             0);
  context->UpdateSubresource(temporary_output_ps_constants_.Get(), 0, nullptr, &ps_constants, 0,
                             0);
  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
  context->OMSetDepthStencilState(nullptr, 0);
  D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  context->RSSetViewports(1, &viewport);
  context->RSSetState(temporary_output_rasterizer_state_.Get());
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ID3D11Buffer* constant_buffer = temporary_output_vs_constants_.Get();
  context->VSSetConstantBuffers(0, 1, &constant_buffer);
  context->VSSetShader(temporary_output_vs_.Get(), nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  constant_buffer = temporary_output_ps_constants_.Get();
  context->PSSetConstantBuffers(0, 1, &constant_buffer);
  ID3D11ShaderResourceView* srv = temporary_output_srv_.Get();
  context->PSSetShaderResources(0, 1, &srv);
  ID3D11SamplerState* sampler = temporary_output_sampler_.Get();
  context->PSSetSamplers(0, 1, &sampler);
  context->PSSetShader(temporary_output_ps_.Get(), nullptr, 0);
  context->Draw(4, 0);
  ID3D11ShaderResourceView* null_srv = nullptr;
  context->PSSetShaderResources(0, 1, &null_srv);
  ID3D11SamplerState* null_sampler = nullptr;
  context->PSSetSamplers(0, 1, &null_sampler);

  InvalidateDrawState();
  render_target_cache_->InvalidateBoundRenderTargets();
}

}  // namespace rex::graphics::d3d11
