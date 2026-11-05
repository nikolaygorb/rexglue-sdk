/**
 * @file        graphics/pipeline/shader/storage_seed.cpp
 * @brief       A shader cache shipped with a title, seeding the player's
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <rex/graphics/pipeline/shader/storage_seed.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(shader_cache_shipped, "", "GPU",
                      "Folder of the shader cache shipped with the title (empty: shader_cache "
                      "beside the executable); seeds this PC's shader and pipeline storage at "
                      "startup")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {
namespace {

using Bytes = std::vector<uint8_t>;

std::optional<Bytes> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return Bytes(std::istreambuf_iterator<char>(file), {});
}

struct Record {
  uint64_t hash;
  std::span<const uint8_t> bytes;  // the whole record
};

// The valid records after the header, in order, stopping at the first
// damaged or cut-off one. Empty when the header is not `format`'s.
std::optional<std::vector<Record>> Records(const Bytes& file, const StorageFormat& format) {
  if (file.size() < format.header.size() ||
      !std::equal(format.header.begin(), format.header.end(), file.begin())) {
    return std::nullopt;
  }
  std::vector<Record> records;
  size_t at = format.header.size();
  for (;;) {
    size_t size = format.record_size, covered_at = 8;
    if (!format.record_size) {
      if (file.size() - at < 12) {
        break;
      }
      uint32_t field;
      std::memcpy(&field, &file[at + 8], 4);
      size = 12 + size_t(field & 0x7FFFFFFF) * 4;
      covered_at = 12;
    }
    if (size < covered_at || file.size() - at < size) {
      break;
    }
    uint64_t hash;
    std::memcpy(&hash, &file[at], 8);
    if (XXH3_64bits(&file[at + covered_at], size - covered_at) != hash) {
      break;
    }
    records.push_back({hash, std::span<const uint8_t>(&file[at], size)});
    at += size;
  }
  return records;
}

bool Write(const std::filesystem::path& path, std::span<const uint8_t> header,
           std::span<const Record> first, std::span<const Record> second, std::string* error) {
  std::filesystem::path temp = path;
  temp += ".seed";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    for (std::span<const Record> records : {first, second}) {
      for (const Record& r : records) {
        file.write(reinterpret_cast<const char*>(r.bytes.data()), std::streamsize(r.bytes.size()));
      }
    }
    if (!file) {
      *error = fmt::format("could not write {}", rex::path_to_utf8(temp));
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    *error = fmt::format("could not replace {}", rex::path_to_utf8(path));
    return false;
  }
  return true;
}

}  // namespace

SeedOutcome SeedStorageFile(const std::filesystem::path& shipped, const std::filesystem::path& user,
                            const StorageFormat& format) {
  SeedOutcome outcome;
  const auto shipped_bytes = ReadFile(shipped);
  if (!shipped_bytes) {
    return outcome;
  }
  const auto shipped_records = Records(*shipped_bytes, format);
  if (!shipped_records) {
    outcome.result = SeedResult::kStale;
    return outcome;
  }
  const auto user_bytes = ReadFile(user);
  const auto user_records = user_bytes ? Records(*user_bytes, format) : std::nullopt;
  if (!user_records) {
    // The player has none, or another version's, which the pipeline cache
    // would discard.
    if (!Write(user, format.header, *shipped_records, {}, &outcome.error)) {
      outcome.result = SeedResult::kFailed;
      return outcome;
    }
    outcome.result = SeedResult::kCopied;
    outcome.added = shipped_records->size();
    return outcome;
  }
  std::unordered_set<uint64_t> known;
  for (const Record& r : *user_records) {
    known.insert(r.hash);
  }
  std::vector<Record> missing;
  for (const Record& r : *shipped_records) {
    if (known.insert(r.hash).second) {
      missing.push_back(r);
    }
  }
  if (missing.empty()) {
    outcome.result = SeedResult::kUpToDate;
    return outcome;
  }
  if (!Write(user, format.header, *user_records, missing, &outcome.error)) {
    outcome.result = SeedResult::kFailed;
    return outcome;
  }
  outcome.result = SeedResult::kMerged;
  outcome.added = missing.size();
  return outcome;
}

void SeedFromShippedShaderCache(const std::filesystem::path& user_file,
                                const StorageFormat& format) {
  const std::string& shipped_setting = REXCVAR_GET(shader_cache_shipped);
  const std::filesystem::path shipped_root =
      shipped_setting.empty() ? rex::filesystem::GetExecutableFolder() / "shader_cache"
                              : rex::to_path(shipped_setting);
  const SeedOutcome outcome =
      SeedStorageFile(shipped_root / user_file.filename(), user_file, format);
  switch (outcome.result) {
    case SeedResult::kCopied:
    case SeedResult::kMerged:
      REXGPU_INFO("Shipped shader cache: {} records added to {}", outcome.added,
                  rex::path_to_utf8(user_file.filename()));
      break;
    case SeedResult::kStale:
      REXGPU_WARN("Shipped shader cache: {} is from another SDK version or damaged; not used",
                  rex::path_to_utf8(user_file.filename()));
      break;
    case SeedResult::kFailed:
      REXGPU_WARN("Shipped shader cache: {}", outcome.error);
      break;
    default:
      break;
  }
}

}  // namespace rex::graphics
