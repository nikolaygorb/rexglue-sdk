/**
 * @file        graphics/native/resolve_memory.h
 * @brief       Native renderer: resolves written into guest memory (the shared memory)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The SDK's resolve copy shaders and their use in VulkanRenderTargetCache::Resolve
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license); the EDRAM layout of
 *              shaders/edram.xesli (XeEdramOffsetBytes).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/scale.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace rex::graphics::native {

class NativeSharedMemory;
class RenderTargets;
class Submitter;
struct RenderTarget;

// What xenos does for every resolve: the guest's texture bytes in memory. The
// source render target's samples over the resolve's EDRAM tiles are stored
// into an EDRAM buffer in the SDK's layout (raw 32/64-bit cells), then the
// SDK's precompiled resolve copy shaders (draw_util::ResolveInfo::GetCopyShader)
// write the destination into the shared memory: tiled, in its format and
// endianness, with the exponent bias, sample selection and k_DXT3A_AS_1_1_1_1.
// Textures loaded from that memory (other formats than the resolve's, offsets
// into it) and the CPU (readback) then see what the GPU would have written.
// At a resolution scale, the host pixel at each guest pixel's centre.
class ResolveMemory {
 public:
  ResolveMemory() = default;
  ResolveMemory(const ResolveMemory&) = delete;
  ResolveMemory& operator=(const ResolveMemory&) = delete;
  ~ResolveMemory();

  bool Initialize(Submitter& submitter, RenderTargets& render_targets,
                  NativeSharedMemory& shared_memory);
  // The caller has waited for the GPU.
  void Shutdown();

  // The copy of the current resolve (the registers) from `source`, which owns
  // its tiles (claimed), into the shared memory: outside the draws' render
  // pass, before the resolve's clears. False if nothing was written (an empty
  // or unsupported copy); written_* the range written then.
  bool Resolve(const RegisterFile& regs, const memory::Memory& memory, RenderTarget& source,
               uint32_t& written_start_out, uint32_t& written_length_out);

 private:
  struct Source {
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageView stencil_view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  const Source* GetSource(RenderTarget& source);
  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
  // The resolve's EDRAM tiles (span_base + row * span_pitch + column) as
  // `target` holds them into the EDRAM buffer: the samples in `sample_mask`
  // (a bit per sample index of the resolve's MSAA, view_msaa).
  bool Dump(RenderTarget& target, uint32_t span_base, uint32_t span_pitch, uint32_t span_columns,
            uint32_t span_rows, xenos::MsaaSamples view_msaa, uint32_t sample_mask);
  void ComputeBarrier(CommandRecorder* command_buffer);
  void LogOnce(uint32_t reason_bit, const char* message);

  Submitter* submitter_ = nullptr;
  RenderTargets* render_targets_ = nullptr;
  NativeSharedMemory* shared_memory_ = nullptr;

  // 2048 tiles of 80x16 32-bit samples.
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory edram_memory_ = VK_NULL_HANDLE;

  // Storage buffer, compute.
  VkDescriptorSetLayout storage_set_layout_ = VK_NULL_HANDLE;
  // Two sampled images, compute (colour, or depth and stencil).
  VkDescriptorSetLayout source_set_layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> pools_;
  uint32_t pool_sets_left_ = 0;
  VkDescriptorSet edram_set_ = VK_NULL_HANDLE;
  // The whole shared memory, when maxStorageBufferRange allows it; else a set
  // per resolve (freed after its submission).
  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;
  VkDescriptorPool transient_pool_ = VK_NULL_HANDLE;

  // The dump: set 0 the source, set 1 the EDRAM buffer.
  VkPipelineLayout dump_layout_ = VK_NULL_HANDLE;
  // By src depth << 1 | src multisampled.
  std::array<VkPipeline, 4> dump_pipelines_{};
  // The SDK's copy shaders: set 0 the EDRAM buffer, set 1 the destination.
  VkPipelineLayout copy_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)> copy_pipelines_{};

  std::unordered_map<const RenderTarget*, Source> sources_;
  uint32_t logged_ = 0;
};

}  // namespace rex::graphics::native
