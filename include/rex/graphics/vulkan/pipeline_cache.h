#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/storage.h>
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

// TODO(Triang3l): Create a common base for both the Vulkan and the Direct3D
// implementations.
class VulkanPipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;

   protected:
    PipelineLayoutProvider() = default;
  };

  struct Pipeline {
    // VK_NULL_HANDLE if creation has failed or is still pending.
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    // The layouts are owned by the VulkanCommandProcessor, and must not be
    // destroyed by it while the pipeline cache is active. Published before
    // the pipeline.
    std::atomic<const PipelineLayoutProvider*> pipeline_layout{nullptr};
    // Queued for creation on a creation thread (async compilation or the
    // shader storage), cleared after the pipeline is published or creation
    // has failed.
    std::atomic<bool> creation_pending{false};
    Pipeline() = default;
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(other.pipeline_layout.load(std::memory_order_acquire)),
          creation_pending(other.creation_pending.load(std::memory_order_acquire)) {}
    Pipeline& operator=(const Pipeline& other) {
      if (this == &other) {
        return *this;
      }
      pipeline.store(other.pipeline.load(std::memory_order_acquire), std::memory_order_release);
      pipeline_layout.store(other.pipeline_layout.load(std::memory_order_acquire),
                            std::memory_order_release);
      creation_pending.store(other.creation_pending.load(std::memory_order_acquire),
                             std::memory_order_release);
      return *this;
    }
  };

  VulkanPipelineCache(VulkanCommandProcessor& command_processor, const RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages, bool zpd_hybrid_supported);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();
  // Shader and pipeline storage.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking, std::function<void()> completion_callback = nullptr);
  void ShutdownShaderStorage();
  void EndSubmission();

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                           uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control, bool apply_polygon_offset_in_shader) const;

  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader);
  // The pipeline may be not created yet if it's created asynchronously
  // (Pipeline::pipeline is VK_NULL_HANDLE while Pipeline::creation_pending is
  // true) - draws with it must be skipped.
  bool ConfigurePipeline(VulkanShader::VulkanTranslation* vertex_shader,
                         VulkanShader::VulkanTranslation* pixel_shader,
                         const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask,
                         VulkanRenderTargetCache::RenderPassKey render_pass_key, bool zpd_total,
                         bool viz_survey, Pipeline** pipeline_out);
  bool IsCreatingPipelines() const;

  // For changing the draw resolution scale at runtime, with the GPU idle and no
  // pipeline being created. Destroy releases all pipelines, all shader
  // translations, the internal shaders and the translator, which have the scale
  // baked in. Create, called once the render target cache has been
  // reinitialized with the new scale, recreates the translator and the
  // internal shaders; translations and pipelines are then made on demand.
  void DestroyDrawResolutionScaleDependentObjects();
  bool CreateDrawResolutionScaleDependentObjects();
  // Waits for any pipeline creation needed by the current draw path to finish
  // before state is consumed. This was added so strict ZPD query paths stop
  // racing pipeline compilation and then blocking work on incomplete state.
  void AwaitPipelineCompletion();


 private:
  // Creates the shader translator for the render target cache's draw
  // resolution scale and the internal shaders it generates.
  bool CreateTranslatorAndInternalShaders();
  void DestroyInternalShaders();
  void StartCreationThreads();
  void StopCreationThreads();
  void DestroyAllPipelines();

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelinePrimitiveTopology : uint32_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : uint32_t {
    kFill,
    kLine,
    kPoint,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  REXPACKEDSTRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;  // 4
    PipelineBlendFactor dst_color_blend_factor : 4;  // 8
    xenos::BlendOp color_blend_op : 3;               // 11
    PipelineBlendFactor src_alpha_blend_factor : 4;  // 15
    PipelineBlendFactor dst_alpha_blend_factor : 4;  // 19
    xenos::BlendOp alpha_blend_op : 3;               // 22
    uint32_t color_write_mask : 4;                   // 26
  });

  REXPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if no pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    // Shader stages.
    PipelineGeometryShader geometry_shader : 2;  // 2
    // Input assembly.
    PipelinePrimitiveTopology primitive_topology : 3;  // 5
    uint32_t primitive_restart : 1;                    // 6
    xenos::TessellationMode tessellation_mode : 2;     // 8
    // Rasterization.
    uint32_t depth_clamp_enable : 1;       // 9
    PipelinePolygonMode polygon_mode : 2;  // 11
    uint32_t cull_front : 1;               // 12
    uint32_t cull_back : 1;                // 13
    uint32_t front_face_clockwise : 1;     // 14
    uint32_t rasterizer_discard : 1;       // 15
    // Depth / stencil.
    uint32_t depth_write_enable : 1;                      // 16
    xenos::CompareFunction depth_compare_op : 3;          // 19
    uint32_t stencil_test_enable : 1;                     // 20
    xenos::StencilOp stencil_front_fail_op : 3;           // 23
    xenos::StencilOp stencil_front_pass_op : 3;           // 26
    xenos::StencilOp stencil_front_depth_fail_op : 3;     // 29
    xenos::CompareFunction stencil_front_compare_op : 3;  // 32
    xenos::StencilOp stencil_back_fail_op : 3;            // 35

    xenos::StencilOp stencil_back_pass_op : 3;           // 3
    xenos::StencilOp stencil_back_depth_fail_op : 3;     // 6
    xenos::CompareFunction stencil_back_compare_op : 3;  // 9
    uint32_t sample_rate_shading : 1;                    // 10
    // Hybrid occlusion query draw (FBO + shader counting for Total).
    // Selects counting the depth-only fragment shader
    // when there's no guest PS.
    uint32_t zpd_total : 1;  // 11
    // Survey draw for conditional rendering (FSI + occlusion_query_viz).
    // Selects the depth-only fragment shader. Surveys don't have a guest PS.
    uint32_t viz_survey : 1;  // 12

    // Filled only for the attachments present in the render pass object.
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    // Including all the padding, for a stable hash.
    static constexpr uint32_t kVersion = 0x20261013;
    PipelineDescription() {
      Reset();
    }
    PipelineDescription(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() {
      std::memset(this, 0, sizeof(*this));
    }
    uint64_t GetHash() const {
      return XXH3_64bits(this, sizeof(*this));
    }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return size_t(description.GetHash());
      }
    };
  });
  // Pipeline storage constants.
  // 'VLKN' as a multicharacter literal (as in canary).
  static constexpr uint32_t kPipelineStorageAPIMagicVulkan = 0x564C4B4E;

  // Pipeline storage description.
  REXPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  // Description that can be passed from the command processor thread to the
  // creation threads, with everything needed from caches pre-looked-up.
  struct PipelineCreationArguments {
    uint8_t priority = 0;
    std::pair<const PipelineDescription, Pipeline>* pipeline = nullptr;
    const PipelineLayoutProvider* pipeline_layout = nullptr;
    // Guest shader translation (VS or TES depending on host vertex type).
    const VulkanShader::VulkanTranslation* vertex_shader = nullptr;
    const VulkanShader::VulkanTranslation* pixel_shader = nullptr;
    // Non-guest stages for tessellation.
    VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
    VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
    uint32_t tessellation_patch_control_points = 0;
    VkShaderModule geometry_shader = VK_NULL_HANDLE;
    // VK_NULL_HANDLE when dynamic rendering is used.
    VkRenderPass render_pass = VK_NULL_HANDLE;
  };
  struct PipelineCreationArgumentsPriorityComparator {
    bool operator()(const PipelineCreationArguments& a, const PipelineCreationArguments& b) const {
      return a.priority < b.priority;
    }
  };

  union TessellationControlShaderKey {
    uint32_t key;
    struct {
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
      xenos::TessellationMode tessellation_mode : 2;
    };

    TessellationControlShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TessellationControlShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TessellationControlShaderKey& other_key) const {
      return key == other_key.key;
    }
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      uint32_t interpolator_count : 5;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
      // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
      uint32_t point_ps_ucp_mode : 2;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                           uint32_t dword_count, uint64_t data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation);

  // Translates shaders in parallel for storage loading.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed, bool edram_fsi_used);
  // Waits until the creation threads have created every queued pipeline.
  void AwaitPipelineCreation();

  void WritePipelineRenderTargetDescription(reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
                                            PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key, bool zpd_total, bool viz_survey,
      PipelineDescription& description_out) const;

  // Whether the pipeline for the given description is supported by the device.
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   SpirvShaderTranslator::Modification vertex_shader_modification,
                                   SpirvShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  static uint32_t GetTessellationPatchControlPointCount(
      Shader::HostVertexShaderType host_vertex_shader_type,
      xenos::TessellationMode tessellation_mode);
  VkShaderModule GetTessellationVertexShader(bool adaptive);
  VkShaderModule GetTessellationControlShader(Shader::HostVertexShaderType host_vertex_shader_type,
                                              xenos::TessellationMode tessellation_mode);
  VkShaderModule GetGeometryShader(GeometryShaderKey key);
  bool TryGetPipelineCreationArgumentsForDescription(
      const PipelineDescription& description,
      std::pair<const PipelineDescription, Pipeline>* pipeline,
      PipelineCreationArguments& creation_arguments);

  // Can be called from creation threads - all needed data must be fully set up
  // at the point of the call: shaders must be translated, and the pipeline
  // layout and render pass object (unless dynamic rendering is used) must be
  // available.
  bool EnsurePipelineCreated(const PipelineCreationArguments& creation_arguments);
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;
  bool zpd_hybrid_supported_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  string::StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator on the command processor thread.
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, VulkanShader*, rex::IdentityHasher<uint64_t>> shaders_;

  // Geometry shaders for Xenos primitive types not supported by Vulkan.
  // Stores VK_NULL_HANDLE if failed to create.
  std::unordered_map<GeometryShaderKey, VkShaderModule, GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Fixed-function emulation shaders for tessellation.
  bool tessellation_indexed_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_indexed_vertex_shader_ = VK_NULL_HANDLE;
  bool tessellation_adaptive_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_adaptive_vertex_shader_ = VK_NULL_HANDLE;
  std::unordered_map<TessellationControlShaderKey, VkShaderModule,
                     TessellationControlShaderKey::Hasher>
      tessellation_control_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer using fragment
  // shader interlock when no Xenos pixel shader provided, and for keeping FBO
  // draws that write nothing rasterized for occlusion queries.
  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;
  // Depth-only shaders for float24 emulation when no Xenos pixel shader is
  // provided in host render target mode.
  VkShaderModule depth_float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule depth_float24_round_fragment_shader_ = VK_NULL_HANDLE;

  VkShaderModule zpd_total_depth_only_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule zpd_total_float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule zpd_total_float24_round_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule viz_survey_depth_only_fragment_shader_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher> pipelines_;

  // Previously used pipeline, to avoid lookups if the state wasn't changed.
  std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;

  // Shader and pipeline storage.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // Pipeline cache of the driver, persisted between runs.
  VkPipelineCache vk_pipeline_cache_ = VK_NULL_HANDLE;
  // VkPipelineCache persistence path.
  std::filesystem::path vk_pipeline_cache_path_;

  mutable std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  std::priority_queue<PipelineCreationArguments, std::vector<PipelineCreationArguments>,
                      PipelineCreationArgumentsPriorityComparator>
      creation_queue_;
  size_t creation_threads_busy_ = 0;
  // During startup loading, don't block on pipeline creation to allow game
  // boot.
  bool startup_loading_ = false;
  std::unique_ptr<rex::thread::Event> creation_completion_event_;
  bool creation_completion_set_event_ = false;
  // Callback to invoke when all queued pipelines are created (for non-blocking
  // initialization). Protected with creation_request_lock_.
  std::function<void()> creation_completion_callback_;
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads_;
};

}  // namespace rex::graphics::vulkan
