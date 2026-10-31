/**
 * @file        fuzz/shader_storage_fuzzer.cpp
 * @brief       libFuzzer target for reading the shader (.xsh) and pipeline
 *              (.xpso) storage files
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/pipeline/shader/storage.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <process.h>
#define REX_FUZZ_GETPID _getpid
#else
#include <unistd.h>
#define REX_FUZZ_GETPID getpid
#endif

using namespace rex::graphics;
namespace fs = std::filesystem;

// Input layout:
//   0: bit 0      - put the right hash into every shader entry header;
//      bit 1      - put the right hash into every pipeline description;
//      bit 2      - start the .xsh file with a valid file header;
//      bit 3      - start the .xpso file with a valid file header;
//      bit 4      - no .xsh file;
//      bit 5      - no .xpso file;
//   1..4          - the length of the .xsh part (little endian, clamped);
//   then          - the .xsh part, then the .xpso part.
// The hashes are filled in by the fuzzer because random data never has them,
// and storage that went bad in another way (a crash in the middle of a write,
// a disk error) has whole entries with the right hashes before the damage.
// tools/fuzz/make_storage_corpus.py turns the titles' storage into inputs.
//
// ShaderStorageWriter::InitializeShaderStorage reads both files like at the
// start of a title (the backends only add what they do with the loaded
// shaders and descriptions) and cuts them to the valid part. Checked on top of
// ASan and the allocation limit: loading the cut files again keeps all of
// them, the same shaders and descriptions.

namespace {

// Laid out like the backends' descriptions: the shader hashes and
// modifications ShaderStorageWriter reads, then render state.
REXPACKEDSTRUCT(FuzzPipelineDescription, {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  uint64_t pixel_shader_hash;
  uint64_t pixel_shader_modification;
  uint8_t state[32];
});

REXPACKEDSTRUCT(FuzzPipelineStoredDescription, {
  uint64_t description_hash;
  FuzzPipelineDescription description;
});

constexpr uint32_t kTitleId = 0x4D5307E6;

ShaderStorageWriter<FuzzPipelineStoredDescription>::PipelineStorageConfig PipelineConfig() {
  ShaderStorageWriter<FuzzPipelineStoredDescription>::PipelineStorageConfig config;
  config.file_suffix = ".fuzz.xpso";
  config.api_magic = 0x5A465558;  // 'XUFZ'
  config.version = 0x20261029;
  return config;
}

// A directory of its own for each process (libFuzzer -fork and -jobs run
// several), removed at exit.
struct CacheRoot {
  fs::path path;
  CacheRoot()
      : path(fs::temp_directory_path() /
             fmt::format("rexglue_shader_storage_fuzzer_{}", REX_FUZZ_GETPID())) {
    std::error_code error_code;
    fs::remove_all(path, error_code);
  }
  ~CacheRoot() {
    std::error_code error_code;
    fs::remove_all(path, error_code);
  }
};

void StampShaderHashes(std::vector<uint8_t>& file, size_t offset) {
  ShaderStoredHeader header;
  while (file.size() - offset >= sizeof(header)) {
    std::memcpy(&header, file.data() + offset, sizeof(header));
    size_t ucode_bytes = size_t(header.ucode_dword_count) * sizeof(uint32_t);
    offset += sizeof(header);
    if (file.size() - offset < ucode_bytes) {
      return;
    }
    header.ucode_data_hash = XXH3_64bits(file.data() + offset, ucode_bytes);
    std::memcpy(file.data() + offset - sizeof(header), &header, sizeof(header));
    offset += ucode_bytes;
  }
}

void StampPipelineHashes(std::vector<uint8_t>& file, size_t offset) {
  FuzzPipelineStoredDescription stored;
  for (; file.size() - offset >= sizeof(stored); offset += sizeof(stored)) {
    std::memcpy(&stored, file.data() + offset, sizeof(stored));
    stored.description_hash = XXH3_64bits(&stored.description, sizeof(stored.description));
    std::memcpy(file.data() + offset, &stored, sizeof(stored));
  }
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& contents) {
  FILE* file = rex::filesystem::OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  bool written = contents.empty() || fwrite(contents.data(), contents.size(), 1, file) == 1;
  fclose(file);
  return written;
}

struct Loaded {
  bool initialized = false;
  std::vector<std::pair<uint64_t, uint32_t>> shaders;
  std::vector<FuzzPipelineStoredDescription> pipelines;
  std::set<std::pair<uint64_t, uint64_t>> translations_needed;
};

Loaded Load(const fs::path& cache_root) {
  Loaded loaded;
  ShaderStorageWriter<FuzzPipelineStoredDescription> writer;
  loaded.initialized = writer.InitializeShaderStorage(
      cache_root, kTitleId, PipelineConfig(),
      [&](xenos::ShaderType type, const uint32_t* ucode_dwords, uint32_t ucode_dword_count,
          uint64_t ucode_data_hash) {
        // The backends copy the microcode out (ASan checks the range).
        std::vector<uint32_t> ucode(ucode_dwords, ucode_dwords + ucode_dword_count);
        loaded.shaders.emplace_back(ucode_data_hash, uint32_t(type));
        return true;
      },
      [&](const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
        loaded.translations_needed = translations_needed;
      },
      loaded.pipelines);
  writer.ShutdownShaderStorage();
  return loaded;
}

[[noreturn]] void Fail(const char* what, const Loaded& first, const Loaded& second) {
  std::fprintf(stderr,
               "shader_storage_fuzzer: %s\n"
               "  first load: %zu shaders, %zu pipelines; second load: %zu shaders, %zu "
               "pipelines\n",
               what, first.shaders.size(), first.pipelines.size(), second.shaders.size(),
               second.pipelines.size());
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 5) {
    return 0;
  }
  static CacheRoot cache_root;
  uint8_t flags = data[0];
  uint32_t xsh_size;
  std::memcpy(&xsh_size, data + 1, sizeof(xsh_size));
  data += 5;
  size -= 5;
  xsh_size = uint32_t(std::min(size_t(xsh_size), size));

  if (!EnsureShaderStorageDirectoriesExist(cache_root.path)) {
    return 0;
  }
  fs::path xsh_path = GetShaderStorageFilePath(cache_root.path, kTitleId);
  fs::path xpso_path = GetShaderStorageShareableRoot(cache_root.path) /
                       fmt::format("{:08X}{}", kTitleId, PipelineConfig().file_suffix);
  std::error_code error_code;

  if (flags & (1 << 4)) {
    fs::remove(xsh_path, error_code);
  } else {
    std::vector<uint8_t> xsh;
    if (flags & (1 << 2)) {
      ShaderStorageFileHeader header;
      header.magic = kShaderStorageMagic;
      header.version_swapped = rex::byte_swap(ShaderStoredHeader::kVersion);
      xsh.resize(sizeof(header));
      std::memcpy(xsh.data(), &header, sizeof(header));
    }
    size_t entries_offset = xsh.size();
    xsh.insert(xsh.end(), data, data + xsh_size);
    if (flags & (1 << 0)) {
      StampShaderHashes(xsh, entries_offset);
    }
    if (!WriteFile(xsh_path, xsh)) {
      return 0;
    }
  }
  data += xsh_size;
  size -= xsh_size;

  if (flags & (1 << 5)) {
    fs::remove(xpso_path, error_code);
  } else {
    std::vector<uint8_t> xpso;
    if (flags & (1 << 3)) {
      ShaderStorageWriter<FuzzPipelineStoredDescription>::PipelineStorageConfig config =
          PipelineConfig();
      PipelineStorageFileHeader header;
      header.magic = kPipelineStorageMagic;
      header.magic_api = config.api_magic;
      header.version_swapped = rex::byte_swap(config.version);
      xpso.resize(sizeof(header));
      std::memcpy(xpso.data(), &header, sizeof(header));
    }
    size_t descriptions_offset = xpso.size();
    xpso.insert(xpso.end(), data, data + size);
    if (flags & (1 << 1)) {
      StampPipelineHashes(xpso, descriptions_offset);
    }
    if (!WriteFile(xpso_path, xpso)) {
      return 0;
    }
  }

  Loaded first = Load(cache_root.path);
  if (!first.initialized) {
    return 0;
  }
  // The files now hold only what was valid, so all of it loads again.
  Loaded second = Load(cache_root.path);
  if (!second.initialized) {
    Fail("the storage cut to its valid part can't be opened", first, second);
  }
  if (second.shaders != first.shaders) {
    Fail("the storage cut to its valid part loads other shaders", first, second);
  }
  if (second.pipelines.size() != first.pipelines.size() ||
      (!first.pipelines.empty() &&
       std::memcmp(second.pipelines.data(), first.pipelines.data(),
                   first.pipelines.size() * sizeof(FuzzPipelineStoredDescription)))) {
    Fail("the storage cut to its valid part loads other pipelines", first, second);
  }
  if (second.translations_needed != first.translations_needed) {
    Fail("the storage cut to its valid part needs other translations", first, second);
  }
  return 0;
}
