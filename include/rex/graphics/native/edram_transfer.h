/**
 * @file        graphics/native/edram_transfer.h
 * @brief       Native renderer: EDRAM contents carried between host render targets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              InitTransferPipelines, GetTransferPipeline, TransferTiles), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
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
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class RenderTargets;
class Submitter;
struct RenderTarget;

// The kTransfer ops of EDRAM claims (EdramOwnership): a pass over the
// destination's tile rectangles reading the source's samples at the same
// EDRAM place (kEdramTransferShaderBody); depth through the fragment depth.
// Stencil: from the same pass with VK_EXT_shader_stencil_export
// (native_transfer_stencil_export), else bit by bit; then, with
// VK_EXT_conditional_rendering, a compute pass first finds which stencil bits
// a depth source has set in the rows the transfer reads, and only those bits'
// draws run (native_transfer_stencil_bit_mask; ReIGlue 0461). A multisampled
// destination: all samples in one pass with sample rate shading
// (native_transfer_sample_shading), else once per sample (gl_SampleMask).
// Both one-pass forms are what xenos does; on an Intel UHD 770 the per-bit
// and per-sample passes made RDR's transfers cost ~70 ms a frame.
class EdramTransfers {
 public:
  EdramTransfers() = default;
  EdramTransfers(const EdramTransfers&) = delete;
  EdramTransfers& operator=(const EdramTransfers&) = delete;
  ~EdramTransfers();

  bool Initialize(Submitter& submitter, RenderTargets& render_targets);
  // The caller has waited for the GPU.
  void Shutdown();

  // Into the submitter's open command buffer, outside the draws' render pass.
  // `parts`: EdramPart (a colour destination takes the whole cell); `rects`
  // in the destination's guest pixels.
  void Transfer(RenderTarget& dst, RenderTarget& src, uint32_t parts,
                const std::vector<Rect>& rects);

 private:
  struct Source {
    // Depth sources: one view per aspect; colour ones use the target's view.
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageView stencil_view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  // The descriptor set reading the target, created on first use (the targets
  // live until shutdown).
  const Source* GetSource(RenderTarget& src);
  // The pixel shader of stencil bits by sample mask << 6 | stencil export << 5
  // | sample shading << 4 | src depth << 3 | dst depth << 2 | src
  // multisampled << 1 | dst multisampled, compiled on first use
  // (VK_NULL_HANDLE if it fails).
  VkShaderModule GetPixelShader(uint32_t variant);
  enum class PassKind : uint32_t {
    // Colour, or depth only.
    kMain,
    // One stencil bit (dynamic write mask).
    kStencilBit,
    // Depth and the stencil from the shader.
    kDepthStencilExport,
    // The stencil from the shader, depth kept.
    kStencilExport,
  };
  // stencil_sample_mask: a kStencilBit pass into a multisampled destination
  // once per pixel, its samples by the sample mask (no sample shading).
  VkPipeline GetPipeline(const RenderTarget& src, const RenderTarget& dst, PassKind kind,
                         bool sample_shading, bool stencil_sample_mask = false);
  void LogOnce(uint32_t reason_bit, const char* message);

  // Stencil bit masks: 8 words a slot (one a bit, nonzero where the bit is set
  // in the source), in kMaskRegions regions of kMaskSlotsPerRegion, one region
  // a submission (by its number), reused once that submission is complete.
  static constexpr uint32_t kMaskRegions = 8;
  static constexpr uint32_t kMaskSlotsPerRegion = 64;
  // False (and nothing created) without conditional rendering or on failure.
  bool InitializeStencilMask();
  void ShutdownStencilMask();
  // A slot for the current submission, or UINT32_MAX.
  uint32_t AllocateMaskSlot();
  // Records the mask of the stencil bits `src` has in the rows the transfer
  // into `rects` (destination guest pixels) reads; outside a render pass.
  // Returns its slot, or UINT32_MAX (then every bit is drawn).
  uint32_t RecordStencilMask(CommandRecorder* command_buffer, const RenderTarget& dst,
                             const RenderTarget& src, const Source& source,
                             const std::vector<Rect>& rects);

  Submitter* submitter_ = nullptr;
  RenderTargets* render_targets_ = nullptr;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, 128> pixel_shaders_{};
  std::array<bool, 128> pixel_shaders_tried_{};
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> pools_;
  uint32_t pool_sets_left_ = 0;
  std::unordered_map<const RenderTarget*, Source> sources_;
  // By the destination VkFormat | pixel shader variant << 32 | PassKind << 39
  // | destination samples << 41; VK_NULL_HANDLE for a failed one.
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  uint32_t logged_ = 0;

  VkBuffer mask_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory mask_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout mask_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool mask_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet mask_set_ = VK_NULL_HANDLE;
  VkPipelineLayout mask_pipeline_layout_ = VK_NULL_HANDLE;
  // Single-sampled and multisampled sources.
  std::array<VkPipeline, 2> mask_pipelines_{};
  std::array<uint64_t, kMaskRegions> mask_region_submission_{};
  uint64_t mask_submission_ = UINT64_MAX;
  uint32_t mask_next_slot_ = 0;
  bool mask_region_usable_ = false;
};

}  // namespace rex::graphics::native
