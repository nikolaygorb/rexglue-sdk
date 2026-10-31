/**
 * @file        graphics/shader_storage_test.cpp
 * @brief       Tests for the persistent shader and pipeline storage files
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/storage.h>

using namespace rex::graphics;
namespace fs = std::filesystem;

namespace {

REXPACKEDSTRUCT(TestPipelineDescription, {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  uint64_t pixel_shader_hash;
  uint64_t pixel_shader_modification;
});

REXPACKEDSTRUCT(TestPipelineStoredDescription, {
  uint64_t description_hash;
  TestPipelineDescription description;
});

constexpr uint32_t kTestTitleId = 0x4D5307E6;
constexpr uint32_t kTestApiMagic = 0x54534554;
constexpr uint32_t kTestVersion = 0x20260109;

struct Scratch {
  fs::path root;

  explicit Scratch(const char* name)
      : root(fs::temp_directory_path() / "rexglue_shader_storage_test" / name) {
    fs::remove_all(root);
    fs::create_directories(root);
  }

  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

ShaderStorageWriter<TestPipelineStoredDescription>::PipelineStorageConfig TestConfig() {
  ShaderStorageWriter<TestPipelineStoredDescription>::PipelineStorageConfig config;
  config.file_suffix = ".test.xpso";
  config.api_magic = kTestApiMagic;
  config.version = kTestVersion;
  return config;
}

void AppendShader(FILE* file, xenos::ShaderType type, const std::vector<uint32_t>& ucode) {
  ShaderStoredHeader header = {};
  header.ucode_data_hash = XXH3_64bits(ucode.data(), ucode.size() * sizeof(uint32_t));
  header.ucode_dword_count = uint32_t(ucode.size());
  header.type = type;
  fwrite(&header, sizeof(header), 1, file);
  fwrite(ucode.data(), sizeof(uint32_t), ucode.size(), file);
}

TestPipelineStoredDescription MakePipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash,
                                           uint64_t ps_mod) {
  TestPipelineStoredDescription stored = {};
  stored.description.vertex_shader_hash = vs_hash;
  stored.description.vertex_shader_modification = vs_mod;
  stored.description.pixel_shader_hash = ps_hash;
  stored.description.pixel_shader_modification = ps_mod;
  stored.description_hash = XXH3_64bits(&stored.description, sizeof(stored.description));
  return stored;
}

struct LoadResult {
  bool initialized = false;
  std::vector<std::pair<xenos::ShaderType, uint64_t>> shaders;
  std::set<std::pair<uint64_t, uint64_t>> translations_needed;
  std::vector<TestPipelineStoredDescription> pipelines;
};

LoadResult Load(const fs::path& cache_root) {
  LoadResult result;
  ShaderStorageWriter<TestPipelineStoredDescription> writer;
  result.initialized = writer.InitializeShaderStorage(
      cache_root, kTestTitleId, TestConfig(),
      [&](xenos::ShaderType type, const uint32_t*, uint32_t, uint64_t ucode_data_hash) {
        result.shaders.emplace_back(type, ucode_data_hash);
        return true;
      },
      [&](const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
        result.translations_needed = translations_needed;
      },
      result.pipelines);
  writer.ShutdownShaderStorage();
  return result;
}

}  // namespace

// xenia-canary 64e51c544.
TEST_CASE("Shader storage creates valid empty files", "[graphics][shader_storage]") {
  Scratch scratch("empty");
  LoadResult first = Load(scratch.root);
  REQUIRE(first.initialized);
  CHECK(first.shaders.empty());
  CHECK(first.pipelines.empty());

  fs::path shareable = GetShaderStorageShareableRoot(scratch.root);
  CHECK(fs::file_size(GetShaderStorageFilePath(scratch.root, kTestTitleId)) ==
        sizeof(ShaderStorageFileHeader));
  CHECK(fs::file_size(shareable / "4D5307E6.test.xpso") == sizeof(PipelineStorageFileHeader));

  // The headers written are accepted on the next run.
  LoadResult second = Load(scratch.root);
  REQUIRE(second.initialized);
  CHECK(second.shaders.empty());
  CHECK(second.pipelines.empty());
}

// xenia-canary 64e51c544.
TEST_CASE("Shader storage loads stored shaders and pipelines", "[graphics][shader_storage]") {
  Scratch scratch("load");
  Load(scratch.root);  // Create the files with valid headers.

  std::vector<uint32_t> vs_ucode = {0x11111111, 0x22222222, 0x33333333};
  std::vector<uint32_t> ps_ucode = {0x44444444};
  uint64_t vs_hash = XXH3_64bits(vs_ucode.data(), vs_ucode.size() * sizeof(uint32_t));
  uint64_t ps_hash = XXH3_64bits(ps_ucode.data(), ps_ucode.size() * sizeof(uint32_t));
  {
    FILE* file =
        rex::filesystem::OpenFile(GetShaderStorageFilePath(scratch.root, kTestTitleId), "ab");
    REQUIRE(file);
    AppendShader(file, xenos::ShaderType::kVertex, vs_ucode);
    AppendShader(file, xenos::ShaderType::kPixel, ps_ucode);
    fclose(file);
  }
  {
    FILE* file = rex::filesystem::OpenFile(
        GetShaderStorageShareableRoot(scratch.root) / "4D5307E6.test.xpso", "ab");
    REQUIRE(file);
    TestPipelineStoredDescription pipelines[] = {
        MakePipeline(vs_hash, 1, ps_hash, 2),
        MakePipeline(vs_hash, 3, 0, 0),
    };
    fwrite(pipelines, sizeof(pipelines[0]), 2, file);
    fclose(file);
  }

  LoadResult result = Load(scratch.root);
  REQUIRE(result.initialized);
  REQUIRE(result.shaders.size() == 2);
  CHECK(result.shaders[0] == std::make_pair(xenos::ShaderType::kVertex, vs_hash));
  CHECK(result.shaders[1] == std::make_pair(xenos::ShaderType::kPixel, ps_hash));
  CHECK(result.pipelines.size() == 2);
  // No pixel shader in the second pipeline - no translation needed for hash 0.
  CHECK(result.translations_needed ==
        std::set<std::pair<uint64_t, uint64_t>>{{vs_hash, 1}, {vs_hash, 3}, {ps_hash, 2}});
}

// xenia-canary 64e51c544.
TEST_CASE("Shader storage truncates corrupted entries", "[graphics][shader_storage]") {
  Scratch scratch("corrupted");
  Load(scratch.root);

  std::vector<uint32_t> ucode = {0xAAAAAAAA, 0xBBBBBBBB};
  fs::path shader_path = GetShaderStorageFilePath(scratch.root, kTestTitleId);
  fs::path pipeline_path = GetShaderStorageShareableRoot(scratch.root) / "4D5307E6.test.xpso";
  {
    FILE* file = rex::filesystem::OpenFile(shader_path, "ab");
    REQUIRE(file);
    AppendShader(file, xenos::ShaderType::kVertex, ucode);
    // Header claiming more ucode than what follows.
    ShaderStoredHeader header = {};
    header.ucode_data_hash = 1;
    header.ucode_dword_count = 64;
    fwrite(&header, sizeof(header), 1, file);
    fclose(file);
  }
  {
    FILE* file = rex::filesystem::OpenFile(pipeline_path, "ab");
    REQUIRE(file);
    TestPipelineStoredDescription good = MakePipeline(1, 2, 3, 4);
    TestPipelineStoredDescription bad = MakePipeline(5, 6, 7, 8);
    bad.description_hash ^= 1;
    fwrite(&good, sizeof(good), 1, file);
    fwrite(&bad, sizeof(bad), 1, file);
    // Partial description at the end.
    fwrite(&good, sizeof(good) / 2, 1, file);
    fclose(file);
  }

  LoadResult result = Load(scratch.root);
  REQUIRE(result.initialized);
  CHECK(result.shaders.size() == 1);
  CHECK(result.pipelines.size() == 1);
  CHECK(fs::file_size(shader_path) ==
        sizeof(ShaderStorageFileHeader) + sizeof(ShaderStoredHeader) + sizeof(uint32_t) * 2);
  CHECK(fs::file_size(pipeline_path) ==
        sizeof(PipelineStorageFileHeader) + sizeof(TestPipelineStoredDescription));
}

// xenia-canary 64e51c544.
// Found by shader_storage_fuzzer: the microcode buffer was sized from the
// entry header before anything was checked, so a damaged header (up to 2^31
// dwords) asked for up to 8 GB at every start of the title.
TEST_CASE("Shader storage stops at an entry larger than the file", "[graphics][shader_storage]") {
  Scratch scratch("huge_entry");
  Load(scratch.root);

  std::vector<uint32_t> ucode = {0xAAAAAAAA, 0xBBBBBBBB};
  fs::path shader_path = GetShaderStorageFilePath(scratch.root, kTestTitleId);
  {
    FILE* file = rex::filesystem::OpenFile(shader_path, "ab");
    REQUIRE(file);
    AppendShader(file, xenos::ShaderType::kPixel, ucode);
    ShaderStoredHeader header = {};
    header.ucode_data_hash = 1;
    header.ucode_dword_count = 0x7FFFFFFF;
    fwrite(&header, sizeof(header), 1, file);
    fwrite(ucode.data(), sizeof(uint32_t), ucode.size(), file);
    fclose(file);
  }

  LoadResult result = Load(scratch.root);
  REQUIRE(result.initialized);
  CHECK(result.shaders.size() == 1);
  CHECK(fs::file_size(shader_path) ==
        sizeof(ShaderStorageFileHeader) + sizeof(ShaderStoredHeader) + sizeof(uint32_t) * 2);
}

TEST_CASE("Shader storage discards pipelines of another version", "[graphics][shader_storage]") {
  Scratch scratch("version");
  Load(scratch.root);
  fs::path pipeline_path = GetShaderStorageShareableRoot(scratch.root) / "4D5307E6.test.xpso";
  {
    FILE* file = rex::filesystem::OpenFile(pipeline_path, "ab");
    REQUIRE(file);
    TestPipelineStoredDescription stored = MakePipeline(1, 2, 3, 4);
    fwrite(&stored, sizeof(stored), 1, file);
    fclose(file);
  }

  ShaderStorageWriter<TestPipelineStoredDescription> writer;
  auto config = TestConfig();
  config.version = kTestVersion + 1;
  std::vector<TestPipelineStoredDescription> pipelines;
  REQUIRE(writer.InitializeShaderStorage(
      scratch.root, kTestTitleId, config,
      [](xenos::ShaderType, const uint32_t*, uint32_t, uint64_t) { return true; }, nullptr,
      pipelines));
  writer.ShutdownShaderStorage();
  CHECK(pipelines.empty());
  CHECK(fs::file_size(pipeline_path) == sizeof(PipelineStorageFileHeader));
}

// ForzaHorizon1-ReImagined SDK patch 0064 (drain part).
TEST_CASE("Shader storage writes what is queued before shutting down",
          "[graphics][shader_storage]") {
  Scratch scratch("shutdown_drain");
  constexpr uint64_t kPipelineCount = 2000;
  {
    ShaderStorageWriter<TestPipelineStoredDescription> writer;
    std::vector<TestPipelineStoredDescription> pipelines;
    REQUIRE(writer.InitializeShaderStorage(
        scratch.root, kTestTitleId, TestConfig(),
        [](xenos::ShaderType, const uint32_t*, uint32_t, uint64_t) { return true; }, nullptr,
        pipelines));
    for (uint64_t i = 0; i < kPipelineCount; ++i) {
      writer.QueuePipelineWrite(MakePipeline(i + 1, 0, 0, 0));
    }
    // Right away: most of the queue is still waiting for the write thread.
    writer.ShutdownShaderStorage();
  }

  LoadResult loaded = Load(scratch.root);
  REQUIRE(loaded.initialized);
  REQUIRE(loaded.pipelines.size() == kPipelineCount);
  CHECK(loaded.pipelines.front().description.vertex_shader_hash == 1);
  CHECK(loaded.pipelines.back().description.vertex_shader_hash == kPipelineCount);
}
