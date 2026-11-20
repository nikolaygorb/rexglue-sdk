/**
 * @file        graphics/native/pipeline_storage.cpp
 * @brief       Native renderer: pipelines kept on disk and created again at start
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              After VulkanPipelineCache's shader storage (Xenia, Copyright 2022 Ben Vanik,
 *              BSD license), on the SDK's ShaderStorageWriter.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/pipeline_storage.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/filesystem.h>
#include <rex/graphics/native/shaders.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/hash.h>
#include <rex/logging.h>

namespace rex::graphics::native {

namespace {
// 'NVKP', after the SDK's Vulkan backend's API magic.
constexpr uint32_t kPipelineStorageAPIMagicNative = 0x504B564E;
// The shortest time between two saves of the driver's cache during play.
constexpr auto kCacheSaveInterval = std::chrono::seconds(30);
}  // namespace

PipelineStorage::~PipelineStorage() {
  Shutdown();
}

void PipelineStorage::Initialize(Shaders& shaders, Pipelines& pipelines) {
  shaders_ = &shaders;
  pipelines_ = &pipelines;
}

void PipelineStorage::Shutdown() {
  JoinSaver();
  SavePipelineCacheData();
  // Writes what is still queued (shaders a draw used, pipelines) first.
  writer_.ShutdownShaderStorage();
}

void PipelineStorage::JoinSaver() {
  if (saver_.joinable()) {
    saver_.join();
  }
}

void PipelineStorage::WriteCacheFile(const std::filesystem::path& path,
                                     const std::vector<uint8_t>& data) {
  std::filesystem::path temporary_path = path;
  temporary_path += ".tmp";
  FILE* file = rex::filesystem::OpenFile(temporary_path, "wb");
  if (!file) {
    return;
  }
  bool written = fwrite(data.data(), 1, data.size(), file) == data.size();
  written &= fclose(file) == 0;
  std::error_code error_code;
  if (written) {
    std::filesystem::rename(temporary_path, path, error_code);
  }
  if (!written || error_code) {
    std::filesystem::remove(temporary_path, error_code);
    return;
  }
  REXGPU_INFO("Native renderer: saved {} bytes of the driver's pipeline cache", data.size());
}

void PipelineStorage::SavePipelineCacheData() {
  if (!pipelines_ || pipeline_cache_path_.empty()) {
    return;
  }
  std::vector<uint8_t> data;
  if (pipelines_->pipeline_count() != saved_pipeline_count_ &&
      pipelines_->GetPipelineCacheData(data) && !data.empty()) {
    WriteCacheFile(pipeline_cache_path_, data);
  }
  pipeline_cache_path_.clear();
}

void PipelineStorage::OnFrameEnd() {
  if (!pipelines_ || pipeline_cache_path_.empty()) {
    return;
  }
  uint32_t pipeline_count = pipelines_->pipeline_count();
  auto now = std::chrono::steady_clock::now();
  if (pipeline_count == saved_pipeline_count_ || now - last_save_ < kCacheSaveInterval ||
      !pipelines_->IsIdle()) {
    return;
  }
  last_save_ = now;
  saved_pipeline_count_ = pipeline_count;
  // The descriptions and shaders written so far reach the disk too.
  writer_.RequestFlush(true, true);
  std::vector<uint8_t> data;
  if (!pipelines_->GetPipelineCacheData(data) || data.empty()) {
    return;
  }
  // The previous save has long finished (30 s).
  JoinSaver();
  saver_ = std::thread([path = pipeline_cache_path_, data = std::move(data)]() {
    WriteCacheFile(path, data);
  });
}

void PipelineStorage::Load(const std::filesystem::path& cache_root, uint32_t title_id,
                           bool blocking) {
  if (!shaders_ || !pipelines_) {
    return;
  }
  Shutdown();

  ShaderStorageWriter<NativePipelineStoredDescription>::PipelineStorageConfig config;
  config.file_suffix = ".native.vk.xpso";
  config.api_magic = kPipelineStorageAPIMagicNative;
  config.version = std::max(NativePipelineDescription::kVersion,
                            SpirvShaderTranslator::Modification::kVersion);
  // Shaders loaded from this storage aren't written to it again.
  const uint32_t storage_index = writer_.storage_index() + 1;
  std::vector<NativePipelineStoredDescription> stored;
  if (!writer_.InitializeShaderStorage(
          cache_root, title_id, config,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords, uint32_t ucode_dword_count,
              uint64_t ucode_data_hash) {
            (void)ucode_data_hash;
            if (vulkan::VulkanShader* shader = shaders_->Load(type, ucode_dwords,
                                                              ucode_dword_count)) {
              shader->set_ucode_storage_index(storage_index);
            }
            return true;
          },
          // Translated below, for the pipelines that need them.
          nullptr, stored)) {
    return;
  }

  // The driver's cache of what it compiled, before anything is created with
  // it.
  std::filesystem::path local_root = GetShaderStorageLocalRoot(cache_root);
  std::error_code error_code;
  std::filesystem::create_directories(local_root, error_code);
  pipeline_cache_path_ = local_root / fmt::format("{:08X}.native.vk.bin", title_id);
  std::vector<uint8_t> cache_data;
  if (FILE* file = rex::filesystem::OpenFile(pipeline_cache_path_, "rb")) {
    rex::filesystem::Seek(file, 0, SEEK_END);
    int64_t size = rex::filesystem::Tell(file);
    if (size > 0) {
      cache_data.resize(size_t(size));
      rex::filesystem::Seek(file, 0, SEEK_SET);
      cache_data.resize(fread(cache_data.data(), 1, cache_data.size(), file));
    }
    fclose(file);
  }
  pipelines_->ReplacePipelineCache(cache_data);
  last_save_ = std::chrono::steady_clock::now();
  saved_pipeline_count_ = 0;

  // Their translations, in parallel.
  uint64_t start_ticks = rex::chrono::Clock::QueryHostTickCount();
  {
    std::vector<std::pair<vulkan::VulkanShader*, uint64_t>> translations;
    for (const NativePipelineStoredDescription& stored_description : stored) {
      const NativePipelineDescription& description = stored_description.description;
      if (vulkan::VulkanShader* vertex_shader =
              shaders_->Find(xenos::ShaderType::kVertex, description.vertex_shader_hash)) {
        translations.emplace_back(vertex_shader, description.vertex_shader_modification);
      }
      if (description.pixel_shader_hash) {
        if (vulkan::VulkanShader* pixel_shader =
                shaders_->Find(xenos::ShaderType::kPixel, description.pixel_shader_hash)) {
          translations.emplace_back(pixel_shader, description.pixel_shader_modification);
        }
      }
    }
    shaders_->TranslateBatch(translations);
  }

  // Every stored pipeline, queued for the workers.
  size_t queued = 0, missing_shader = 0, failed = 0;
  for (const NativePipelineStoredDescription& stored_description : stored) {
    const NativePipelineDescription& description = stored_description.description;
    vulkan::VulkanShader* vertex_shader =
        shaders_->Find(xenos::ShaderType::kVertex, description.vertex_shader_hash);
    vulkan::VulkanShader* pixel_shader =
        description.pixel_shader_hash
            ? shaders_->Find(xenos::ShaderType::kPixel, description.pixel_shader_hash)
            : nullptr;
    if (!vertex_shader || (description.pixel_shader_hash && !pixel_shader)) {
      ++missing_shader;
      continue;
    }
    vulkan::VulkanShader::VulkanTranslation* vertex_translation =
        shaders_->Translate(*vertex_shader, description.vertex_shader_modification);
    vulkan::VulkanShader::VulkanTranslation* pixel_translation =
        pixel_shader ? shaders_->Translate(*pixel_shader, description.pixel_shader_modification)
                     : nullptr;
    if (!vertex_translation || (pixel_shader && !pixel_translation)) {
      ++failed;
      continue;
    }
    // The tessellation stages from the vertex shader's modification.
    SpirvShaderTranslator::Modification vertex_modification(
        description.vertex_shader_modification);
    Shader::HostVertexShaderType host_vertex_shader_type =
        vertex_modification.vertex.host_vertex_shader_type;
    VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
    VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
    if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      auto tessellation_mode = xenos::TessellationMode(vertex_modification.vertex.tessellation_mode);
      tessellation_vertex_shader = pipelines_->GetTessellationVertexShader(
          tessellation_mode == xenos::TessellationMode::kAdaptive);
      tessellation_control_shader =
          pipelines_->GetTessellationControlShader(host_vertex_shader_type, tessellation_mode);
      if (tessellation_vertex_shader == VK_NULL_HANDLE ||
          tessellation_control_shader == VK_NULL_HANDLE) {
        ++failed;
        continue;
      }
    }
    const Pipelines::Layout* layout = pipelines_->GetLayout(
        uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size()),
        uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size()),
        pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0,
        pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0);
    if (!layout) {
      ++failed;
      continue;
    }
    VkShaderModule pixel_shader_module =
        pixel_translation ? pixel_translation->shader_module() : VK_NULL_HANDLE;
    if (!description.pixel_shader_hash && description.pixel_shader_modification) {
      pixel_shader_module =
          shaders_->GetDepthOnlyFloat24Shader(description.pixel_shader_modification == 2);
      if (pixel_shader_module == VK_NULL_HANDLE) {
        ++failed;
        continue;
      }
    }
    bool pending;
    pipelines_->GetPipeline(
        vertex_translation->shader_module(), pixel_shader_module,
        tessellation_vertex_shader, tessellation_control_shader, description.render_pass,
        *layout, description.state, true, 0, pending);
    ++queued;
  }
  if (blocking) {
    pipelines_->AwaitIdle();
  }
  if (!stored.empty()) {
    REXGPU_INFO(
        "Native renderer: {} stored pipelines queued in {} ms ({} with a shader not stored, {} "
        "failed), the driver's cache {} bytes",
        queued,
        (rex::chrono::Clock::QueryHostTickCount() - start_ticks) * 1000 /
            rex::chrono::Clock::QueryHostTickFrequency(),
        missing_shader, failed, cache_data.size());
  }
}

void PipelineStorage::StoreShader(const Shader& shader) {
  if (!writer_.is_active()) {
    return;
  }
  // Once per storage: the index is that of the storage it was written to.
  if (const_cast<Shader&>(shader).try_set_ucode_storage_index(writer_.storage_index())) {
    writer_.QueueShaderWrite(&shader);
  }
}

void PipelineStorage::StorePipeline(const NativePipelineDescription& description) {
  if (!writer_.is_active()) {
    return;
  }
  NativePipelineStoredDescription stored;
  stored.description_hash = XXH3_64bits(&description, sizeof(description));
  std::memcpy(&stored.description, &description, sizeof(description));
  writer_.QueuePipelineWrite(stored);
}

}  // namespace rex::graphics::native
