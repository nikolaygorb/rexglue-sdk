/**
 * @file        graphics/native/command_processor.h
 * @brief       Native renderer: command processor
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/bindings.h>
#include <rex/graphics/native/composite.h>
#include <rex/graphics/native/draw_state.h>
#include <rex/graphics/native/memexport.h>
#include <rex/graphics/native/pipeline_storage.h>
#include <rex/graphics/native/pipelines.h>
#include <rex/graphics/native/present.h>
#include <rex/graphics/native/primitive_processor.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/resolve_readback.h>
#include <rex/graphics/native/shaders.h>
#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/native/texture_cache.h>
#include <rex/graphics/null_command_processor.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/vulkan/query_pool.h>

namespace rex::graphics::native {

// rexgpu-native's command processor (memory/28-native-renderers.md, section 9).
// Early: draws are drawn into host render targets, one per EDRAM surface,
// with the guest's textures (or the texture a resolve wrote to their address);
// resolves copy render targets into host textures and do their clears, and
// the swap presents the texture resolved to the front buffer (or the front
// buffer in guest memory). The ring and every synchronisation are the SDK's
// CommandProcessor (through NullCommandProcessor).
class NativeCommandProcessor : public NullCommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  std::string GetWindowTitleText() const override;

  // store_shaders: the title's shaders and native pipelines on disk, the
  // stored ones created in the background now (PipelineStorage).
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking,
                               std::function<void()> completion_callback = nullptr) override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void InvalidateGpuMemory() override;
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  // Tells the texture cache which fetch constants changed: it reads a slot's
  // binding again only then (the constant blocks always come through here or
  // WriteRegistersFromMem).
  void WriteRegister(uint32_t index, uint32_t value) override;
  // As the SDK's Vulkan backend: a range in a constant block is stored at once
  // and checked against the bindings once, not register by register through
  // WriteRegister.
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;

  // readback_resolve: resolves the guest's CPU reads copied back from the
  // shared memory (the SDK decides which, and when).
  bool CanReadBackResolvesNow() const override { return true; }
  void ReadBackResolvesNow(const ResolveFetch* fetches, size_t count,
                           std::vector<const uint8_t*>& data_out) override;
  // readback_resolve_full_deferred: a resolve's copy back is recorded at the
  // resolve and only waited for at the guest's first read of it.
  bool CanDeferResolveReadbacks() const override { return true; }
  void CompleteDeferredReadbacks(const ResolveFetch* fetches, size_t count,
                                 std::vector<const uint8_t*>& data_out) override;

  // vulkan_submit_on_primary_buffer_end: what the game sent so far goes to the
  // GPU at the end of each of its command buffers, not only at the swap.
  void OnPrimaryBufferEnd() override;
  void OnGuestMemorySyncPoint() override;
  SharedMemory* GetSharedMemoryForStats() const override { return shared_memory_.get(); }
  // frame_stats_interval: the submitter's GPU time and counts, pipeline binds.
  void CollectRendererFrameCounters(RendererFrameCounters& counters) override;

  // Occlusion queries (EVENT_WRITE_ZPD) on host occlusion queries, through the
  // SDK's report bookkeeping and its occlusion_query modes: a query per
  // segment of the draws' render pass, copied at the end of its submission
  // into the SDK pool's readback buffer. VIZ surveys (occlusion_query_viz) are
  // drawn without writes in a segment of their ID; until a survey resolves,
  // the draws under its ID run under its result (conditional rendering).
  void EnsureQueryResources() override;
  bool IsQueryPoolReady() const override;
  bool CanOpenQuery() const override;
  QueryOpenResult OpenQuery(bool can_close_submission) override;
  bool CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                  uint64_t& out_submission) override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle, uint64_t wait_for_submission) override;
  void AwaitVIZQueryResolve(uint64_t wait_for_submission) override;
  void PollCompletedSubmission() override;
  uint64_t GetCompletedSubmission() const override;

 private:
  // Frames the CPU may record ahead of the GPU.
  static constexpr uint32_t kMaxFramesInFlight = 3;

  // What a draw does not do yet, logged once each.
  enum DrawLog : uint32_t {
    kDrawLogMemExport = 1u << 0,
    kDrawLogTessellation = 1u << 1,
    kDrawLogFetchConstant = 1u << 2,
    kDrawLogTopology = 1u << 3,
    kDrawLogAliasedTargets = 1u << 4,
    kDrawLogResolvedFormat = 1u << 5,
    kDrawLogReadbackMode = 1u << 6,
    kDrawLogQueryPool = 1u << 7,
  };
  void LogDrawOnce(uint32_t reason_bit, const char* message);

  ui::vulkan::VulkanDevice* GetVulkanDevice() const;
  void ShutdownModules();
  // Whether the fetch can sample the resolve destination's image: a 1D or 2D
  // texture in the resolved format (depth: k_24_8 / k_24_8_FLOAT of its
  // depth format).
  static bool IsResolvedTextureFetchable(const ResolvedTexture& texture,
                                         const xenos::xe_gpu_texture_fetch_t& fetch);
  // At a resolution scale: the view of a scaled composite for the fetch
  // constant (textures the game rendered, Composites), VK_NULL_HANDLE to
  // sample the texture cache's 1x texture.
  // Memoized per fetch constant for the frame (scaled_view_memos_).
  VkImageView GetScaledRenderTextureView(uint32_t fetch_constant, uint64_t frame);
  VkImageView ComputeScaledRenderTextureView(uint32_t fetch_constant, uint64_t frame,
                                             bool bound_2d,
                                             const NativeTextureCache::BoundTexture2D& bound);
  // The front buffer (resolved, the last large colour resolve, or from guest
  // memory) to the presenter.
  void PresentFrame(ui::Presenter* presenter, uint32_t frontbuffer_ptr,
                    uint32_t frontbuffer_width, uint32_t frontbuffer_height);
  // The last frame (numbered from 1) the GPU has completed.
  uint64_t CompletedFrame();
  // Requests the vertex fetch constants' ranges the shader reads; false when
  // one is not a vertex fetch constant (the draw is skipped).
  // Also adds the ranges to draw_read_ranges_.
  bool RequestVertexBuffers(const Shader& shader);
  // readback_resolve after a resolve that wrote the guest's bytes to
  // [written_start, + written_length) of the shared memory: copied into guest
  // memory now, the GPU waited for, if the SDK says so.
  void ReadbackResolve(uint32_t written_start, uint32_t written_length);

  Submitter submitter_;
  std::unique_ptr<NativeSharedMemory> shared_memory_;
  std::unique_ptr<NativePrimitiveProcessor> primitive_processor_;
  // The rows a draw can reach (native_claim_draw_extent), as xenos' render
  // target cache bounds them.
  std::unique_ptr<DrawExtentEstimator> draw_extent_estimator_;
  std::unique_ptr<NativeTextureCache> texture_cache_;
  Shaders shaders_;
  Pipelines pipelines_;
  PipelineStorage pipeline_storage_;
  Bindings bindings_;
  RenderTargets render_targets_;
  Resolver resolver_;
  Composites composites_;
  // Scratch for the draw's composites, and the fetch constants sampled from
  // one (textures_resolution_scaled).
  std::vector<ResolvedTexture*> composite_overlapping_;
  std::vector<Composites::Piece> composite_pieces_;
  // The pieces' destinations, for RectsCoverTexture (reused, not per call).
  std::vector<Rect> composite_cover_rects_;
  uint32_t scaled_texture_mask_ = 0;
  // Why fetches at a resolution scale were or weren't sampled from a
  // composite, counted since the last summary (logged every 300 frames).
  enum ScaledFetch : uint32_t {
    kScaledFetchServed,
    kScaledFetchNotA2DTexture,
    kScaledFetchLayout,
    kScaledFetchFormat,
    kScaledFetchResolveMismatch,
    kScaledFetchNotPlaced,
    kScaledFetchOutside,
    kScaledFetchComposeFailed,
    kScaledFetchCount,
  };
  std::array<uint64_t, kScaledFetchCount> scaled_fetch_counts_{};
  // Reasons whose first case was logged with its values.
  uint32_t scaled_fetch_logged_ = 0;
  void CountScaledFetch(ScaledFetch reason) {
    ++scaled_fetch_counts_[reason];
    scaled_fetch_last_counted_ = reason;
  }
  // What the last computation counted (kScaledFetchCount: nothing).
  ScaledFetch scaled_fetch_last_counted_ = kScaledFetchCount;
  // GetScaledRenderTextureView's last result for each fetch constant, reused
  // while the frame, the texture cache's image and the fetch constant are the
  // same and no resolve image of the guest memory it looked at changed
  // (composites with a base are drawn again each frame, and are destroyed
  // only at a frame's end). It was dropped by every resolve anywhere (70 or
  // more a frame): 3,500-5,000 lookups a frame computed again in town in Red
  // Dead Redemption.
  struct ScaledViewMemo {
    uint64_t frame = 0;
    // Checked up to this resolver generation (Resolver::UnchangedSince).
    uint64_t resolver_generation = 0;
    VkImageView bound_view = VK_NULL_HANDLE;
    uint32_t fetch[6] = {};
    // The guest memory GetPlaceableOverlapping was asked about.
    uint32_t range_start = 0;
    uint32_t range_end = 0;
    VkImageView view = VK_NULL_HANDLE;
    ScaledFetch counted = kScaledFetchCount;
    // Changes with each computation (binding_view_memos_ compare it).
    uint64_t serial = 0;
  };
  std::array<ScaledViewMemo, 32> scaled_view_memos_{};
  uint64_t scaled_view_serial_ = 0;
  // Whether the memo's result still holds as far as the resolver goes,
  // bringing its generation up to date if so.
  bool IsScaledViewMemoCurrent(ScaledViewMemo& memo, uint64_t resolver_generation) {
    if (memo.resolver_generation != resolver_generation &&
        !resolver_.UnchangedSince(memo.resolver_generation, memo.range_start, memo.range_end)) {
      return false;
    }
    memo.resolver_generation = resolver_generation;
    return true;
  }
  void LogScaledFetchSummary();
  ResolveReadback readback_;
  // Draws with memory export: where they write, and readback_memexport.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
  // The shared memory the draw's shaders read directly (vertex fetches),
  // marked in its render pass once it's entered (native_hoist_uploads).
  std::vector<std::pair<uint32_t, uint32_t>> draw_read_ranges_;
  MemexportReadback memexport_readback_;
  // Occlusion queries: the pool (made at the first report), the query open in
  // the draws' render pass, and the closed ones until their submission
  // completes, oldest first.
  std::unique_ptr<vulkan::VulkanQueryPool> query_pool_;
  // The presenter whose publish steps go to the submission thread
  // (native_threaded_submission), or null.
  ui::Presenter* present_deferred_presenter_ = nullptr;
  uint32_t active_query_index_ = UINT32_MAX;
  uint32_t active_query_generation_ = 0;
  struct PendingQueryResolve {
    uint64_t submission = 0;
    uint32_t query_index = 0;
    uint32_t query_generation = 0;
    // Host samples a guest sample, in sixteenths (scale4 squared).
    uint32_t scale_area16 = 16;
    ReportHandle report_handle = kInvalidReportHandle;
    VIZQueryHandle viz;
  };
  std::deque<PendingQueryResolve> query_resolves_in_flight_;
  vulkan::VulkanQueryPool::ResolveBatch query_resolve_batch_;
  // Closes the open query (before the draws' render pass ends) and records
  // the copies of the closed ones (before a submission).
  void OnDrawRenderPassEnding();
  void RecordQueryResolves();
  // VIZ predicates: one 32-bit value per ID (VK_EXT_conditional_rendering),
  // written from a survey's query outside the render pass.
  struct PendingVIZCopy {
    uint32_t id = 0;
    uint32_t query_index = 0;
  };
  std::vector<PendingVIZCopy> viz_pending_copies_;
  VkBuffer viz_predicate_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory viz_predicate_memory_ = VK_NULL_HANDLE;
  bool viz_predicate_buffer_failed_ = false;
  bool EnsureVIZPredicateBuffer();
  void RecordVIZPredicateCopies(CommandRecorder* command_buffer);
  // The predicate the draw being issued runs under (offset_out into it), or
  // VK_NULL_HANDLE; may end the render pass to copy its value.
  VkBuffer BindVIZPredicate(VkDeviceSize& offset_out);
  std::vector<std::pair<uint32_t, uint32_t>> readback_ranges_;
  std::vector<const uint8_t*> readback_data_;
  Present present_;
  bool context_ready_ = false;
  // The draw resolution scale in quarters (4 = 1x, 6 = 1.5x; both axes),
  // chosen at SetupContext.
  uint32_t scale4_ = 4;
  // A draw of this frame was skipped while its pipeline is being created, and
  // since when frames are held back for that.
  bool frame_used_pending_pipeline_ = false;
  bool incomplete_frames_held_ = false;
  std::chrono::steady_clock::time_point incomplete_frames_held_since_;
  bool frontbuffer_miss_logged_ = false;
  uint32_t draw_logged_ = 0;

  // What the open command buffer (Submitter::buffer_serial) has bound.
  uint64_t bound_serial_ = UINT64_MAX;
  uint64_t bound_graphics_state_ = 0;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  // The targets of the draws' render pass this command processor last began
  // (native_keep_render_pass): a later draw writing a subset of them in the
  // same slots stays in it while it is open (Submitter::render_pass).
  struct OpenPassTargets {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    Pipelines::RenderPassKey key;
    RenderTarget* color[xenos::kMaxColorRenderTargets] = {};
    RenderTarget* depth = nullptr;
  };
  OpenPassTargets open_pass_;
  // Draw pipeline binds since the last CollectRendererFrameCounters.
  uint64_t stats_pipeline_binds_ = 0;
  // The views and samplers of the last draw's textures (draw_textures_,
  // scaled_texture_mask_) and what they were made from: a draw with the same
  // shaders, fetch constants (fetch_constant_changes_), texture cache bindings
  // and resolve images in the same frame takes them as they are.
  struct DrawTexturesMemo {
    bool valid = false;
    const void* vertex_shader = nullptr;
    const void* pixel_shader = nullptr;
    uint64_t frame = 0;
    uint64_t fetch_constant_changes = 0;
    uint64_t binding_generation = 0;
    uint64_t resolver_generation = 0;
    bool scaled = false;
  };
  DrawTexturesMemo draw_textures_memo_;
  // Fetch constant registers written with another value.
  uint64_t fetch_constant_changes_ = 0;
  // The same per fetch constant: a binding's view stays while its own
  // constant, its texture cache binding and the resolve images are unchanged.
  uint64_t fetch_constant_serials_[xenos::kTextureFetchConstantCount] = {};
  // When the draw's textures differ from the last draw's (most draws: other
  // shaders, or one fetch constant of several changed), each binding's view
  // as the last time for its fetch constant, dimension and signedness, in
  // the same frame.
  struct BindingViewMemo {
    uint64_t frame = 0;
    uint64_t fetch_serial = 0;
    uint64_t binding_generation = 0;
    // scaled_lookup: the fetch constant's ScaledViewMemo::serial it came from
    // (that memo still current: IsScaledViewMemoCurrent).
    uint64_t scaled_serial = 0;
    VkImageView view = VK_NULL_HANDLE;
    // native_scaled_render_textures on and the binding a 1D / 2D one, and
    // whether the view is a scaled render texture.
    bool scaled_lookup = false;
    bool scaled = false;
  };
  BindingViewMemo binding_view_memos_[xenos::kTextureFetchConstantCount * 8];
  uint64_t stats_draw_textures_reused_ = 0;
  uint64_t stats_binding_views_reused_ = 0;
  // Draws that stayed in the open render pass with targets they don't write,
  // and why others with a pass open didn't: [0] they write all of its targets
  // and more, [1] other targets, [2] their scissor is outside its extent.
  uint64_t stats_pass_kept_draws_ = 0;
  uint64_t stats_pass_not_kept_[3] = {};
  // frame_stats_interval: IssueDraw's host ticks by phase since the last
  // CollectRendererFrameCounters (DrawPhaseTimer in the .cpp).
  enum class DrawPhase : uint32_t {
    // Shader choice, modifications, translations, memory export ranges.
    kShaders,
    // Index processing (primitive processor).
    kIndices,
    // Vertex and memory export ranges requested in the shared memory.
    kVertexData,
    // Textures requested from the texture cache (loads).
    kTextures,
    // Render targets, render pass and framebuffer (the claims are kClaims).
    kTargets,
    kPipeline,
    // Viewport, dynamic state, system constants.
    kState,
    // Attachment layouts, the shared memory barrier.
    kLayouts,
    // Texture views (scaled render textures, composites, resolved images).
    kViews,
    // Samplers.
    kSamplers,
    // The render pass, the pipeline and dynamic state recorded.
    kPass,
    // Descriptors and constants (Bindings::Bind).
    kDescriptors,
    // The query segment, the draw and what follows it.
    kRecord,
    // EDRAM claims of the draw's targets and what they record (transfers,
    // clears), with the ownership updates: timed apart from kTargets, last so
    // that the earlier phases keep their place in the log.
    kClaims,
    kCount,
  };
  uint64_t stats_draw_phase_ticks_[size_t(DrawPhase::kCount)] = {};
  // One draw in kDrawPhaseSampleRate is timed by phase (the clock reads of
  // every draw cost 11-14% of IssueDraw), its ticks counted that many times.
  static constexpr uint32_t kDrawPhaseSampleRate = 8;
  uint32_t stats_draw_phase_sample_ = 0;
  // frame_stats_interval: IssueCopy's host ticks in the resolve itself and in
  // its readback (which waits for the graphics card); the scaled render
  // texture lookups of the draws' bindings, and those not answered by the
  // fetch constant's memo.
  uint64_t stats_copy_resolve_ticks_ = 0;
  uint64_t stats_copy_readback_ticks_ = 0;
  uint64_t stats_scaled_view_lookups_ = 0;
  uint64_t stats_scaled_view_computed_ = 0;
  // Textures of 1 MiB and more that resolves overlap, per
  // ComputeScaledRenderTextureView computation (memo hits not counted): 0 / 1 /
  // 2 / 3+ placed pieces, covered by one piece, covered only by several
  // together. Logged with native_profile_transfers.
  std::array<uint64_t, 6> stats_large_coverage_{};
  bool bound_dynamic_state_valid_ = false;
  DynamicState bound_dynamic_state_ = {};
  // The last draw's render targets from the surface registers and the colour
  // mask: the same registers give the same targets (they live until
  // shutdown), so a draw with them skips the lookups.
  struct DrawTargetsMemo {
    bool valid = false;
    uint32_t surface_info = 0;
    uint32_t color_info[xenos::kMaxColorRenderTargets] = {};
    uint32_t depth_info = 0;
    uint32_t color_mask_in = 0;
    bool depth_needed = false;
    // The lookups' results.
    RenderTarget* color[xenos::kMaxColorRenderTargets] = {};
    RenderTarget* depth = nullptr;
    uint32_t color_mask = 0;
    uint32_t color_attachments = 0;
    uint32_t fixed16_color_targets = 0;
  };
  DrawTargetsMemo draw_targets_memo_;
  // The last draw's render pass and framebuffer and what they were looked up
  // with (both caches give the same handles for the same key).
  struct DrawFramebufferMemo {
    bool valid = false;
    Pipelines::RenderPassKey key;
    RenderTarget* color[xenos::kMaxColorRenderTargets] = {};
    RenderTarget* depth = nullptr;
    VkExtent2D extent = {};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };
  DrawFramebufferMemo draw_framebuffer_memo_;
  // The last ready translation of each stage (vertex, pixel) for its shader
  // and modification: translations stay as they are once translated, and
  // shaders live until shutdown.
  struct TranslationMemo {
    const vulkan::VulkanShader* shader = nullptr;
    uint64_t modification = 0;
    vulkan::VulkanShader::VulkanTranslation* translation = nullptr;
  };
  TranslationMemo translation_memos_[2];
  // The last draw's pipeline layout by its binding counts, and its pipeline by
  // everything Pipelines::GetPipeline keys it with (only a created one).
  struct LayoutMemo {
    uint32_t counts[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const Pipelines::Layout* layout = nullptr;
  };
  LayoutMemo layout_memo_;
  struct PipelineMemo {
    VkShaderModule modules[4] = {};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    PipelineState state = {};
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  PipelineMemo pipeline_memo_;
  // The views and samplers of the draw being recorded.
  DrawTextures draw_textures_;

  // The submission that ended each of the last frames, by frame % count.
  std::array<uint64_t, kMaxFramesInFlight> frame_submissions_{};
  // Swaps so far; the frame being recorded is frame_ + 1.
  uint64_t frame_ = 0;
  // (frame, its last submission) of frames the GPU may not have finished.
  std::deque<std::pair<uint64_t, uint64_t>> frames_in_flight_;
  uint64_t completed_frame_ = 0;
  // Draws recorded in the frame.
  uint64_t frame_draws_recorded_ = 0;
  // vulkan_submission_draw_limit: an integrated GPU (the auto limit applies),
  // the draws begun in the submission draws_in_submission_of_ (its number),
  // and the submissions ended for the limit since the last frame statistics.
  // EdramOwnership's walk counts at the last frame statistics.
  EdramOwnership::WalkCounts stats_walk_last_;
  bool integrated_gpu_ = false;
  uint32_t draws_in_submission_ = 0;
  uint64_t draws_in_submission_of_ = 0;
  uint64_t stats_submission_splits_ = 0;
};

}  // namespace rex::graphics::native
