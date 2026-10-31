/**
 * @file        graphics/d3d11/command_processor.h
 * @brief       Command processor of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/d3d11/primitive_processor.h>
#include <rex/graphics/d3d11/render_target_cache.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/util/draw.h>
// GpuSection and AddGpuSectionTicks: header-only, not tied to Vulkan.
#include <rex/graphics/vulkan/gpu_time.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11GraphicsSystem;

// Stages 4 to 7 of the D3D11 plan: guest draws are rendered into host render
// targets with ownership transfers and resolves, and with textures and
// occlusion queries (without tessellation). Without a front buffer texture,
// the largest render target of the frame is shown on swap.
class D3D11CommandProcessor : public CommandProcessor {
 public:
  D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~D3D11CommandProcessor() override;

  void ClearCaches() override;
  void InvalidateGpuMemory() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  // The pipeline storage of the D3D11 pipeline cache (B4).
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking, std::function<void()> completion_callback) override;

  const ui::d3d11::D3D11Provider& GetD3D11Provider() const;
  D3D11SharedMemory& shared_memory() const { return *shared_memory_; }

  // After the immediate context state used by draws (shaders, fixed-function
  // state, bindings) was changed outside the draw path.
  void InvalidateDrawState();
  // Hybrid occlusion queries (occlusion_query_full_counters), known before the
  // pipeline cache is initialized.
  bool IsZpdHybridSupported() const { return zpd_hybrid_supported_; }

  // The graphics card's time by kind of work (d3d11_gpu_time, perf counter
  // builds), as Vulkan's (0354): the work after a section change, up to the
  // next one, is that section's. Draws are the default; the other kinds are
  // set around the work itself. Under the context mutex.
  using GpuSection = vulkan::GpuSection;
  GpuSection SetGpuSection(GpuSection section);
  class ScopedGpuSection {
   public:
    ScopedGpuSection(D3D11CommandProcessor& command_processor, GpuSection section)
        : command_processor_(command_processor),
          previous_(command_processor.SetGpuSection(section)) {}
    ~ScopedGpuSection() { command_processor_.SetGpuSection(previous_); }
    ScopedGpuSection(const ScopedGpuSection&) = delete;
    ScopedGpuSection& operator=(const ScopedGpuSection&) = delete;

   private:
    D3D11CommandProcessor& command_processor_;
    GpuSection previous_;
  };

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

  // readback_resolve_cpu_reads_only: the destinations guest threads wait for.
  bool CanReadBackResolvesNow() const override { return true; }
  void ReadBackResolvesNow(const ResolveFetch* fetches, size_t count,
                           std::vector<const uint8_t*>& data_out) override;
  // readback_resolve_full_deferred: mapping the slot a resolve's copy went to
  // waits for that copy only.
  bool CanDeferResolveReadbacks() const override { return true; }
  void CompleteDeferredReadbacks(const ResolveFetch* fetches, size_t count,
                                 std::vector<const uint8_t*>& data_out) override;

  void OnPrimaryBufferEnd() override;

  // Direct3D 11 has no submissions or fences, and needs none: it keeps the
  // resources the GPU uses alive, readback buffers wait for their own copies
  // when mapped, and queries have their own data. The "submissions" are the
  // closed query segments (see the occlusion queries below): completed are
  // those up to the first whose result hasn't been taken. Polling takes the
  // results that have arrived, as D3D12's PollCompletedSubmission.
  void PollCompletedSubmission() override { PumpQueryResolves(); }
  uint64_t GetCompletedSubmission() const override {
    return query_resolves_in_flight_.empty() ? query_resolves_closed_
                                             : query_resolves_in_flight_.front().number - 1;
  }

  // Occlusion queries (ZPD reports and VIZ surveys) as D3D12's native query
  // path: one D3D11_QUERY_OCCLUSION per segment, with the hybrid counter
  // (zpd_hybrid_supported_), without ROV. Direct3D 11 has no submissions, so the "submission" of a closed
  // segment is its number in close order, and waiting for it means waiting
  // for the query's data. A VIZ survey segment also runs the ID's
  // occlusion predicate, used for the ID's draws (SetPredication) while the
  // survey's answer isn't known, if it's the survey's only segment.
  void EnsureQueryResources() override;
  void ShutdownQueryResources() override;
  bool IsQueryPoolReady() const override { return !occlusion_queries_.empty(); }
  QueryOpenResult OpenQuery(bool can_close_submission) override;
  bool CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                  uint64_t& out_submission) override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle, uint64_t wait_for_submission) override;
  void AwaitVIZQueryResolve(uint64_t wait_for_submission) override;

 private:
  // Texture views (from t1), their channel mappings for the shader (only the
  // non-identity ones) and samplers (from s0) of a shader stage.
  struct StageTextureBindings {
    std::vector<ID3D11ShaderResourceView*> srvs;
    std::vector<TextureSwizzle> swizzles;
    std::vector<ID3D11SamplerState*> samplers;
  };

  struct ConstantBufferBinding {
    bool up_to_date = false;
  };

  // Initializes the render target cache (created the first time, shut down
  // otherwise) and creates the texture cache for the scale; on failure, the
  // render target cache is shut down and there's no texture cache.
  bool CreateDrawResolutionScaleDependentCaches(uint32_t draw_resolution_scale_x,
                                                uint32_t draw_resolution_scale_y);
  // At the end of a frame, under the context mutex: applies a
  // RequestDrawResolutionScale request (resolution_scale changed while the
  // game runs), as D3D12CommandProcessor::ApplyDrawResolutionScaleRequest.
  void ApplyDrawResolutionScaleRequest();
  // Takes the context lock for the draw path; invalidates the cached state if
  // another thread used the context since.
  void BeginContextUse();
  void UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                const draw_util::Scissor& scissor, bool primitive_polygonal,
                                reg::RB_DEPTHCONTROL normalized_depth_control,
                                uint32_t normalized_color_mask,
                                uint32_t bound_depth_and_color_render_target_bits);
  // A generated copy of the D3D12 function (command_processor_copies.cpp).
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  uint32_t line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask,
                                  const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset,
                                  int32_t window_offset_tiles);
  bool UpdateBindings(const D3D11Shader* vertex_shader, const D3D11Shader* pixel_shader,
                      bool zpd_counter_used, bool shared_memory_is_uav);
  // For the textures of the latest RequestTextures.
  void GetStageTextureBindings(const D3D11Shader& shader, StageTextureBindings& bindings_out);
  void BindStageTextures(bool pixel_stage, const StageTextureBindings& bindings);
  bool UpdateConstantBuffer(ID3D11Buffer* buffer, const void* data, size_t size);
  // A copy of guest DMA indices for a draw that binds the shared memory as a
  // UAV, which Direct3D 11 can't use as an index buffer at the same time.
  ID3D11Buffer* CopyIndicesToScratchBuffer(uint32_t offset, uint32_t size);
  // d3d11_debug_white_textures: white 1x1 textures (2D array, 3D, cube) for
  // the guest texture bindings until stage 6, bound with the pipeline.
  bool InitializeDebugWhiteTextures();
  void BindDebugWhiteTextures(const D3D11Shader* vertex_shader, const D3D11Shader* pixel_shader);
  struct ReadbackRange {
    uint32_t address;  // Physical.
    uint32_t length;
    // A resolution-scaled resolve, downscaled from the scaled resolve memory
    // (length in whole 32x32 tiles of the pixel size).
    bool scaled = false;
    uint32_t pixel_size_log2 = 0;
  };
  // Copies the shared memory ranges to the readback buffer and maps it,
  // waiting for the GPU: data_out gets each one's bytes, valid until the next
  // call (nullptr where that failed). Under the context mutex. For memexport
  // without readback_memexport_fast.
  void ReadBackSharedMemory(const ReadbackRange* ranges, size_t count,
                            std::vector<const uint8_t*>& data_out);
  // Copies a range (downscaled if scaled) into a staging buffer at an offset:
  // all of it, or only `pieces` (offset, length within the range), each at its
  // own offset (unscaled only). False if nothing was copied.
  bool CopyReadbackRange(const ReadbackRange& range, ID3D11Buffer* target, uint32_t target_offset,
                         const std::vector<std::pair<uint32_t, uint32_t>>* pieces = nullptr);

  // Double-buffered staging buffers of resolve destinations (by
  // GetReadbackResolveKey) and memexport ranges, as D3D12's ReadbackBuffer.
  // Direct3D 11 can't keep a staging buffer mapped while the GPU copies into
  // it, so a slot is mapped when read and unmapped before its next copy;
  // mapping waits only for the copies into that buffer.
  struct ReadbackSlot {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    uint32_t size = 0;
    const uint8_t* mapping = nullptr;
    // What the last copy into it holds: bytes, and whether only pieces.
    uint32_t written_size = 0;
    bool partial = false;
  };
  struct ReadbackBuffer {
    ReadbackSlot slots[2];
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  // Unmapped, with at least `size` bytes (rounded up as on D3D12).
  bool PrepareReadbackSlotForCopy(ReadbackSlot& slot, uint32_t size);
  // Null on failure, or, without wait, if the copy isn't done yet.
  const uint8_t* MapReadbackSlot(ReadbackSlot& slot, bool wait);
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  // readback_resolve with a mode other than disabled, after the resolve, as
  // D3D12CommandProcessor::IssueCopy_ReadbackResolvePath.
  void ReadBackResolve(uint32_t written_address, uint32_t written_length,
                       reg::RB_COPY_DEST_INFO copy_dest_info, bool written_scaled);
  // readback_memexport_fast, as D3D12's IssueDraw_MemexportReadbackFastPath:
  // the guest gets the previous copy of the same ranges if it's done, else
  // this one, waited for.
  void ReadBackMemexportFast(const std::vector<draw_util::MemExportRange>& ranges);
  // Downscales a scaled resolve to the start of the downscale buffer with
  // resolve_downscale_cs, as D3D12CommandProcessor::RecordResolveReadbackCopy.
  bool DownscaleResolveForReadback(const ReadbackRange& range);
  bool InitializeTemporaryGuestOutput();
  bool InitializeSwap();
  // The guest's gamma ramp applied to the front buffer, as the D3D12 swap, by
  // apply_gamma_table_ps / apply_gamma_pwl_ps (their _fxaa_luma variants for
  // FXAA), into the guest output or, with swap_post_effect, an intermediate
  // texture the FXAA or SMAA compute shaders read to write the guest output
  // through its UAV (without one, no post effect). post_effect_out is whether
  // FXAA or SMAA was applied.
  bool DrawSwapTexture(ID3D11RenderTargetView* rtv, ID3D11UnorderedAccessView* uav,
                       uint32_t width, uint32_t height, ID3D11ShaderResourceView* source,
                       uint32_t host_swizzle, bool pwl, bool& post_effect_out);
  // The gamma pixel shader converted with the front buffer's host swizzle.
  ID3D11PixelShader* GetSwapPixelShader(bool pwl, bool fxaa_luma, uint32_t host_swizzle);
  // A 2D texture with shader resource, unordered access and render target
  // views for the swap post effects.
  struct PostEffectTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  };
  // (Re)creates the texture if it doesn't have the format and the size.
  bool EnsurePostEffectTexture(PostEffectTexture& texture, DXGI_FORMAT format, uint32_t width,
                               uint32_t height);
  // The intermediate textures for the size and the lookup textures, as
  // D3D12CommandProcessor::EnsureSmaaResources.
  bool EnsureSmaaResources(uint32_t width, uint32_t height);
  void DrawTemporaryGuestOutput(ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height);

  std::unique_ptr<D3D11SharedMemory> shared_memory_;
  std::unique_ptr<D3D11PrimitiveProcessor> primitive_processor_;
  // Names as in the D3D12 command processor, for the generated copies.
  std::unique_ptr<D3D11RenderTargetCache> render_target_cache_;
  std::unique_ptr<D3D11TextureCache> texture_cache_;
  std::unique_ptr<PipelineCache> pipeline_cache_;

  uint64_t frame_current_ = 1;

  // Constants.
  DxbcShaderTranslator::SystemConstants system_constants_;
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  ConstantBufferBinding cbuffer_binding_fetch_;
  // Float constants used by the bound shaders, for invalidation on writes.
  uint64_t current_float_constant_map_vertex_[4] = {};
  uint64_t current_float_constant_map_pixel_[4] = {};
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_system_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_float_vertex_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_float_pixel_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_bool_loop_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_fetch_;
  // Reused for packing the used float constants.
  std::vector<uint8_t> float_constant_staging_;

  // Occlusion queries. active_query_index_ and active_query_is_rov_ (always
  // false) are also read by the copied UpdateSystemConstantValues.
  uint32_t active_query_index_ = UINT32_MAX;
  bool active_query_is_rov_ = false;
  // The other threads' draws on the immediate context (the presenter, the UI)
  // must not be counted: when another thread takes the context lock, the
  // active query and predicate are ended (OnContextOwnerChange), and the next
  // draw closes the segment and opens a new one.
  bool active_query_interrupted_ = false;
  void OnContextOwnerChange();
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> occlusion_queries_;
  std::vector<uint32_t> free_occlusion_queries_;
  bool occlusion_queries_failed_ = false;
  // By VIZ ID, created on first use.
  std::array<Microsoft::WRL::ComPtr<ID3D11Predicate>, 64> viz_predicates_;
  // The ID whose predicate the active segment runs, UINT32_MAX if none.
  uint32_t active_viz_predicate_ = UINT32_MAX;
  // Hybrid occlusion queries (occlusion_query_full_counters), as D3D12's on
  // the host render target path: the native query counts passed samples, and
  // tested draws without depth writes also count their coverage (Total) with
  // pixel shader atomics into the query's slot of the counter buffer (u2 in
  // the translator, after the render targets and the shared memory on the
  // host). Each slot is copied into its own small staging buffer at the end of
  // the query, so reading it waits for that copy only.
  bool zpd_hybrid_supported_ = false;
  bool active_query_hybrid_ = false;
  Microsoft::WRL::ComPtr<ID3D11Buffer> zpd_counter_buffer_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> zpd_counter_uav_;
  // By query index, created on first use.
  std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> zpd_counter_readback_;
  struct PendingQueryResolve {
    uint64_t number;
    uint32_t query_index;
    uint32_t scale_area;
    // Total is in the counter slot's staging buffer.
    bool hybrid;
    ReportHandle report_handle;
    VIZQueryHandle viz;
  };
  std::deque<PendingQueryResolve> query_resolves_in_flight_;
  uint64_t query_resolves_closed_ = 0;
  // Takes the oldest resolve in flight if its data is ready (or, if wait, once
  // it is); false if none was taken.
  bool TakeQueryResolve(bool wait);

  // Cached immediate context state of the draw path.
  uint64_t context_epoch_ = 0;
  bool draw_state_valid_ = false;
  const PipelineCache::Pipeline* current_pipeline_ = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY current_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D11_VIEWPORT current_viewport_ = {};
  D3D11_RECT current_scissor_ = {};
  float current_blend_factor_[4] = {};
  uint32_t current_stencil_ref_ = 0;
  bool current_shared_memory_is_uav_ = false;
  // Of the current draw, and what's bound (cleared with the draw state).
  StageTextureBindings vertex_texture_bindings_;
  StageTextureBindings pixel_texture_bindings_;
  StageTextureBindings current_vertex_texture_bindings_;
  StageTextureBindings current_pixel_texture_bindings_;

  // Staging buffer of ReadBackSharedMemory, mapped until its next use.
  Microsoft::WRL::ComPtr<ID3D11Buffer> readback_buffer_;
  uint32_t readback_buffer_size_ = 0;
  bool readback_buffer_mapped_ = false;
  std::vector<ReadbackRange> readback_ranges_;
  // The memexport ranges before the draw, without readback_memexport_fast.
  std::vector<uint8_t> memexport_snapshot_;
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;
  // readback_resolve_full_deferred: where each destination's last deferred
  // copy went, by (physical address << 32 | length).
  struct DeferredReadbackSlot {
    uint64_t buffer_key;
    uint32_t index;
  };
  std::unordered_map<uint64_t, DeferredReadbackSlot> deferred_readback_slots_;
  // The pieces of the current full resolve readback.
  std::vector<std::pair<uint32_t, uint32_t>> readback_pieces_;
  // Created on the first readback of a scaled resolve. The constants of
  // resolve_downscale_cs, as D3D12's.
  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t source_offset_bytes;
    uint32_t half_pixel_offset;
  };
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> resolve_downscale_cs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> resolve_downscale_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> resolve_downscale_buffer_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resolve_downscale_uav_;
  uint32_t resolve_downscale_buffer_size_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Buffer> scratch_index_buffer_;
  uint32_t scratch_index_buffer_size_ = 0;

  // 2D array, 3D, cube; empty without d3d11_debug_white_textures.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> debug_white_srvs_[3];
  Microsoft::WRL::ComPtr<ID3D11SamplerState> debug_white_sampler_;

  // Swap: the 256-entry table (R10G10B10A2) and then the PWL ramp (R16G16).
  Microsoft::WRL::ComPtr<ID3D11Buffer> gamma_ramp_buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_ramp_table_srv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_ramp_pwl_srv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> swap_vs_;
  // By FXAA luma << 33 | PWL << 32 | host swizzle; null for a conversion that
  // failed.
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11PixelShader>> swap_pixel_shaders_;
  bool swap_texture_failure_logged_ = false;
  bool swap_unscaled_logged_ = false;

  // swap_post_effect, as in D3D12 (formats and passes there). The compute
  // shaders take the size and its inverse at b0 and a linear clamp sampler at
  // s0.
  Microsoft::WRL::ComPtr<ID3D11Buffer> post_effect_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> post_effect_sampler_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> fxaa_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> fxaa_extreme_cs_;
  static constexpr DXGI_FORMAT kFxaaSourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  PostEffectTexture fxaa_source_texture_;
  enum SmaaPass : size_t {
    kSmaaPassEdgeDetection,
    kSmaaPassBlendingWeightCalculation,
    kSmaaPassNeighborhoodBlending,

    kSmaaPassCount,
  };
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> smaa_cs_[kSmaaPassCount];
  enum SmaaTexture : size_t {
    kSmaaTextureColor,
    kSmaaTextureEdges,
    kSmaaTextureWeights,

    kSmaaTextureCount,
  };
  static constexpr DXGI_FORMAT kSmaaTextureFormats[kSmaaTextureCount] = {
      DXGI_FORMAT_R10G10B10A2_UNORM,
      DXGI_FORMAT_R10G10B10A2_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT,
  };
  PostEffectTexture smaa_textures_[kSmaaTextureCount];
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> smaa_area_srv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> smaa_search_srv_;
  bool post_effect_no_uav_logged_ = false;

  // Stand-in for the front buffer when it's unavailable.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> temporary_output_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> temporary_output_ps_;
  // Separate for the vertex and the pixel shader: both read theirs at b0.
  Microsoft::WRL::ComPtr<ID3D11Buffer> temporary_output_vs_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> temporary_output_ps_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> temporary_output_sampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> temporary_output_rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> temporary_output_resolved_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> temporary_output_srv_;

  // d3d11_log_frames: what happened in the current frame.
  struct FrameStats {
    uint32_t draws_issued = 0;
    uint32_t draws_submitted = 0;
    uint32_t draws_failed_pipeline = 0;
    uint32_t draws_pending_pipeline = 0;
    uint32_t copies = 0;
    uint32_t copies_written = 0;
    uint32_t textures_bound = 0;
    uint32_t textures_missing = 0;
  };
  FrameStats frame_stats_;
  uint32_t frames_logged_ = 0;

  bool pipeline_failure_logged_ = false;

  // Device loss (as D3D12's BeginSubmission check): checked at each swap and
  // when a readback map fails; logged once with the reason, reported to the
  // graphics system, and nothing is drawn after it.
  bool CheckDeviceRemoved();
  bool device_removed_ = false;

  // GPU time: a TIMESTAMP_DISJOINT query per frame (swap to swap), a
  // timestamp at its start and end and at each section change, read when the
  // frame's data is there. Work of other threads on the context (the
  // presenter, the UI) is counted as swap work.
  void InitializeGpuTime();
  void ShutdownGpuTime();
  bool AllocateGpuTimeQuery(uint32_t& index_out);
  void BeginGpuTimeFrame();
  void EndGpuTimeFrame();
  void ReadGpuTimes();
  static constexpr uint32_t kGpuTimeMaxQueries = 4096;
  static constexpr uint32_t kGpuTimeMaxMarksPerFrame = 512;
  static constexpr size_t kGpuTimeMaxFramesInFlight = 8;
  struct GpuTimeMark {
    uint32_t query;
    GpuSection ended;
  };
  struct GpuTimeFrame {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
    uint32_t start_query = 0;
    uint32_t end_query = 0;
    GpuSection last = GpuSection::kDraws;
    std::vector<GpuTimeMark> marks;
  };
  void FreeGpuTimeFrame(GpuTimeFrame& frame);
  bool gpu_time_enabled_ = false;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> gpu_time_queries_;
  std::vector<uint32_t> gpu_time_free_queries_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> gpu_time_free_disjoint_;
  bool gpu_time_recording_ = false;
  GpuTimeFrame gpu_time_current_;
  std::deque<GpuTimeFrame> gpu_time_submitted_;
  std::vector<vulkan::GpuSectionMark> gpu_time_marks_;  // reused by ReadGpuTimes
  GpuSection gpu_section_ = GpuSection::kDraws;
  // Another thread holds the context: its work is counted as swap work, then
  // the section it interrupted goes on.
  bool gpu_section_other_thread_ = false;
  GpuSection gpu_section_interrupted_ = GpuSection::kDraws;
  std::thread::id command_processor_thread_id_;
};

}  // namespace rex::graphics::d3d11
