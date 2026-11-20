/**
 * @file        graphics/native/bindings.h
 * @brief       Native renderer: what the translated shaders read (constant buffers, shared
 *              memory, textures), as descriptor sets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The constant buffers follow the SDK's VulkanCommandProcessor::UpdateBindings
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/pipelines.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::native {

class Submitter;

// The views and samplers of a draw's textures, in the order of the shaders'
// bindings (SpirvShader::GetTextureBindingsAfterTranslation and
// GetSamplerBindingsAfterTranslation); VK_NULL_HANDLE where the placeholder
// goes.
struct DrawTextures {
  std::vector<VkImageView> vertex_views;
  std::vector<VkSampler> vertex_samplers;
  std::vector<VkImageView> pixel_views;
  std::vector<VkSampler> pixel_samplers;
  void Clear() {
    vertex_views.clear();
    vertex_samplers.clear();
    pixel_views.clear();
    pixel_samplers.clear();
  }
};

// The four descriptor sets of a draw:
// 0 - the shared memory, written once;
// 1 - the system, float (vertex, pixel), bool / loop and fetch constants, as
//     dynamic uniform buffers in an upload pool (the set is written again
//     only when a buffer or a size changes, the offsets go with the bind);
//     a buffer is written again only when its values may have changed
//     (ConstantRegisterWritten, other system constants, another float
//     constant map) or a new submission began;
// 2, 3 - the vertex and pixel shader textures and samplers (DrawTextures), a
//     transient set per stage, reused while its contents stay the same; the
//     placeholder for a missing texture is transparent black, as the SDK's
//     for invalid fetches, the placeholder sampler linear. The pixel
//     shader's are pushed into the command buffer instead where the layout
//     is a push one (Pipelines::PixelTexturesPushed).
class Bindings {
 public:
  Bindings() = default;
  Bindings(const Bindings&) = delete;
  Bindings& operator=(const Bindings&) = delete;
  ~Bindings();

  bool Initialize(Submitter& submitter, Pipelines& pipelines, VkBuffer shared_memory_buffer,
                  uint32_t shared_memory_size);
  // The caller has waited for the GPU.
  void Shutdown();

  // The sets of the frame retire with its last submission.
  void EndFrame(uint64_t last_submission);

  // frame_stats_interval: texture sets written, and found already written in
  // the frame, since the last call.
  // Disjoint CPU phases of sampled Bind calls (not GPU timings).
  enum class BindPhase {
    kUniforms, kConstants, kConstantAllocate, kConstantUpdate, kTextureSets,
    kBindPrepare, kBindApi, kPushPrepare, kPushApi, kCount
  };
  struct Stats {
    uint64_t bind_profile_calls = 0, bind_profile_samples = 0;
    uint64_t bind_phase_ticks[size_t(BindPhase::kCount)] = {};
    uint64_t texture_sets_written = 0;
    uint64_t texture_sets_found = 0;
    // Pixel shader textures pushed (Pipelines::PixelTexturesPushed).
    uint64_t texture_sets_pushed = 0;
    uint64_t uniform_requests = 0, uniform_bytes = 0;
    uint64_t constants_sets_written = 0, descriptor_binds = 0;
    uint64_t pushes_invalid = 0, pushes_layout = 0, pushes_content = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

  // A shader constant register written by the guest (the command
  // processor's WriteRegister): the buffers holding it are written again.
  void ConstantRegisterWritten(uint32_t index);
  // The same for the registers first...last, all in one constant block (float,
  // fetch, or bool and loop), as a range the guest wrote at once.
  void ConstantRegistersWritten(uint32_t first, uint32_t last);
  // The open command buffer's graphics descriptor sets may have been replaced
  // (a new command buffer, passes binding their own): all are bound again.
  void InvalidateBound() {
    bound_.valid = false;
    pushed_pixel_.valid = false;
  }

  // Writes the draw's constant buffers where changed and binds the sets that
  // differ from the bound ones, inside or outside the render pass. False on
  // failure (the draw is skipped).
  bool Bind(CommandRecorder* command_buffer, const Pipelines::Layout& layout,
            const RegisterFile& regs,
            const SpirvShaderTranslator::SystemConstants& system_constants,
            const vulkan::VulkanShader& vertex_shader, const vulkan::VulkanShader* pixel_shader,
            const DrawTextures& textures);

 private:
  // Descriptor pools that grow as needed.
  VkDescriptorSet AllocatePersistentSet(VkDescriptorSetLayout layout);
  VkDescriptorSet AllocateTransientSet(VkDescriptorSetLayout layout);
  VkDescriptorPool CreatePool(bool transient);
  bool CreatePlaceholderTextures();
  // The set of a stage's textures (VK_NULL_HANDLE on failure); the empty set
  // without bindings.
  VkDescriptorSet GetTextureSet(const vulkan::VulkanShader& shader, bool vertex,
                                const std::vector<VkImageView>& views,
                                const std::vector<VkSampler>& samplers);
  // image_infos_ for a stage's textures then samplers, the placeholders where
  // missing, and the writes of them (dst_set VK_NULL_HANDLE for a push).
  // Returns the write count.
  uint32_t WriteTextureInfos(const vulkan::VulkanShader& shader,
                             const std::vector<VkImageView>& views,
                             const std::vector<VkSampler>& samplers, VkDescriptorSet dst_set,
                             VkWriteDescriptorSet writes_out[2]);

  Submitter* submitter_ = nullptr;
  Pipelines* pipelines_ = nullptr;

  std::vector<VkDescriptorPool> persistent_pools_;
  // Transient: the ones the frame fills (the last one current), then retired
  // with the frame's last submission, reset and reused once it's done.
  std::vector<VkDescriptorPool> transient_pools_frame_;
  std::deque<std::pair<uint64_t, VkDescriptorPool>> transient_pools_retired_;
  std::vector<VkDescriptorPool> transient_pools_free_;

  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;
  VkDescriptorSet empty_set_ = VK_NULL_HANDLE;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;
  VkDescriptorBufferInfo constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferCount] = {};
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;
  bool profile_bind_enabled_ = false;
  uint32_t profile_bind_random_ = 0x6D2B79F5u;
  // The buffers written last: where (infos, dynamic offsets), in which
  // submission, and which still hold the current values (bits by
  // SpirvShaderTranslator::kConstantBuffer*). Their memory is kept by the
  // upload pool until that submission completes, so they are reused only in
  // it.
  VkDescriptorBufferInfo current_infos_[SpirvShaderTranslator::kConstantBufferCount] = {};
  uint32_t current_offsets_[SpirvShaderTranslator::kConstantBufferCount] = {};
  uint64_t constants_submission_ = 0;
  uint32_t constants_up_to_date_ = 0;
  SpirvShaderTranslator::SystemConstants current_system_constants_ = {};
  // The float constants packed into the vertex and pixel buffers.
  uint64_t current_float_map_vertex_[4] = {};
  uint64_t current_float_map_pixel_[4] = {};
  // What vkCmdBindDescriptorSets last bound in the open command buffer.
  struct BoundSets {
    bool valid = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount] = {};
    uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
  };
  BoundSets bound_;
  // What the open command buffer has pushed as the pixel shader textures
  // (Pipelines::PixelTexturesPushed), under which pipeline layout. Kept by
  // binds of the lower sets with that layout; another layout or a bind of
  // the pixel set itself replaces it.
  struct PushedTextures {
    bool valid = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
  };
  PushedTextures pushed_pixel_;

  // Placeholders: a cube-compatible 1x1 image of 6 layers (2D array and cube
  // views) and a 1x1x1 3D image.
  VkImage placeholder_2d_cube_ = VK_NULL_HANDLE;
  VkDeviceMemory placeholder_2d_cube_memory_ = VK_NULL_HANDLE;
  VkImage placeholder_3d_ = VK_NULL_HANDLE;
  VkDeviceMemory placeholder_3d_memory_ = VK_NULL_HANDLE;
  VkImageView placeholder_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView placeholder_view_cube_ = VK_NULL_HANDLE;
  VkImageView placeholder_view_3d_ = VK_NULL_HANDLE;
  VkSampler placeholder_sampler_ = VK_NULL_HANDLE;
  // The last texture set of each stage (vertex, pixel) and what it holds,
  // reused within the frame while the same.
  struct TextureSetMemo {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  TextureSetMemo texture_set_memos_[2];
  // Every texture set written in the frame, by a hash of its layout and
  // contents: a draw going back to textures an earlier one used (A, B, A)
  // takes that set instead of writing a new one. Cleared with the frame's
  // pools.
  std::unordered_multimap<uint64_t, TextureSetMemo> texture_sets_frame_;
  std::vector<VkDescriptorImageInfo> image_infos_;
  Stats stats_;
};

}  // namespace rex::graphics::native
