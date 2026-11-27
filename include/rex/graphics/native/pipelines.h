/**
 * @file        graphics/native/pipelines.h
 * @brief       Native renderer: descriptor set layouts, draw render passes and pipelines
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The layouts are the SDK's SPIR-V translator's; pipeline creation follows the
 *              SDK's VulkanPipelineCache::EnsurePipelineCreated for host render targets
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/graphics/native/draw_state.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class Submitter;

// Everything a draw's pipeline is made of, created on first use and kept
// until shutdown. Pipelines can be created by background threads
// (async_shader_compilation, as the SDK's backends): the draw that needs one
// still being created waits up to a given time, then is skipped. Everything
// else is on the command processor thread. (A pipeline storage is milestone
// N7.)
class Pipelines {
 public:
  Pipelines() = default;
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;
  ~Pipelines();

  // Before Initialize: set 0 also has the vertex shaders' object motion
  // capture buffer (SpirvShaderTranslator::kMotionCaptureBinding).
  void SetMotionCaptureBinding(bool enabled) { motion_capture_binding_ = enabled; }
  bool motion_capture_binding() const { return motion_capture_binding_; }
  bool Initialize(Submitter& submitter);
  // The caller has waited for the GPU.
  void Shutdown();

  // The translator's sets: 0 the shared memory (shared_memory_binding_count
  // storage buffers), 1 the five constant buffers (dynamic), 2 and 3 the
  // vertex and pixel shader textures with their samplers after them.
  uint32_t shared_memory_binding_count() const { return shared_memory_binding_count_; }
  VkDescriptorSetLayout shared_memory_set_layout() const { return shared_memory_set_layout_; }
  VkDescriptorSetLayout constants_set_layout() const { return constants_set_layout_; }
  VkDescriptorSetLayout empty_set_layout() const { return empty_set_layout_; }
  // The empty layout without bindings. A push descriptor layout for the pixel
  // shader's where PixelTexturesPushed.
  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t texture_count,
                                            uint32_t sampler_count);
  // The pixel shader's textures and samplers are pushed into the command
  // buffer (VK_KHR_push_descriptor, native_push_descriptors) rather than
  // written into a descriptor set: when there are any and the device takes
  // that many. Only one set of a pipeline layout may be pushed.
  bool PixelTexturesPushed(uint32_t texture_count, uint32_t sampler_count) const {
    return push_descriptors_ && (texture_count || sampler_count) &&
           texture_count + sampler_count <= max_push_descriptors_;
  }

  struct Layout {
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_pixel = VK_NULL_HANDLE;
    // textures_pixel is a push descriptor layout (PixelTexturesPushed).
    bool textures_pixel_pushed = false;
  };
  // nullptr on failure.
  const Layout* GetLayout(uint32_t vertex_textures, uint32_t vertex_samplers,
                          uint32_t pixel_textures, uint32_t pixel_samplers);

  // A draw's attachments: colour formats by render target index
  // (VK_FORMAT_UNDEFINED when not attached), the depth format (or undefined).
  struct RenderPassKey {
    VkFormat color[xenos::kMaxColorRenderTargets] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  };
  // Attachments in the order colour (by render target index), then depth;
  // loaded and stored in their attachment layouts; none for draws only
  // exporting to memory. VK_NULL_HANDLE on failure.
  VkRenderPass GetRenderPass(const RenderPassKey& key);

  // VK_NULL_HANDLE if it can't be created (logged once per pipeline, not
  // retried) or, with pending_out set, while a background thread creates it.
  // pixel_shader may be VK_NULL_HANDLE (depth only). allow_async: a new
  // pipeline goes to the background threads (else it is created here, and one
  // already there is waited for); wait_ms: how long to wait for one being
  // created before giving up for this draw.
  // Tessellated: tessellation_vertex_shader and tessellation_control_shader
  // (GetTessellation*Shader) run before vertex_shader, the guest's translated
  // as the evaluation shader; VK_NULL_HANDLE both otherwise.
  VkPipeline GetPipeline(VkShaderModule vertex_shader, VkShaderModule pixel_shader,
                         VkShaderModule tessellation_vertex_shader,
                         VkShaderModule tessellation_control_shader,
                         const RenderPassKey& render_pass_key, const Layout& layout,
                         const PipelineState& state, bool allow_async, uint32_t wait_ms,
                         bool& pending_out, bool* requested_first_out = nullptr);

  // Until no pipeline is being created or waits to be.
  void AwaitIdle();
  // Whether none is, now.
  bool IsIdle();
  // The driver's pipeline cache: its data, and a new one from stored data
  // (once no pipeline is being created; what the old one held is dropped).
  bool GetPipelineCacheData(std::vector<uint8_t>& data_out);
  void ReplacePipelineCache(const std::vector<uint8_t>& data);

  // Tessellation (the device's tessellationShader): the host stages before the
  // guest's vertex shader (vulkan/tessellation_shaders.h), compiled on first
  // use and kept; VK_NULL_HANDLE if not supported or on failure.
  bool tessellation_supported() const { return tessellation_supported_; }
  VkShaderModule GetTessellationVertexShader(bool adaptive);
  VkShaderModule GetTessellationControlShader(Shader::HostVertexShaderType host_vertex_shader_type,
                                              xenos::TessellationMode tessellation_mode);

  // Requested so far (created, being created or failed).
  uint32_t pipeline_count() const { return uint32_t(pipelines_.size()); }

  // The device's abilities for the draw state, filled at Initialize.
  const DrawStateCaps& caps() const { return caps_; }

 private:
  struct PipelineKey {
    VkShaderModule vertex_shader;
    VkShaderModule pixel_shader;
    VkShaderModule tessellation_vertex_shader;
    VkShaderModule tessellation_control_shader;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    PipelineState state;
    // Explicit, so the key has no implicit padding to compare or hash.
    uint8_t padding[8 - sizeof(PipelineState) % 8];
  };
  struct KeyHasher {
    size_t operator()(const PipelineKey& key) const;
  };
  struct KeyEqual {
    bool operator()(const PipelineKey& a, const PipelineKey& b) const;
  };
  // Thread-safe: reads only the key, the device and the pipeline cache (which
  // Vulkan synchronizes internally).
  VkPipeline CreatePipeline(const PipelineKey& key, const RenderPassKey& render_pass_key) const;

  // A pipeline, filled once: by the command processor thread or a worker.
  struct Entry {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<bool> done{false};
  };
  struct Job {
    PipelineKey key;
    RenderPassKey render_pass_key;
    Entry* entry;
  };
  void WorkerMain();
  void StopWorkers();

  Submitter* submitter_ = nullptr;
  uint32_t shared_memory_binding_count_ = 1;
  bool motion_capture_binding_ = false;
  // The stages the guest's vertex shader runs in (also as the tessellation
  // evaluation shader where the device can), and every guest-facing stage.
  bool tessellation_supported_ = false;
  // PixelTexturesPushed.
  bool push_descriptors_ = false;
  uint32_t max_push_descriptors_ = 0;
  VkShaderStageFlags guest_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  VkShaderStageFlags guest_stages_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Tessellation: the host vertex shaders (indexed, adaptive) and the
  // control shaders by host vertex shader type << 2 | tessellation mode.
  bool tessellation_vertex_shader_attempted_[2] = {};
  VkShaderModule tessellation_vertex_shaders_[2] = {};
  std::unordered_map<uint32_t, VkShaderModule> tessellation_control_shaders_;
  VkDescriptorSetLayout empty_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout shared_memory_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout constants_set_layout_ = VK_NULL_HANDLE;
  // By texture count | sampler count << 16 | vertex << 32.
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_set_layouts_;
  // By the four counts, 16 bits each.
  std::unordered_map<uint64_t, std::unique_ptr<Layout>> layouts_;
  // By a hash of the key, collisions checked on the key.
  std::unordered_multimap<uint64_t, std::pair<RenderPassKey, VkRenderPass>> render_passes_;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  // The map on the command processor thread only; the entries are written by
  // whoever creates them.
  std::unordered_map<PipelineKey, std::unique_ptr<Entry>, KeyHasher, KeyEqual> pipelines_;

  std::vector<std::thread> workers_;
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_;
  std::deque<Job> jobs_;
  // Jobs taken by a worker and not done yet; idle_cv_ notified when a worker
  // finishes one.
  uint32_t jobs_running_ = 0;
  std::condition_variable idle_cv_;
  bool workers_exit_ = false;
  // Notified whenever a worker finishes one.
  std::mutex done_mutex_;
  std::condition_variable done_cv_;
  DrawStateCaps caps_;
};

}  // namespace rex::graphics::native
