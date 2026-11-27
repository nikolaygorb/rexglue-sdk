/**
 * @file        graphics/native/render_targets.h
 * @brief       Native renderer: host render targets standing for EDRAM surfaces
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              RenderTarget, GetRenderTarget, ColorFormat .. DepthFormat), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/edram_ownership.h>
#include <rex/graphics/native/edram_tiles.h>
#include <rex/graphics/native/scale.h>
#include <rex/graphics/native/transfer_filter.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

class EdramTransfers;
class Submitter;

// EDRAM formats with the same bits share one host render target (the gamma
// format and the AS_ variants render through other views of it).
xenos::ColorRenderTargetFormat KeyColorFormat(xenos::ColorRenderTargetFormat format);
bool Is64bppColorFormat(xenos::ColorRenderTargetFormat format);

// One host image per EDRAM surface: base, pitch, key format, MSAA, colour or
// depth. Its size is the pitch by the rows to the end of the EDRAM
// (SurfaceHeightPixels), at scale4. EdramTarget: its surface (guest pixels,
// width = pitch), depth, format (KeyColorFormat / DepthRenderTargetFormat)
// and EDRAM ownership state.
struct RenderTarget : EdramTarget {
  // Monotonic per cache instance, including context recreation; not a lease.
  uint64_t id = 0;
  // Key (with EdramTarget::format and depth).
  uint32_t base_tiles = 0;
  uint32_t pitch = 0;
  xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;

  uint32_t scale4 = kScale4Min;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  // 16-bit fixed formats: values stored /32 (EDRAM range -32...32).
  bool fixed16 = false;

  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = 0;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  // All aspects (attachment, copies).
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // For clears: a framebuffer with only this attachment.
  VkFramebuffer framebuffer = VK_NULL_HANDLE;

  Rect guest_rect() const { return {0, 0, int32_t(surface.width), int32_t(surface.height)}; }
};

class RenderTargets {
 public:
  // Out of line: transfers_ is of a type complete only in the .cpp.
  RenderTargets();
  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;
  ~RenderTargets();

  // scale4: every surface's resolution scale in quarters (kScale4Min = 1x).
  bool Initialize(Submitter& submitter, uint32_t tile_count, uint32_t scale4 = kScale4Min);
  void Shutdown();

  uint32_t tile_count() const { return tile_count_; }
  uint32_t scale4() const { return scale4_; }

  // nullptr when the image can't be created. scale4 0: ScaleFor (the scene
  // policy below), else that scale's image of the surface.
  RenderTarget* GetColor(uint32_t base_tiles, xenos::ColorRenderTargetFormat format,
                         uint32_t pitch, xenos::MsaaSamples msaa, uint32_t scale4 = 0);
  RenderTarget* GetDepth(uint32_t base_tiles, xenos::DepthRenderTargetFormat format,
                         uint32_t pitch, xenos::MsaaSamples msaa, uint32_t scale4 = 0);
  RenderTarget* Find(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                     xenos::MsaaSamples msaa, uint32_t scale4 = 0) const;
  // The same surface at another scale (0: the cache's), created on first use.
  RenderTarget* Instance(const RenderTarget& target, uint32_t scale4);

  // Scene render scale (temporal upscaling, native_scene_upscale): while the
  // scene interval is open, surfaces marked as the scene's are drawn and
  // resolved at scene_scale4, in their own images; EDRAM ownership moves
  // content between a surface's images as between aliased surfaces.
  // `format`: the key colour format (KeyColorFormat) or the depth format.
  static uint64_t SurfaceKey(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                             xenos::MsaaSamples msaa);
  static uint64_t SurfaceKey(const RenderTarget& target) {
    return SurfaceKey(target.base_tiles, target.format, target.depth, target.pitch, target.msaa);
  }
  void SetSceneScale(uint32_t scale4) { scene_scale4_ = scale4; }
  uint32_t scene_scale4() const { return scene_scale4_; }
  void SetSceneInterval(bool open) {
    scene_interval_ = open && scene_scale4_ && scene_scale4_ != scale4_;
  }
  bool scene_interval() const { return scene_interval_; }
  void MarkScene(uint64_t surface_key) { scene_surfaces_.insert(surface_key); }
  bool IsScene(uint64_t surface_key) const { return scene_surfaces_.count(surface_key) != 0; }
  void ClearScene() {
    scene_surfaces_.clear();
    scene_interval_ = false;
  }
  uint32_t ScaleFor(uint64_t surface_key) const {
    return scene_interval_ && IsScene(surface_key) ? scene_scale4_ : scale4_;
  }

  // Before a draw or resolve uses `rect` (guest pixels) of the target: makes
  // it the owner of the EDRAM tiles there, bringing in what other targets
  // left in them (clears and transfers, outside the draws' render pass),
  // except the parts in `overwrite` (EdramPart) where `rect` covers tiles
  // completely. Then Written / Cleared on edram() for what is written.
  // `caller`: who claims, for the statistics and the EDRAM log only.
  enum class ClaimCaller : uint32_t {
    // A draw's colour or depth target.
    kDraw,
    // A draw found to replace whole tiles (a Direct3D 9 style clear
    // rectangle), claiming with an overwrite mask.
    kDrawClearRect,
    // A resolve's source.
    kResolve,
    // The colour or depth clear that comes with a resolve.
    kResolveClear,
    kCount,
  };
  static const char* ClaimCallerName(ClaimCaller caller);
  void Claim(RenderTarget& target, const Rect& rect, uint32_t overwrite = 0,
             ClaimCaller caller = ClaimCaller::kDraw);
  struct ClaimStats {
    uint64_t claims = 0, no_ops = 0, clears = 0, transfers = 0;
    uint64_t transfer_rects = 0, transfer_guest_pixels = 0, reinterpret = 0, stencil = 0;
    // EDRAM tiles a claim found still valid in its target (no transfer).
    uint64_t kept_tiles = 0;
    // By ClaimCaller and by the claiming target (0 colour, 1 depth): claims,
    // the transfers and clears they recorded, the transfers' guest pixels.
    struct ByCaller {
      uint64_t claims = 0, transfers = 0, clears = 0, transfer_guest_pixels = 0;
    };
    ByCaller by_caller[size_t(ClaimCaller::kCount)][2] = {};
  };
  ClaimStats TakeClaimStats() {
    auto stats = claim_stats_;
    claim_stats_ = {};
    return stats;
  }
  EdramOwnership& edram() { return edram_; }
  // Passes outside the draws (EDRAM transfers, resolve passes) so far: each
  // binds its own graphics pipeline and dynamic state in the open command
  // buffer, which the draws then bind again.
  uint64_t graphics_state_changes() const { return graphics_state_changes_; }
  void NoteGraphicsStateChange() { ++graphics_state_changes_; }

  // Into the submitter's open command buffer; `rect` in guest pixels, clipped
  // to the surface.
  void ClearColor(RenderTarget& target, const Rect& rect, const float color[4]);
  void ClearDepthStencil(RenderTarget& target, const Rect& rect, float depth, uint32_t stencil);
  void ClearColor(RenderTarget& target, const std::vector<Rect>& rects, const float color[4]);
  // `aspects`: depth, stencil or both.
  void ClearDepthStencil(RenderTarget& target, const std::vector<Rect>& rects, float depth,
                         uint32_t stencil, VkImageAspectFlags aspects);

  // Moves the image to `layout` (a barrier even when it is already in it).
  void Transition(CommandRecorder* command_buffer, RenderTarget& target, VkImageLayout layout);

  // A draw's framebuffer, in the attachment order of its render pass (the
  // colour targets by render target index, null where none, then depth),
  // created on first use. VK_NULL_HANDLE on failure.
  VkFramebuffer GetDrawFramebuffer(VkRenderPass render_pass,
                                   RenderTarget* const color[xenos::kMaxColorRenderTargets],
                                   RenderTarget* depth, VkExtent2D extent);

  // Host formats, chosen for the device at Initialize.
  VkFormat ColorVkFormat(xenos::ColorRenderTargetFormat format) const;
  VkFormat DepthVkFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormatFeatureFlags OptimalTilingFeatures(VkFormat format) const;

  // A render pass with the target's only attachment, loaded and stored, in
  // its attachment layout (clears, EDRAM transfers; target.framebuffer).
  VkRenderPass GetClearRenderPass(VkFormat format, VkSampleCountFlagBits samples, bool depth);

 private:
  RenderTarget* Get(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                    xenos::MsaaSamples msaa, uint32_t scale4);
  bool CreateImage(RenderTarget& target);
  void DestroyTarget(RenderTarget& target);
  // The render pass over the clear rectangles' bounds
  // (native_pass_area_bounds), else the whole target.
  bool BeginClear(RenderTarget& target, CommandRecorder*& command_buffer_out,
                  const std::vector<VkClearRect>& clear_rects);

  Submitter* submitter_ = nullptr;
  uint32_t tile_count_ = xenos::kEdramTileCount;
  uint32_t scale4_ = kScale4Min;
  VkFormat color_formats_[16] = {};
  VkFormat depth_formats_[2] = {};
  mutable std::unordered_map<VkFormat, VkFormatFeatureFlags> format_features_;

  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  uint32_t scene_scale4_ = 0;
  bool scene_interval_ = false;
  std::unordered_set<uint64_t> scene_surfaces_;
  uint64_t next_target_id_ = 1;
  EdramOwnership edram_;
  // Null when the transfer pipelines can't be set up (contents don't move
  // between surfaces then, as before).
  std::unique_ptr<EdramTransfers> transfers_;
  std::vector<EdramOp> claim_ops_;
  uint64_t graphics_state_changes_ = 0;
  bool profile_transfers_ = false;
  ClaimStats claim_stats_;
  // native_drop_transfers as last parsed, and the transfers it left out since.
  std::string drop_transfers_text_;
  TransferFilter drop_transfers_;
  uint64_t dropped_transfers_ = 0;
  // By VkFormat | samples << 32 | depth << 40.
  std::unordered_map<uint64_t, VkRenderPass> clear_render_passes_;

  // Compared and hashed as memory (no padding).
  struct DrawFramebufferKey {
    VkRenderPass render_pass;
    VkImageView views[xenos::kMaxColorRenderTargets + 1];
    uint32_t width;
    uint32_t height;
  };
  // By a hash of the key, collisions checked on the key. Kept while the
  // targets live (until shutdown).
  std::unordered_multimap<uint64_t, std::pair<DrawFramebufferKey, VkFramebuffer>>
      draw_framebuffers_;
};

}  // namespace rex::graphics::native
