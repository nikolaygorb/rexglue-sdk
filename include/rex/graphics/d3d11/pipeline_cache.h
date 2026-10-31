/**
 * @file        graphics/d3d11/pipeline_cache.h
 * @brief       Shaders and fixed-function state of the experimental Direct3D 11
 *              backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/d3d11/geometry_shader.h>
#include <rex/graphics/d3d11/render_target_cache.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/storage.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/string/buffer.h>
#include <rex/thread.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

class D3D11Shader : public DxbcShader {
 public:
  class D3D11Translation : public DxbcTranslation {
   public:
    D3D11Translation(D3D11Shader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}

   private:
    friend class PipelineCache;
    // The guest translation is attempted once, a failure is kept.
    bool translation_attempted_ = false;
    // Shader Model 5.0 shader objects, by whether the shared memory is bound
    // as a UAV (memexport in the pipeline) rather than read through the SRV,
    // and the channel mappings of the bound textures (Direct3D 11 views have
    // none). Pipeline descriptions refer to them by index. A deque: creation
    // threads write elements while new ones are added.
    struct Native {
      enum State : uint32_t {
        kNotRequested,
        // Queued for or being created by a creation thread.
        kPending,
        // shader is set (null if the conversion or the creation failed).
        kDone,
      };
      bool shared_memory_is_uav = false;
      std::vector<TextureSwizzle> texture_swizzles;
      std::atomic<uint32_t> state{kNotRequested};
      Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
    };
    std::deque<Native> natives_;
  };

  D3D11Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
              size_t ucode_dword_count)
      : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count) {}

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override {
    return new D3D11Translation(*this, modification);
  }
};

// Direct3D 11 has no pipeline state objects: a pipeline here is the set of
// shader objects and immutable state objects of a draw, cached by the same
// kind of description the D3D12 backend uses. Shaders are translated by the
// shared DXBC translator (bindful, Shader Model 5.1) and converted to Shader
// Model 5.0 by ConvertShaderBytecode.
//
// With async_shader_compilation (S7 of the requirements), the conversion and
// Create*Shader of the guest shaders, where the driver compiles them, run on
// creation threads (the device is free-threaded), and draws whose shaders
// aren't ready are skipped, after async_pipeline_wait_ms, as on D3D12.
// Pipelines without a pixel shader wait for theirs unless
// async_depth_only_pipelines, and memexporting ones always do. Translation
// stays on the command processor thread: the draw needs the texture bindings
// it sets up (for the textures' channel mappings), and translations of one
// shader must not run at the same time (the first sets the shader's
// bindings).
class PipelineCache {
 public:
  struct Pipeline {
    // For tessellation, a built-in one, with a built-in hull shader and the
    // translated guest vertex shader as the domain shader.
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11HullShader* hull_shader = nullptr;
    ID3D11DomainShader* domain_shader = nullptr;
    ID3D11GeometryShader* geometry_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
    uint32_t sample_mask = UINT32_MAX;
    xenos::MsaaSamples host_msaa_samples = xenos::MsaaSamples::k1X;
  };

  // GetBackendShaderBytecodeOptions for the device. alias_shared_memory_uav
  // only for translated guest shaders of a pipeline without memexport.
  static ShaderBytecodeOptions GetShaderBytecodeOptions(const ui::d3d11::D3D11Provider& provider,
                                                        bool alias_shared_memory_uav);

  PipelineCache(D3D11CommandProcessor& command_processor, const RegisterFile& register_file,
                const D3D11RenderTargetCache& render_target_cache);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();
  void ClearCache();

  // For a draw resolution scale change while the game runs, as D3D12's: the
  // pipelines and the shader translations, which have the scale baked in, are
  // destroyed before the render target cache gets the new scale, and the
  // translator is created again for it after that.
  void DestroyDrawResolutionScaleDependentObjects();
  bool CreateDrawResolutionScaleDependentObjects();

  // Persistent storage (B4 of the requirements): the guest shaders in the
  // .xsh file shared with D3D12 and Vulkan, the pipelines in
  // <title>.rtv.d3d11.xpso. At initialization, the stored pipelines' shaders
  // are translated (in parallel, one shader per job) and their native
  // variants created on the creation threads; if blocking, that is waited
  // for and the pipelines are created too, else they're created at their
  // first draw. On the command processor thread or while it's paused.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);
  void ShutdownShaderStorage();
  // At the end of a frame: has the storage files flushed if written.
  void EndFrame();

  D3D11Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count);
  void AnalyzeShaderUcode(Shader& shader);

  // Generated copies of the D3D12 pipeline cache's functions
  // (pipeline_cache_copies.cpp).
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control, bool apply_polygon_offset_in_shader) const;

  // Translates the shader if not attempted yet (the texture and sampler
  // bindings of its Shader are known after that). False if it failed.
  bool EnsureTranslated(D3D11Shader::D3D11Translation& translation);

  // Translates and creates the shaders if needed, with the channel mappings
  // of the textures bound to each stage (non-identity ones only, by SRV
  // register). Null if the pipeline can't be used (the reason is logged once
  // per shader or pipeline), or, with pending_out, if its shaders are still
  // being created by creation threads (call again later). zpd_total for a
  // hybrid occlusion query draw (the pixel shader's modification has it too;
  // without a guest pixel shader, a built-in one counts).
  const Pipeline* ConfigurePipeline(
      D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader, bool shared_memory_is_uav, bool zpd_total,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      const std::vector<TextureSwizzle>& vertex_texture_swizzles,
      const std::vector<TextureSwizzle>& pixel_texture_swizzles, bool& pending_out);

  // Pipelines and shaders that failed (for diagnostics; S3).
  uint32_t failed_pipeline_count() const { return failed_pipeline_count_; }

 private:
  enum class CullMode : uint32_t { kNone, kFront, kBack, kDisableRasterization };

  // Zero-initialized and compared bytewise, like D3D12's PipelineDescription.
  struct PipelineDescription {
    const D3D11Shader::D3D11Translation* vertex_shader;
    const D3D11Shader::D3D11Translation* pixel_shader;
    // Indices in the translations' natives_.
    uint32_t vertex_shader_native;
    uint32_t pixel_shader_native;
    uint32_t geometry_shader_key;
    uint32_t shared_memory_is_uav;
    // Counts the coverage into the ZPD counter (hybrid occlusion queries).
    uint32_t zpd_total;
    uint32_t tessellated;
    xenos::TessellationMode tessellation_mode;
    // Rasterizer.
    CullMode cull_mode;
    uint32_t fill_mode_wireframe;
    uint32_t front_counter_clockwise;
    uint32_t depth_clip;
    int32_t depth_bias;
    float depth_bias_slope_scaled;
    // Depth / stencil.
    xenos::CompareFunction depth_func;
    uint32_t depth_write;
    uint32_t stencil_enable;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    xenos::StencilOp stencil_front_fail_op;
    xenos::StencilOp stencil_front_depth_fail_op;
    xenos::StencilOp stencil_front_pass_op;
    xenos::CompareFunction stencil_front_func;
    xenos::StencilOp stencil_back_fail_op;
    xenos::StencilOp stencil_back_depth_fail_op;
    xenos::StencilOp stencil_back_pass_op;
    xenos::CompareFunction stencil_back_func;
    xenos::DepthRenderTargetFormat depth_format;
    // Blending.
    struct RenderTarget {
      uint32_t used;
      uint32_t write_mask;
      xenos::BlendFactor src_blend;
      xenos::BlendFactor dest_blend;
      xenos::BlendOp blend_op;
      xenos::BlendFactor src_blend_alpha;
      xenos::BlendFactor dest_blend_alpha;
      xenos::BlendOp blend_op_alpha;
    };
    RenderTarget render_targets[xenos::kMaxColorRenderTargets];
    xenos::MsaaSamples host_msaa_samples;

    // Of the stored state (PipelineStoredData).
    static constexpr uint32_t kVersion = 0x20261030;
  };
  // A pipeline in the storage file: the shaders by hash and modification, the
  // channel mappings of their native variants (whether the shared memory is a
  // UAV is in the state), and the state with the shader fields (translation
  // pointers, native indices) zero. Pipelines with more non-identity mappings
  // per stage than fit aren't stored.
  static constexpr uint32_t kMaxStoredTextureSwizzles = 16;
  REXPACKEDSTRUCT(PipelineStoredData, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 without a pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    uint32_t vertex_texture_swizzle_count;
    uint32_t pixel_texture_swizzle_count;
    TextureSwizzle vertex_texture_swizzles[kMaxStoredTextureSwizzles];
    TextureSwizzle pixel_texture_swizzles[kMaxStoredTextureSwizzles];
    PipelineDescription state;
  });
  REXPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineStoredData description;
  });
  struct PipelineDescriptionHasher {
    size_t operator()(const PipelineDescription& description) const;
  };
  struct PipelineDescriptionEqual {
    bool operator()(const PipelineDescription& a, const PipelineDescription& b) const;
  };

  // With the render target cache's draw resolution scale.
  bool CreateTranslatorAndDepthOnlyPixelShader();
  static uint32_t FindOrAddNative(D3D11Shader::D3D11Translation& translation,
                                  bool shared_memory_is_uav,
                                  const std::vector<TextureSwizzle>& texture_swizzles);
  // Makes sure the native shader is created or being created: right here, or,
  // if async, by a creation thread (if it's already being created there, not
  // async waits for it). Returns whether it's done (its shader is null if the
  // translation, the conversion or the creation failed).
  bool RequestNativeShader(D3D11Shader::D3D11Translation& translation, uint32_t native_index,
                           bool async);
  // Converts SM 5.1 DXBC and creates the shader object; logs the reason of a
  // failure with the name. Thread-safe; debug layer messages are logged only
  // on the command processor thread (log_debug_messages).
  Microsoft::WRL::ComPtr<ID3D11DeviceChild> CreateNativeShader(
      const void* sm51, size_t size, bool shared_memory_is_uav, const std::string& name,
      const std::vector<TextureSwizzle>& texture_swizzles = {}, bool log_debug_messages = true);
  static std::string GetNativeShaderName(const D3D11Shader::D3D11Translation& translation);

  // Translates with the translator given (one of a storage loading thread).
  static bool TranslateWith(DxbcShaderTranslator& translator,
                            string::StringBuffer& ucode_disasm_buffer,
                            D3D11Shader::D3D11Translation& translation);
  std::unique_ptr<DxbcShaderTranslator> CreateTranslator() const;
  // The needed (shader hash, modification) pairs, on threads, all
  // modifications of a shader on one.
  void TranslateShadersForStorage(const std::set<std::pair<uint64_t, uint64_t>>& translations_needed);
  // The storage writes of a new pipeline and of a newly translated shader.
  void StorePipeline(const PipelineDescription& description,
                     const std::vector<TextureSwizzle>& vertex_texture_swizzles,
                     const std::vector<TextureSwizzle>& pixel_texture_swizzles);
  void StoreShader(const Shader& shader);
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;
  // Hashes of the descriptions in the pipeline storage file. Unlike D3D12's,
  // stored pipelines may be created only at their first draw (when the
  // storage isn't loaded blocking), so they'd be written again then; the
  // writer doesn't check.
  std::unordered_set<uint64_t> stored_pipeline_hashes_;
  bool shader_storage_flush_needed_ = false;
  bool pipeline_storage_flush_needed_ = false;

  // Creation threads (d3d11_shader_creation_threads).
  void StartCreationThreads();
  void StopCreationThreads();
  // Drops the queued native shaders (back to not requested) and waits until
  // the creation threads are idle, before translations are destroyed.
  void DrainCreationQueue();
  // Waits until everything queued is created.
  void AwaitCreationQueue();
  void CreationThread();
  struct CreationJob {
    D3D11Shader::D3D11Translation::Native* native;
    // The translation's binary, kept until the job is done or dropped.
    const std::vector<uint8_t>* sm51;
    std::string name;
  };
  std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads_;
  std::mutex creation_mutex_;
  std::condition_variable creation_request_cond_;
  // Notified after each job is done.
  std::condition_variable creation_done_cond_;
  std::deque<CreationJob> creation_queue_;
  uint32_t creation_threads_busy_ = 0;
  bool creation_threads_shutdown_ = false;
  ID3D11GeometryShader* GetOrCreateGeometryShader(GeometryShaderKey key);
  void GetCurrentStateDescription(
      D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader, bool shared_memory_is_uav, bool zpd_total,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      PipelineDescription& description_out);
  std::unique_ptr<Pipeline> CreatePipeline(const PipelineDescription& description);

  D3D11CommandProcessor& command_processor_;
  // Names as in the D3D12 pipeline cache, for the generated copies.
  const RegisterFile& register_file_;
  const D3D11RenderTargetCache& render_target_cache_;
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;

  rex::string::StringBuffer ucode_disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<D3D11Shader>> shaders_;

  // Built-in vertex and hull shaders of tessellation, as D3D12 chooses them
  // (CreateD3D12Pipeline). Null for a combination that doesn't exist.
  ID3D11HullShader* GetHullShader(xenos::TessellationMode tessellation_mode,
                                  Shader::HostVertexShaderType host_vertex_shader_type) const;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> tessellation_indexed_vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> tessellation_adaptive_vertex_shader_;
  // By TessellationMode, then triangle control-point-indexed, triangle
  // patch-indexed, quad control-point-indexed, quad patch-indexed.
  Microsoft::WRL::ComPtr<ID3D11HullShader> hull_shaders_[3][4];

  // Pixel shaders used without a guest one.
  Microsoft::WRL::ComPtr<ID3D11PixelShader> depth_only_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> float24_truncate_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> float24_round_pixel_shader_;
  // Depth-only pixel shaders counting the coverage for hybrid occlusion
  // queries (also with float24 conversion), with the translator's draw
  // resolution scale; null without hybrid queries.
  Microsoft::WRL::ComPtr<ID3D11PixelShader> zpd_total_depth_only_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> zpd_total_float24_truncate_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> zpd_total_float24_round_pixel_shader_;

  // Null for a key whose shader failed.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11GeometryShader>> geometry_shaders_;

  // Null for a description that failed.
  std::unordered_map<PipelineDescription, std::unique_ptr<Pipeline>, PipelineDescriptionHasher,
                     PipelineDescriptionEqual>
      pipelines_;
  uint32_t failed_pipeline_count_ = 0;
};

}  // namespace rex::graphics::d3d11
