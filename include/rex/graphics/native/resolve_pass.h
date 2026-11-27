/**
 * @file        graphics/native/resolve_pass.h
 * @brief       Native renderer: resolves that convert, drawn into the destination
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              kResolveShader, InitResolvePipelines, ResolveWithShader), Copyright (c) 2026
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

#include <rex/graphics/native/scale.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class RenderTargets;
class Submitter;
struct RenderTarget;
struct ResolvedTexture;

// The resolves a copy or blit can't do: a fullscreen triangle into the
// destination, scissored to the copied rectangle, reading the source pixel at
// the same place (AoT's resolve pass): colour scaled (copy_dest_exp_bias, and
// x32 for 16-bit fixed render targets, stored /32), multisampled sources
// averaged over the selected samples (copy_sample_select), depth written as
// its float value (the destination R32_SFLOAT, as the SDK's texture cache
// holds k_24_8 textures; one sample). Into any colour-renderable destination
// format. The shader is compiled at Initialize.
class ResolvePass {
 public:
  ResolvePass() = default;
  ResolvePass(const ResolvePass&) = delete;
  ResolvePass& operator=(const ResolvePass&) = delete;
  ~ResolvePass();

  bool Initialize(Submitter& submitter, RenderTargets& render_targets);
  // The caller has waited for the GPU.
  void Shutdown();

  // Whether `format` can be drawn into (the destination needs
  // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT then).
  bool CanDrawInto(VkFormat format) const;

  // Into the submitter's open command buffer, outside the draws' render pass;
  // false if nothing was recorded (the caller falls back to its copy).
  // `host_rect`: host pixels of the destination; the source is read at the
  // same pixels moved by (source_x, source_y) (a scene viewport off the origin).
  bool Draw(RenderTarget& src, ResolvedTexture& dest, const Rect& host_rect, float scale,
            uint32_t sample_mask, int32_t source_x = 0, int32_t source_y = 0);

 private:
  struct Source {
    // Depth sources: a depth-aspect view; colour ones use the target's view.
    VkImageView depth_view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  const Source* GetSource(RenderTarget& src);
  VkRenderPass GetRenderPass(VkFormat format);
  VkPipeline GetPipeline(bool src_depth, bool src_multisampled, VkFormat dest_format);
  VkFramebuffer GetFramebuffer(ResolvedTexture& dest, VkRenderPass render_pass);
  void LogOnce(uint32_t reason_bit, const char* message);

  Submitter* submitter_ = nullptr;
  RenderTargets* render_targets_ = nullptr;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  // By src depth << 1 | src multisampled.
  std::array<VkShaderModule, 4> pixel_shaders_{};
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> pools_;
  uint32_t pool_sets_left_ = 0;
  std::unordered_map<const RenderTarget*, Source> sources_;
  std::unordered_map<VkFormat, VkRenderPass> render_passes_;
  // By the destination VkFormat | src depth << 32 | src multisampled << 33;
  // VK_NULL_HANDLE for a failed one.
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  uint32_t logged_ = 0;
};

}  // namespace rex::graphics::native
