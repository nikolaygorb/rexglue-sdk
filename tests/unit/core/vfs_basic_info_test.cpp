/**
 * @file        core/vfs_basic_info_test.cpp
 * @brief       FileBasicInformation: attributes kept on host entries, times
 *              rounded as FAT keeps them (xenia-canary e70915a98)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>

using rex::filesystem::Entry;
using rex::filesystem::HostPathDevice;

namespace {

std::filesystem::path MakeTempDir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("rexglue_vfs_basic_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir / "sub");
  std::ofstream(dir / "save.bin") << "data";
  return dir;
}

}  // namespace

TEST_CASE("File times are rounded down to an even second", "[filesystem][vfs]") {
  constexpr uint64_t kSecond = 10'000'000ull;
  CHECK(Entry::RoundToFatTime(0) == 0);
  CHECK(Entry::RoundToFatTime(2 * kSecond) == 2 * kSecond);
  CHECK(Entry::RoundToFatTime(3 * kSecond + 5) == 2 * kSecond);
  CHECK(Entry::RoundToFatTime(4 * kSecond - 1) == 2 * kSecond);
}

TEST_CASE("A host entry keeps the attributes a title sets", "[filesystem][vfs]") {
  const auto dir = MakeTempDir();
  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* file = device.ResolvePath("save.bin");
    Entry* sub = device.ResolvePath("sub");
    REQUIRE(file != nullptr);
    REQUIRE(sub != nullptr);

    CHECK(file->SetAttributes(rex::filesystem::kFileAttributeArchive |
                              rex::filesystem::kFileAttributeReadOnly));
    CHECK(file->attributes() ==
          (rex::filesystem::kFileAttributeArchive | rex::filesystem::kFileAttributeReadOnly));
    // The host's own file is left alone: it is still writable.
    std::ofstream(dir / "save.bin", std::ios::app) << "more";
    CHECK(std::filesystem::file_size(dir / "save.bin") == 8);

    // A directory stays one; a file never becomes one.
    CHECK(sub->SetAttributes(rex::filesystem::kFileAttributeArchive));
    CHECK((sub->attributes() & rex::filesystem::kFileAttributeDirectory) != 0);
    CHECK(file->SetAttributes(rex::filesystem::kFileAttributeDirectory));
    CHECK((file->attributes() & rex::filesystem::kFileAttributeDirectory) == 0);
  }
  {
    HostPathDevice device("\\Device\\Test", dir, true);
    REQUIRE(device.Initialize());
    Entry* file = device.ResolvePath("save.bin");
    REQUIRE(file != nullptr);
    CHECK_FALSE(file->SetAttributes(rex::filesystem::kFileAttributeArchive));
  }

  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
