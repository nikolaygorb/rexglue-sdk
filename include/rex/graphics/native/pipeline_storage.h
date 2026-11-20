/**
 * @file        graphics/native/pipeline_storage.h
 * @brief       Native renderer: pipelines kept on disk and created again at start
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanPipelineCache's shader storage (Xenia, Copyright 2022 Ben Vanik,
 *              BSD license), on the SDK's ShaderStorageWriter.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include <rex/graphics/native/draw_state.h>
#include <rex/graphics/native/pipelines.h>
#include <rex/graphics/pipeline/shader/storage.h>

namespace rex::graphics {
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::native {

class Shaders;

// What a pipeline is made of, without this run's handles: the guest shaders
// by their microcode hash and their modifications (which also give the
// tessellation stages), the draw render pass and the pipeline state. Stored
// as memory: no implicit padding.
struct NativePipelineDescription {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  // 0: none (depth only, or nothing rasterized); the modification then names
  // the host's depth-only float24 shader: 0 none, 1 truncating, 2 rounding.
  uint64_t pixel_shader_hash;
  uint64_t pixel_shader_modification;
  Pipelines::RenderPassKey render_pass;
  PipelineState state;
  uint8_t padding[8 - (sizeof(Pipelines::RenderPassKey) + sizeof(PipelineState)) % 8];

  // Any change to this, to PipelineState or to RenderPassKey changes it.
  static constexpr uint32_t kVersion = 0x20261110;
};
static_assert(sizeof(NativePipelineDescription) ==
                  sizeof(uint64_t) * 4 + sizeof(Pipelines::RenderPassKey) +
                      sizeof(PipelineState) +
                      sizeof(NativePipelineDescription::padding),
              "NativePipelineDescription is stored and hashed as memory");

struct NativePipelineStoredDescription {
  uint64_t description_hash;
  NativePipelineDescription description;
};

// The guest shaders in the SDK's shader storage (<title>.xsh, shared with the
// other backends) and the native pipelines in <title>.native.vk.xpso, plus
// the driver's pipeline cache in <title>.native.vk.bin (local). Load creates
// every stored pipeline in the background (Pipelines' workers); a draw that
// needs one before it is done waits for it as for any other. Command
// processor thread only.
class PipelineStorage {
 public:
  PipelineStorage() = default;
  PipelineStorage(const PipelineStorage&) = delete;
  PipelineStorage& operator=(const PipelineStorage&) = delete;
  ~PipelineStorage();

  void Initialize(Shaders& shaders, Pipelines& pipelines);
  // Saves and closes the storage open (if any).
  void Shutdown();

  // Opens the title's storage (closing the one open), loads its shaders and
  // queues its pipelines; blocking: also waits until they are created.
  void Load(const std::filesystem::path& cache_root, uint32_t title_id, bool blocking);

  bool is_active() const { return writer_.is_active(); }
  // A shader a draw uses, written once per storage.
  void StoreShader(const Shader& shader);
  // A pipeline a draw has just requested for the first time.
  void StorePipeline(const NativePipelineDescription& description);

  // At the end of a frame: the driver's cache saved (in the background) when
  // pipelines were created since the last save, at most every 30 s and only
  // while no pipeline is being created, and the storage files flushed. Titles
  // usually end by exiting the process, without a shutdown.
  void OnFrameEnd();

 private:
  void SavePipelineCacheData();
  // Through a temporary file renamed over the old one, so an exit while
  // writing leaves the previous cache.
  static void WriteCacheFile(const std::filesystem::path& path, const std::vector<uint8_t>& data);
  void JoinSaver();

  Shaders* shaders_ = nullptr;
  Pipelines* pipelines_ = nullptr;
  ShaderStorageWriter<NativePipelineStoredDescription> writer_;
  std::filesystem::path pipeline_cache_path_;
  std::chrono::steady_clock::time_point last_save_;
  uint32_t saved_pipeline_count_ = 0;
  std::thread saver_;
};

}  // namespace rex::graphics::native
