/**
 * @file        graphics/native/present.h
 * @brief       Native renderer: the front buffer into the presenter's guest output
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The guest output pass follows ArmyOfTwoRecompilation
 *              (aot/src/gpu/native/renderer.cpp, RendererSwap), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/registers.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/presenter.h>

namespace rex::graphics::native {

class Submitter;

// The swap: the host texture the guest resolved its front buffer into, with
// the guest's display gamma ramp applied (the 256-entry table, or the PWL ramp
// for 10-bit front buffers), written pixel for pixel into the presenter's guest
// output image; the presenter scales and letterboxes it into the window. AoT
// sampled and letterboxed into its own swap chain instead.
// swap_post_effect as the SDK's Vulkan backend does it: the gamma pass draws
// into an intermediate image (FXAA: R16G16B16A16 with the luma in .a, by the
// apply_gamma_*_fxaa_luma shaders; SMAA: A2B10G10R10), then compute passes
// write the guest output (FXAA or extreme FXAA: one, the shared GLSL; SMAA:
// edge detection, blending weights, neighborhood blending, the SDK's shaders).
class Present {
 public:
  enum class PostEffect { kNone, kFxaa, kFxaaExtreme, kSmaa };

  struct Source {
    // SHADER_READ_ONLY_OPTIMAL, readable by the fragment shader; colour in
    // .rgb (0..1). VK_NULL_HANDLE: a black frame.
    VkImageView view = VK_NULL_HANDLE;
    // Host pixels to present, from the top left of the image.
    uint32_t width = 0;
    uint32_t height = 0;
    // The PWL ramp (2_10_10_10 front buffers) rather than the 256-entry table.
    bool pwl_gamma_ramp = false;
  };

  Present() = default;
  Present(const Present&) = delete;
  Present& operator=(const Present&) = delete;
  ~Present();

  bool Initialize(Submitter& submitter);
  void Shutdown();

  // Records the pass into the submitter's open command buffer (the caller
  // submits it before the refresher returns). The ramps are the command
  // processor's composed ones (gamma_ramp_256_entry_table / _pwl_rgb).
  // effect: swap_post_effect (without its resources, the gamma pass alone,
  // logged once).
  bool Record(ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext& context,
              uint32_t output_width, uint32_t output_height, const Source& source,
              const reg::DC_LUT_30_COLOR* ramp_table, const reg::DC_LUT_PWL_DATA* ramp_pwl,
              PostEffect effect);

 private:
  // A device-local image of an effect, its view and the framebuffer of the
  // gamma pass drawing into it (VK_NULL_HANDLE for the others).
  struct EffectImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  enum EffectImageIndex : size_t {
    // FXAA's source: the gamma-corrected image, luma in .a.
    kEffectImageFxaaSource,
    // SMAA's: the gamma-corrected colour, edges, blending weights.
    kEffectImageSmaaColor,
    kEffectImageSmaaEdges,
    kEffectImageSmaaWeights,
    // SMAA's lookup textures, uploaded at their first use.
    kEffectImageSmaaArea,
    kEffectImageSmaaSearch,
    kEffectImageCount,
  };
  static constexpr uint32_t kSmaaPassCount = 3;
  static constexpr uint32_t kSmaaSourceCount = 3;
  struct EffectConstants {
    uint32_t size[2];
    float size_inv[2];
  };

  // Compute pipelines and their layouts; false if any is missing (the
  // effects are then off, logged).
  bool InitializeEffects();
  void ShutdownEffects();
  // An intermediate image of the output size (recreated on a size change,
  // the old one destroyed after the frames using it), with the framebuffer
  // of gamma_render_pass when given.
  bool EnsureEffectImage(EffectImageIndex index, VkFormat format, VkImageUsageFlags usage,
                         uint32_t width, uint32_t height, VkRenderPass gamma_render_pass);
  void DestroyEffectImage(EffectImageIndex index, bool now);
  // The SMAA lookup textures, copied in the open command buffer once.
  bool EnsureSmaaLookupTextures(CommandRecorder* command_buffer);
  // The gamma pass into `framebuffer` (render_pass: render_pass_ or
  // fxaa_render_pass_), cleared to black, then the source drawn where it
  // reaches. luma_in_alpha: FXAA's source (black's luma 0 in .a).
  void RecordGammaPass(CommandRecorder* command_buffer, VkRenderPass render_pass,
                       VkFramebuffer framebuffer, VkPipeline pipeline, uint32_t output_width,
                       uint32_t output_height, const Source& source, uint32_t slot, uint32_t pwl,
                       bool luma_in_alpha);

  // Both ramps of one frame: the 256-entry table (A2B10G10R10 texels), then
  // the PWL ramp (R16G16_UINT texels: base, delta), each at a multiple of 256
  // bytes (the largest minTexelBufferOffsetAlignment).
  static constexpr uint32_t kRampTableSize = sizeof(uint32_t) * 256;
  static constexpr uint32_t kRampPwlSize = sizeof(uint16_t) * 2 * 3 * 128;
  static constexpr uint32_t kRampSlotSize = (kRampTableSize + kRampPwlSize + 255) & ~uint32_t(255);
  // Frames whose ramps / descriptors may be in use by the GPU at once.
  static constexpr uint32_t kSlotCount = 3;

  struct Framebuffer {
    uint64_t image_version = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  VkFramebuffer GetFramebuffer(
      const ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext& context,
      uint32_t width, uint32_t height);

  Submitter* submitter_ = nullptr;

  VkBuffer ramp_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory ramp_memory_ = VK_NULL_HANDLE;
  uint32_t ramp_memory_type_ = UINT32_MAX;
  VkDeviceSize ramp_memory_size_ = 0;
  uint8_t* ramp_mapping_ = nullptr;
  // [slot][0: table, 1: PWL].
  std::array<std::array<VkBufferView, 2>, kSlotCount> ramp_views_{};

  VkDescriptorSetLayout ramp_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout source_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<std::array<VkDescriptorSet, 2>, kSlotCount> ramp_sets_{};
  std::array<VkDescriptorSet, kSlotCount> source_sets_{};
  // The last submission that used each slot.
  std::array<uint64_t, kSlotCount> slot_submissions_{};
  uint32_t next_slot_ = 0;

  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  // [0: table, 1: PWL].
  std::array<VkPipeline, 2> pipelines_{};

  std::array<Framebuffer, ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};

  // swap_post_effect.
  bool effects_ready_ = false;
  uint32_t effects_logged_ = 0;
  // The gamma pass into the R16G16B16A16 FXAA source, and its pipelines
  // writing the luma into .a ([0: table, 1: PWL]).
  VkRenderPass fxaa_render_pass_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 2> fxaa_luma_pipelines_{};
  VkSampler linear_clamp_sampler_ = VK_NULL_HANDLE;
  // FXAA: set 0 the source (combined image sampler), set 1 the destination.
  VkDescriptorSetLayout combined_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout storage_set_layout_ = VK_NULL_HANDLE;
  // SMAA: set 0 the immutable linear clamp sampler, set 1 three sampled
  // images, set 2 the destination.
  VkDescriptorSetLayout smaa_sampler_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout smaa_sources_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout smaa_pipeline_layout_ = VK_NULL_HANDLE;
  // [0: FXAA, 1: extreme].
  std::array<VkPipeline, 2> fxaa_pipelines_{};
  std::array<VkPipeline, kSmaaPassCount> smaa_pipelines_{};
  VkDescriptorPool effect_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet smaa_sampler_set_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotCount> fxaa_source_sets_{};
  std::array<VkDescriptorSet, kSlotCount> fxaa_destination_sets_{};
  std::array<std::array<VkDescriptorSet, kSmaaPassCount>, kSlotCount> smaa_source_sets_{};
  std::array<std::array<VkDescriptorSet, kSmaaPassCount>, kSlotCount> smaa_destination_sets_{};
  std::array<EffectImage, kEffectImageCount> effect_images_{};
};

}  // namespace rex::graphics::native
