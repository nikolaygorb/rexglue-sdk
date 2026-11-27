// Opt-in retained GPU scene inputs for a temporal consumer: owned color/depth
// history, camera motion, reactive/camera-locked/sky masks and debug views.
#pragma once
#include <array>
#include <map>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <rex/cvar.h>
#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/object_motion.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/scene_history.h>
#include <rex/graphics/native/temporal_upscaler.h>
#include <rex/graphics/title_scene.h>
#include <rex/ui/vulkan/presenter.h>

REXCVAR_DECLARE(bool, native_scene_frames);
REXCVAR_DECLARE(int32_t, native_scene_debug);
REXCVAR_DECLARE(int32_t, native_scene_jitter);
REXCVAR_DECLARE(bool, native_scene_frame_images);
REXCVAR_DECLARE(bool, native_scene_upscale);  // command_processor.cpp

namespace rex::graphics::native {
// native_scene_frames, or implied by a temporal upscaler (native_temporal_upscaler)
// or frame generation (native_frame_generation, the presenter's).
bool SceneFramesEnabled();
// native_frame_generation is not off (read by name: the presenter's setting).
bool FrameGenerationRequested();
// One line for the launcher ("Last run: ..."): logs/upscaler.log next to the
// executable, with the GPU's name; written only when the text changes.
void WriteUpscalerStatus(Submitter& submitter, const std::string& text);
class SceneFrames {
 public:
  enum Image : size_t { kColor, kDepth, kMotion, kDebug, kMasks, kImageCount };
  ~SceneFrames();
  bool Initialize(Submitter& submitter, RenderTargets& targets);
  // The command processor's object motion (outlives the scene frames), or none.
  void SetObjectMotion(ObjectMotion* object_motion) { object_motion_ = object_motion; }
  void Shutdown();  // Caller waits for the GPU; before the source target cache dies.
  void BeginView(const scene::View& view);
  bool Matches(uint64_t session, uint64_t frame) const { return pending_valid_ && pending_.session == session && pending_.frame == frame; }
  void InvalidateView();
  // kLockedVolume of the pending view (before Retain): up to 4 boxes.
  void AddLockedVolume(const std::array<float, 6>& box);
  bool RejectView(const char* reason) { pending_valid_ = false; return Reject(reason); }
  // A refused annotation that leaves the frame's accepted view as it is.
  bool NoteRejected(const char* reason) { return Reject(reason); }
  // The last retained view: jitter targets and history identity for the next frame.
  const scene::View* LastView() const { return current_ < kNoSlot ? &slots_[current_].view : nullptr; }
  // The accepted view of the frame being built (kView), before kSceneEnd.
  const scene::View* PendingView() const { return pending_valid_ && !retained_ ? &pending_ : nullptr; }
  void NoteJitterDraws(uint64_t draws) { jitter_draws_ = draws; }
  // The temporal upscaler replaced the last retained frame's scene color.
  // Off at once when the upscaler is switched off (no marker may follow).
  bool UpscalerActive() const {
    return upscaler_active_ && TemporalUpscalerOn();
  }
  // The scale is within the loaded upscaler's ratio (XeSS: 3x), so the scene may
  // be drawn at 1x (native_scene_upscale); above it the scene stays at the scale.
  bool UpscaleFits() const;
  // Jitter phases the upscaler wants (0: no upscaler in use).
  uint32_t UpscalerJitterPhases();
  // Opaque/translucent boundary: the first blended draw tested against the
  // scene depth; copies its colour target once per frame before Retain.
  bool WantsAutoOpaque(uint64_t depth_id) const {
    return pending_valid_ && pending_.auto_opaque && !opaque_taken_ && !retained_ && depth_id == pending_.depth_id;
  }
  bool SnapshotOpaque(RenderTarget& color);
  bool Retain(RenderTarget& color, RenderTarget& depth);
  bool PresentDebug(RenderTarget& target, uint32_t width, uint32_t height, uint32_t color_mask = 15);

