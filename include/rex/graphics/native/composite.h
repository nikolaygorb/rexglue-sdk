/**
 * @file        graphics/native/composite.h
 * @brief       Native renderer: textures the game rendered, sampled at the resolution scale
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/native/scale.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class RenderTargets;
class Resolver;
class Submitter;
struct ResolvedTexture;

// A texture the game samples that resolves have written into, at the
// resolution scale (milestone N6b). Built from the texture cache's 1x image
// of it (always what guest memory holds: the base, its texels repeated) with
// the resolves' scaled images drawn over it where they are (newest last), so
// a resolve that can't be placed costs sharpness there, never the picture.
// Kept while the same images with the same writes make it up, so it is
// redrawn only after a resolve into it. Command processor thread only.
class Composites {
 public:
  Composites() = default;
  Composites(const Composites&) = delete;
  Composites& operator=(const Composites&) = delete;
  ~Composites();

  bool Initialize(Submitter& submitter, RenderTargets& render_targets);
  // The caller has waited for the GPU.
  void Shutdown();

  // Whether a composite can be in `format` (drawn into and sampled).
  bool CanCompose(VkFormat format) const;

  struct Key {
    // Physical address, guest texels.
    uint32_t base = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    friend bool operator==(const Key&, const Key&) = default;
  };
  // A resolve's image drawn over the base: `source` (guest pixels, in the
  // image) to the composite's (dest_x, dest_y).
  struct Piece {
    ResolvedTexture* texture = nullptr;
    Rect source;
    int32_t dest_x = 0;
    int32_t dest_y = 0;
  };
  // The composite of `key` at scale4, as a 2D array view with host_swizzle
  // (TextureCache's binding host_swizzle; the identity without view
  // swizzles on the device), in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
  // base_view: the texture cache's image of it with the identity swizzle,
  // in that layout; VK_NULL_HANDLE when the pieces cover all of it. Records
  // into the submitter's open command buffer, outside the draws' render pass,
  // when it has to be drawn. VK_NULL_HANDLE on failure.
  VkImageView Get(const Key& key, uint32_t scale4, VkImageView base_view,
                  const std::vector<Piece>& pieces, Resolver& resolver, uint32_t host_swizzle,
                  uint64_t frame);

  // Composites unused for a while are destroyed (after the GPU is done).
  void EndFrame(uint64_t frame);

 private:
  struct KeyHasher {
    size_t operator()(const Key& key) const;
  };
  struct Composite {
    uint32_t scale4 = kScale4Min;
    uint32_t host_width = 0;
    uint32_t host_height = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImageView> sampled_views;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // What it was last drawn from: the base and (image id, serial) per piece
    // with its placement.
    VkImageView drawn_base = VK_NULL_HANDLE;
    std::vector<uint64_t> drawn_signature;
    bool drawn = false;
    uint64_t last_drawn_frame = 0;
    uint64_t last_frame = 0;
  };
  bool Create(const Key& key, uint32_t scale4, Composite& composite);
  void DestroyLater(Composite& composite);
  VkImageView GetSampledView(const Key& key, Composite& composite, uint32_t host_swizzle);
  VkRenderPass GetRenderPass(VkFormat format);
  VkPipeline GetPipeline(VkFormat format);
  // A transient set for one source view (the sampler is immutable).
  VkDescriptorSet AllocateSourceSet(VkImageView view);
  void LogOnce(uint32_t reason_bit, const char* message);

  Submitter* submitter_ = nullptr;
  RenderTargets* render_targets_ = nullptr;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  VkShaderModule pixel_shader_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::unordered_map<VkFormat, VkRenderPass> render_passes_;
  std::unordered_map<VkFormat, VkPipeline> pipelines_;
  // Descriptor pools: the one being filled, those of submissions in flight
  // (with the last submission using them), free ones.
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  uint32_t pool_sets_left_ = 0;
  uint64_t pool_submission_ = 0;
  std::deque<std::pair<uint64_t, VkDescriptorPool>> pools_in_flight_;
  std::vector<VkDescriptorPool> pools_free_;
  std::unordered_map<Key, Composite, KeyHasher> composites_;
  uint32_t logged_ = 0;
};

}  // namespace rex::graphics::native
