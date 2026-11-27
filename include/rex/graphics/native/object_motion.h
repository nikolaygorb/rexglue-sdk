// Object motion for the scene frames (title-agnostic). The scene draws' vertex
// shaders store their final host clip positions per vertex
// (SpirvShaderTranslator::SetMotionCapture); at kSceneEnd every captured draw
// is drawn again from those positions against the retained scene depth, with
// the positions of the same draw in the last retained frame (matched by its
// shaders, buffers and vertex count, ties by the closest float constants).
// Per pixel the result is the previous uv and depth of the surface there,
// moving objects and skinned meshes included.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native/resolve.h>
#include <rex/graphics/native/submission.h>

REXCVAR_DECLARE(bool, native_scene_object_motion);
REXCVAR_DECLARE(std::string, native_scene_object_motion_identity);

namespace rex::graphics::native {

class ObjectMotion {
 public:
  // Per vertex: xyzw bits, then (host vertex index & 0xFFFFFF) | tag.
  static constexpr uint32_t kEntryDwords = 5;
  // Fingerprint of a draw: its first used vertex shader float constants.
  static constexpr uint32_t kFingerprintRegisters = 32;

  ObjectMotion() = default;
  ObjectMotion(const ObjectMotion&) = delete;
  ObjectMotion& operator=(const ObjectMotion&) = delete;
  ~ObjectMotion();

  // native_scene_object_motion while the scene frames are on at start.
  static bool Requested();
  // native_scene_object_motion_identity (from Initialize): the float constant
  // registers [first, first + count) a draw is told apart by when its vertex
  // shader reads all of them; count 0: its first used constants.
  uint32_t identity_first() const { return identity_first_; }
  uint32_t identity_count() const { return identity_count_; }
  // The capture buffer (before the pipelines and bindings that hold it).
  bool Initialize(Submitter& submitter);
  // The caller has waited for the GPU.
  void Shutdown();
  VkBuffer buffer() const { return buffer_; }

  // From kFrameEnd: the next frames' draws into this depth target over this
  // viewport (host pixels) are the scene's.
  void Arm(uint64_t depth_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void Disarm() { armed_ = false; }
  bool IsSceneDraw(uint64_t depth_id, const VkViewport& viewport) const {
    return armed_ && depth_id == depth_id_ && viewport.x == float(x_) && viewport.y == float(y_) &&
           viewport.width == float(width_) && viewport.height == float(height_);
  }

  struct Draw {
    uint64_t key = 0;
    // Capture region (entries) and tag.
    uint32_t base = 0, mask = 0, tag = 0;
    VkBuffer index_buffer = VK_NULL_HANDLE;  // None: not indexed.
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    uint32_t index_endian = 0;  // vertex_index_endian (xenos::Endian) of the draw.
    uint32_t vertex_count = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool primitive_restart = false;
    std::array<float, 2> jitter{};       // In host NDC.
    std::array<float, 2> depth_range{};  // Viewport minDepth, maxDepth.
    uint32_t fingerprint_offset = 0, fingerprint_count = 0;
    int32_t previous = -1;  // In the previous frame's draws.
  };
  // A scene draw (IsSceneDraw): its capture region for the system constants
  // (motion_capture_base / _mask / _tag). False: not captured.
  bool Capture(Draw draw, const float* fingerprint, uint32_t fingerprint_count, uint32_t& base,
               uint32_t& mask, uint32_t& tag);
  // kSceneEnd, outside the draws' render pass: the captured draws into
  // output() (width x height, RGBA32F: previous uv, previous depth, 1 where a
  // draw with history matched `depth` of `depth_samples`), then this frame's
  // captures become the previous frame's. False: no object motion this frame.
  // history false: no pass, only the frames move on.
  bool Record(ResolvedTexture& depth, uint32_t width, uint32_t height,
              VkSampleCountFlagBits depth_samples, bool history);
  // The frame ended without Record: its captures are dropped.
  void DropFrame();
  ResolvedTexture& output() { return output_; }
  // A submission reads output().
  void UseOutput(uint64_t submission) { output_retire_ = std::max(output_retire_, submission); }

  void Report(std::ostream& file) const;

 private:
  struct Frame {
    std::vector<Draw> draws;
    std::vector<float> fingerprints;
    std::unordered_multimap<uint64_t, uint32_t> by_key;
  };
  bool EnsurePipelines();
  VkPipeline GetPipeline(VkPrimitiveTopology topology, bool primitive_restart);
  bool EnsureOutput(uint32_t width, uint32_t height);
  void DestroyOutput();
  void Flip();

  Submitter* submitter_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  uint32_t capacity_ = 0;  // Entries per frame (half of the buffer).
  uint32_t identity_first_ = 0, identity_count_ = 0;
  bool cleared_ = false;

  bool armed_ = false;
  uint64_t depth_id_ = 0;
  uint32_t x_ = 0, y_ = 0, width_ = 0, height_ = 0;

  Frame current_, previous_;
  std::vector<bool> previous_used_;
  uint32_t half_ = 0, cursor_ = 0, salt_ = 1;

  VkShaderModule vertex_shader_ = VK_NULL_HANDLE, fragment_shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  static constexpr size_t kSetCount = 8;
  std::array<VkDescriptorSet, kSetCount> sets_{};
  std::array<uint64_t, kSetCount> set_retire_{};
  size_t next_set_ = 0;
  std::unordered_map<uint32_t, VkPipeline> pipelines_;
  bool pipelines_failed_ = false;
  ResolvedTexture output_{};
  uint64_t output_retire_ = 0;

  // Report.
  uint64_t frames_ = 0, captured_draws_ = 0, captured_vertices_ = 0, matched_draws_ = 0,
           unmatched_draws_ = 0, budget_skips_ = 0, dropped_frames_ = 0, recorded_draws_ = 0;
  uint32_t last_draws_ = 0, last_vertices_ = 0, peak_vertices_ = 0;
};

}  // namespace rex::graphics::native