  // Frame generation (the presenter's; ui/vulkan/frame_generation.h).
  // Every marker of the main view: the frame's rectangle and kSceneNoFrameGeneration.
  void NoteFrameMarker(const title::SceneMarker& marker);
  // kPostEnd: the bound colour target holds the frame without the HUD.
  bool SnapshotHudless(RenderTarget& color, uint32_t guest_width, uint32_t guest_height);
  // kFrameEnd: the frame's inputs are complete when its view was retained.
  void EndFrame(uint64_t session, uint64_t frame);
  // In the presenter's refresher at the swap: the ended frame's depth and
  // motion into `targets` (frame space: output_width x output_height host
  // pixels for guest_width x guest_height), and the image to draw the
  // presenter's hudless image from. False: no inputs for this frame.
  struct HudlessSource {
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
  };
  bool WriteFrameGeneration(const ui::vulkan::VulkanPresenter::FrameGenerationTargets& targets,
                            uint32_t output_width, uint32_t output_height, uint32_t guest_width,
                            uint32_t guest_height, HudlessSource& hudless);
  // Its camera facts and flags for the presenter (after WriteFrameGeneration).
  void FillFrameGenerationInputs(ui::Presenter::FrameGenerationInputs& inputs) const;
  bool Export(const std::filesystem::path& base);
  void Report(const std::filesystem::path& path);
 private:
  struct Slot {
    // color, raw host depth, motion (xy px current->previous, z expected
    // previous depth, w coverage), debug view, masks (reactive, locked, sky, coverage)
    std::array<ResolvedTexture, kImageCount> images;
    scene::View view;
    uint64_t lease = 0, retire = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> present_sets{};
  };
  bool CreateImage(ResolvedTexture& image, uint32_t width, uint32_t height, VkFormat format, bool storage);
  void DestroyImage(ResolvedTexture& image);
  bool Dispatch(Slot& current, Slot* previous, bool history, bool objects);
  bool Upscale(RenderTarget& target, Slot& slot, bool history);
  // The slot's colour beyond its view: the view's edge repeated (upscaler input).
  void PadColor(CommandRecorder* commands, Slot& slot);
  // Frame-local image shared by all slots; recreated (after a GPU wait) on resize. True: (re)created.
  // At the pending view's size, or width x height when given.
  bool EnsureShared(ResolvedTexture& image, VkFormat format, bool storage, bool& created,
                    uint32_t width = 0, uint32_t height = 0);
  // At (x, y) of the target (a scene viewport off the origin).
  bool DrawImage(RenderTarget& target, ResolvedTexture& image, VkDescriptorSet set, uint32_t width,
                 uint32_t height, uint32_t color_mask, uint32_t x = 0, uint32_t y = 0);
  VkRenderPass PresentRenderPass(VkFormat format);
  VkPipeline PresentPipeline(VkFormat format, uint32_t color_mask);
  // The width x height corner (the view's part of a larger image).
  bool ReadImage(ResolvedTexture& image, std::vector<uint8_t>& bytes, uint32_t width, uint32_t height);
  bool EnsureFrameGenerationPipeline();
  // The largest view so far at the view's scale (host pixels): the part of the
  // images the scene pass and the upscalers cover (the view in its corner).
  uint32_t ExtentWidth(const scene::View& view) const;
  uint32_t ExtentHeight(const scene::View& view) const;
  bool Reject(const char* reason);
  Submitter* submitter_ = nullptr;
  RenderTargets* targets_ = nullptr;
  ObjectMotion* object_motion_ = nullptr;
  uint64_t object_frames_ = 0;  // Dispatches with object motion.
  uint64_t extent_continues_ = 0;  // Extent-only resets whose motion kept its history.
  // The largest view so far, guest pixels: the images' and the upscalers' size.
  uint32_t extent_guest_width_ = 0, extent_guest_height_ = 0;
  // Images re-created larger (a GPU wait or new memory), Retain calls over 4 ms, the longest.
  uint64_t image_growths_ = 0, retain_slow_ = 0;
  double retain_max_ms_ = 0;
  std::unique_ptr<ResolvePass> resolve_;
  // Current + pinned previous + frames the GPU may still read (CP records up
  // to 3 frames ahead) + one spare. 4 slots dropped ~15% of frames as busy.
  static constexpr size_t kSlotCount = 6;
  std::array<Slot, kSlotCount> slots_{};
  ResolvedTexture opaque_{};  // Shared: written and consumed within one frame, in queue order.
  ResolvedTexture ndc_depth_{}, upscaled_{};  // Shared too: provider depth input, upscaler output.
  VkDescriptorSet upscaled_set_ = VK_NULL_HANDLE;
  std::unique_ptr<TemporalUpscaler> upscaler_;
  UpscalerProvider upscaler_provider_ = UpscalerProvider::kFsr;  // The loaded/attempted one.
  bool upscaler_attempted_ = false, upscaler_active_ = false, upscale_limited_ = false;
  uint32_t upscaler_misses_ = 0;
  std::string upscaler_state_ = "off";
  uint64_t upscaled_count_ = 0, upscaler_skips_ = 0;
  uint64_t opaque_retire_ = 0, opaque_color_id_ = 0;  // The snapshot's source (reactive needs the retained colour's).
  scene::View pending_{};
  bool pending_valid_ = false, retained_ = false, opaque_taken_ = false, opaque_valid_ = false;
  bool images_exported_ = false;
  uint32_t output_width_ = 0, output_height_ = 0;
  uint32_t output_mask_ = 0;
  size_t current_ = kSlotCount, previous_ = kSlotCount;
  uint64_t next_lease_ = 0, previous_lease_ = 0;
  uint64_t retained_count_ = 0, dispatch_count_ = 0, debug_count_ = 0, busy_skips_ = 0, rejected_ = 0, failures_ = 0, resets_ = 0;
  uint64_t opaque_count_ = 0, title_resets_ = 0, jittered_ = 0, jitter_draws_ = 0;
  static constexpr size_t kNoSlot = kSlotCount;
  std::string reset_reason_ = "first-view";
  const char* last_reject_reason_ = "none";
  std::map<std::string, uint64_t> reject_reasons_;  // Reason -> count (report).
  uint64_t last_reject_frame_ = 0, last_reject_session_ = 0;
  VkShaderModule shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  // scene::LockedVolumes per slot (uniform buffer, host-visible).
  static constexpr VkDeviceSize kLockedStride = 256;
  VkBuffer locked_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory locked_memory_ = VK_NULL_HANDLE;
  uint32_t locked_memory_type_ = 0;
  VkDeviceSize locked_memory_size_ = 0;
  uint8_t* locked_mapping_ = nullptr;
  uint64_t locked_frames_ = 0;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
  VkShaderModule present_vs_ = VK_NULL_HANDLE, present_ps_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  // Frame generation: this frame's state (building) and the ended frame's.
  struct FrameGenerationFrame {
    uint64_t session = 0, frame = 0, lease = 0;
    size_t slot = kSlotCount;
    bool retained = false, hudless = false, no_generation = false, reset = false;
    uint16_t rect[4]{};  // Scene in the frame, guest pixels (0: at the origin, 1:1).
  };
  FrameGenerationFrame fg_building_{}, fg_ended_{};
  ResolvedTexture hudless_{}, fg_view_{};  // The frame without the HUD; diagnostics view.
  uint32_t hudless_guest_width_ = 0, hudless_guest_height_ = 0;
  VkShaderModule fg_shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout fg_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fg_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline fg_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool fg_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotCount> fg_sets_{};
  std::array<uint64_t, kSlotCount> fg_set_retire_{};
  size_t fg_next_set_ = 0;
  bool fg_pipeline_failed_ = false;
  FrameGenerationFrame fg_written_frame_{};
  std::array<float, 2> fg_scale_{1.f, 1.f};  // Scene pixels -> frame pixels of the written frame.
  // The written frames' view-projections: this one's and the one before
  // (consecutive frames of a session only), for the camera matrices.
  scene::Matrix fg_pv_{}, fg_previous_pv_{};
  uint64_t fg_pv_session_ = 0, fg_pv_frame_ = 0, fg_motion_previous_frame_ = 0;
  bool fg_has_previous_pv_ = false;
  // The written frames' scene clip space in the frame's: sx, ox, sy, oy.
  std::array<double, 4> fg_frame_clip_{1., 0., 1., 0.}, fg_previous_frame_clip_{1., 0., 1., 0.};
  std::array<float, 4> fg_rect_{};  // The written frame's scene in the frame, output pixels.
  uint64_t fg_written_ = 0, fg_hudless_count_ = 0, fg_skips_ = 0;
  const char* fg_skip_reason_ = "none";
  std::map<std::string, uint64_t> fg_skip_reasons_;  // Reason -> count (report).
  std::map<std::string, uint64_t> reset_reasons_;    // History resets by reason (report).
  uint32_t logged_resets_ = 0;                       // The first ones logged with what changed.
  uint64_t fg_implicit_ends_ = 0;  // Frames written at the present before their kFrameEnd.
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  std::unordered_map<VkFormat, VkRenderPass> present_passes_;
  std::unordered_map<uint64_t, VkPipeline> present_pipelines_;
  std::unordered_map<const RenderTarget*, VkFramebuffer> present_framebuffers_;
};
}  // namespace rex::graphics::native
