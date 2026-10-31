/**
 * @file        core/content_device_test.cpp
 * @brief       Damaged disc images and content packages mount safely or not
 *              at all (found by disc_image_fuzzer and stfs_fuzzer)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

using namespace rex;
using namespace rex::filesystem;

namespace {

constexpr size_t kSectorSize = 0x800;
constexpr uint32_t kRootSector = 33;
constexpr uint32_t kFileAttributeDirectoryBit = 0x10;

struct TestFile {
  std::filesystem::path path;
  explicit TestFile(const char* name) {
    path = std::filesystem::temp_directory_path() / name;
  }
  ~TestFile() {
    std::error_code error_code;
    std::filesystem::remove(path, error_code);
  }
  void Write(const std::vector<uint8_t>& data) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  }
};

void StoreLe16(uint8_t* p, uint16_t value) {
  std::memcpy(p, &value, sizeof(value));
}

void StoreLe32(uint8_t* p, uint32_t value) {
  std::memcpy(p, &value, sizeof(value));
}

void StoreDiscEntry(uint8_t* p, uint16_t left, uint16_t right, uint32_t sector, uint32_t length,
                    uint8_t attributes, std::string_view name) {
  StoreLe16(p, left);
  StoreLe16(p + 2, right);
  StoreLe32(p + 4, sector);
  StoreLe32(p + 8, length);
  p[12] = attributes;
  p[13] = uint8_t(name.size());
  std::memcpy(p + 14, name.data(), name.size());
}

// Sector 32 pointing at a one-sector root directory in sector 33, then 4 KB.
std::vector<uint8_t> MakeDiscImage() {
  std::vector<uint8_t> image((kRootSector + 1) * kSectorSize + 0x1000);
  uint8_t* descriptor = image.data() + 32 * kSectorSize;
  std::memcpy(descriptor, "MICROSOFT*XBOX*MEDIA", 20);
  StoreLe32(descriptor + 20, kRootSector);
  StoreLe32(descriptor + 24, kSectorSize);
  return image;
}

uint8_t* RootDirectory(std::vector<uint8_t>& image) {
  return image.data() + kRootSector * kSectorSize;
}

// A read-only STFS package: a hash table, the directory in block 0, data after.
std::vector<uint8_t> MakeStfsPackage(uint32_t file_table_block = 0) {
  constexpr size_t kDataOffset = 0xA000;
  constexpr size_t kBlockSize = 0x1000;
  std::vector<uint8_t> package(kDataOffset + 3 * kBlockSize);
  auto* header = reinterpret_cast<StfsHeader*>(package.data());
  header->header.magic = XContentPackageType::kCon;
  header->header.header_size = uint32_t(sizeof(StfsHeader));
  header->metadata.volume_type = XContentVolumeType::kStfs;
  auto& descriptor = header->metadata.volume_descriptor.stfs;
  descriptor.descriptor_length = sizeof(StfsVolumeDescriptor);
  descriptor.flags.bits.read_only_format = 1;
  descriptor.file_table_block_count = 1;
  descriptor.set_file_table_block_number(file_table_block);
  descriptor.total_block_count = 1;
  // The directory block is the last of its chain.
  auto* table = reinterpret_cast<StfsHashTable*>(package.data() + kDataOffset);
  table->entries[0].set_level0_next_block(0xFFFFFF);
  return package;
}

StfsDirectoryEntry& FirstStfsEntry(std::vector<uint8_t>& package) {
  return reinterpret_cast<StfsDirectoryBlock*>(package.data() + 0xB000)->entries[0];
}

// Reads every file under the entry to its end.
void ReadTree(Entry& entry, std::vector<uint8_t>& buffer, size_t& files, uint64_t& bytes) {
  for (const auto& child : entry.children()) {
    if (child->attributes() & kFileAttributeDirectoryBit) {
      ReadTree(*child, buffer, files, bytes);
      continue;
    }
    INFO(child->path());
    File* opened = nullptr;
    REQUIRE(child->Open(1, &opened) == X_STATUS_SUCCESS);
    size_t offset = 0;
    while (offset < child->size()) {
      size_t bytes_read = 0;
      if (opened->ReadSync(std::span<uint8_t>(buffer), offset, &bytes_read) != X_STATUS_SUCCESS ||
          !bytes_read) {
        break;
      }
      offset += bytes_read;
    }
    opened->Destroy();
    CHECK(offset == child->size());
    ++files;
    bytes += offset;
  }
}

}  // namespace

// Not part of the normal run: real packages can't be in the repository. Run with
// "[package-list]" and REX_TEST_PACKAGE_LIST naming a file of one path per line.
TEST_CASE("Packages of the list mount and read to the end", "[.][package-list]") {
  const char* list_path = std::getenv("REX_TEST_PACKAGE_LIST");
  REQUIRE(list_path);
  std::ifstream list(list_path);
  REQUIRE(list.is_open());
  std::vector<uint8_t> buffer(0x100000);
  size_t packages = 0;
  std::string path;
  while (std::getline(list, path)) {
    if (path.empty()) {
      continue;
    }
    INFO(path);
    StfsContainerDevice device("\\Device\\Test", std::filesystem::u8path(path));
    REQUIRE(device.Initialize());
    size_t files = 0;
    uint64_t bytes = 0;
    Entry* root = device.ResolvePath("");
    REQUIRE(root);
    ReadTree(*root, buffer, files, bytes);
    CHECK(files > 0);
    WARN(path << ": " << files << " files, " << bytes << " bytes");
    ++packages;
  }
  CHECK(packages > 0);
}

TEST_CASE("Disc image with a well-formed root mounts", "[filesystem][disc]") {
  TestFile file("rexglue_disc_ok.iso");
  auto image = MakeDiscImage();
  StoreDiscEntry(RootDirectory(image), 0, 0, kRootSector + 1, 0x100, 0, "a.bin");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  REQUIRE(device.Initialize());
  CHECK(device.ResolvePath("a.bin") != nullptr);
}

TEST_CASE("Disc image directory cycles end", "[filesystem][disc]") {
  TestFile file("rexglue_disc_cycle.iso");
  auto image = MakeDiscImage();
  // The second entry's right node is the second entry.
  StoreDiscEntry(RootDirectory(image), 0, 8, kRootSector + 1, 0x10, 0, "a");
  StoreDiscEntry(RootDirectory(image) + 8 * 4, 0, 8, kRootSector + 1, 0x10, 0, "b");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  REQUIRE(device.Initialize());
  CHECK(device.ResolvePath("")->child_count() == 2);
}

TEST_CASE("Disc image folder that holds itself ends", "[filesystem][disc]") {
  TestFile file("rexglue_disc_folder.iso");
  auto image = MakeDiscImage();
  StoreDiscEntry(RootDirectory(image), 0, 0, kRootSector, kSectorSize,
                 uint8_t(kFileAttributeDirectoryBit), "dir");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  CHECK(device.Initialize());
}

TEST_CASE("Disc image entry outside its directory is refused", "[filesystem][disc]") {
  TestFile file("rexglue_disc_node.iso");
  auto image = MakeDiscImage();
  StoreDiscEntry(RootDirectory(image), 0, 0xFFFF, kRootSector + 1, 0x10, 0, "a");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("Disc image folder outside the image is refused", "[filesystem][disc]") {
  TestFile file("rexglue_disc_far.iso");
  auto image = MakeDiscImage();
  StoreDiscEntry(RootDirectory(image), 0, 0, 0x10000000, kSectorSize,
                 uint8_t(kFileAttributeDirectoryBit), "dir");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("Disc image entry name that isn't UTF-8 is refused", "[filesystem][disc]") {
  TestFile file("rexglue_disc_name.iso");
  auto image = MakeDiscImage();
  StoreDiscEntry(RootDirectory(image), 0, 0, kRootSector + 1, 0x10, 0, "\xFF\xFE");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("Disc image file past the end reads what is there", "[filesystem][disc]") {
  TestFile file("rexglue_disc_cut.iso");
  auto image = MakeDiscImage();
  // Starts in the last 0x800 bytes and claims 16 KB; a second one starts far away.
  StoreDiscEntry(RootDirectory(image), 0, 8, kRootSector + 2, 0x4000, 0, "cut");
  StoreDiscEntry(RootDirectory(image) + 8 * 4, 0, 0, 0x10000000, 0x4000, 0, "far");
  file.Write(image);

  DiscImageDevice device("\\Device\\Test", file.path);
  REQUIRE(device.Initialize());
  std::vector<uint8_t> buffer(0x4000);

  Entry* cut = device.ResolvePath("cut");
  REQUIRE(cut);
  File* opened = nullptr;
  REQUIRE(cut->Open(1, &opened) == X_STATUS_SUCCESS);
  size_t bytes_read = 0;
  CHECK(opened->ReadSync(std::span<uint8_t>(buffer), 0, &bytes_read) == X_STATUS_SUCCESS);
  CHECK(bytes_read == kSectorSize);
  CHECK(opened->ReadSync(std::span<uint8_t>(buffer), 0x1000, &bytes_read) ==
        X_STATUS_END_OF_FILE);
  opened->Destroy();

  Entry* distant = device.ResolvePath("far");
  REQUIRE(distant);
  REQUIRE(distant->Open(1, &opened) == X_STATUS_SUCCESS);
  CHECK(opened->ReadSync(std::span<uint8_t>(buffer), 0, &bytes_read) == X_STATUS_END_OF_FILE);
  opened->Destroy();
  auto mapped = distant->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
  REQUIRE(mapped);
  CHECK(mapped->size() == 0);
}

TEST_CASE("STFS package with a well-formed directory mounts", "[filesystem][stfs]") {
  TestFile file("rexglue_stfs_ok");
  auto package = MakeStfsPackage();
  StfsDirectoryEntry& entry = FirstStfsEntry(package);
  std::memcpy(entry.name, "a", 1);
  entry.flags.name_length = 1;
  entry.directory_index = 0xFFFF;
  file.Write(package);

  StfsContainerDevice device("\\Device\\Test", file.path);
  REQUIRE(device.Initialize());
  CHECK(device.ResolvePath("a") != nullptr);
}

TEST_CASE("STFS entry with a parent not read yet is refused", "[filesystem][stfs]") {
  TestFile file("rexglue_stfs_parent");
  auto package = MakeStfsPackage();
  StfsDirectoryEntry& entry = FirstStfsEntry(package);
  std::memcpy(entry.name, "a", 1);
  entry.flags.name_length = 1;
  entry.directory_index = 5;
  file.Write(package);

  StfsContainerDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("STFS entry name that isn't UTF-8 is refused", "[filesystem][stfs]") {
  TestFile file("rexglue_stfs_name");
  auto package = MakeStfsPackage();
  StfsDirectoryEntry& entry = FirstStfsEntry(package);
  std::memcpy(entry.name, "\xFF\xFE", 2);
  entry.flags.name_length = 2;
  entry.directory_index = 0xFFFF;
  file.Write(package);

  StfsContainerDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("STFS file table past the end of the package is refused", "[filesystem][stfs]") {
  TestFile file("rexglue_stfs_table");
  // Block 2000 is past the file.
  auto package = MakeStfsPackage(2000);
  file.Write(package);

  StfsContainerDevice device("\\Device\\Test", file.path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("STFS file whose block chain leaves the package still mounts", "[filesystem][stfs]") {
  TestFile file("rexglue_stfs_chain");
  auto package = MakeStfsPackage();
  StfsDirectoryEntry& entry = FirstStfsEntry(package);
  std::memcpy(entry.name, "a", 1);
  entry.flags.name_length = 1;
  entry.directory_index = 0xFFFF;
  // Block 2000's hash table is past the end of the file: GetBlockHash fails.
  entry.set_start_block_number(2000);
  entry.length = 0x3000;
  file.Write(package);

  StfsContainerDevice device("\\Device\\Test", file.path);
  CHECK(device.Initialize());
}
