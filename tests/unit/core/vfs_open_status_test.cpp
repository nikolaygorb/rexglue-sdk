/**
 * Unit tests for the NT status VirtualFileSystem::OpenFile returns for a missing
 * file or parent directory (xenia-canary e91faca1c).
 *
 * Ported for the ReXGlue fork.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;
using rex::filesystem::FileAccess;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::filesystem::HostPathDevice;
using rex::filesystem::VirtualFileSystem;

TEST_CASE("OpenFile reports a missing file and a missing directory apart", "[filesystem][vfs]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("rexglue_vfs_open_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "not_folder") << "file";

  {
    VirtualFileSystem vfs;
    auto device = std::make_unique<HostPathDevice>("\\TestStore", dir, true);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    REQUIRE(vfs.RegisterSymbolicLink("test:", "\\TestStore"));

    rex::filesystem::File* file = nullptr;
    FileAction action = FileAction::kOpened;
    CHECK(vfs.OpenFile(nullptr, "test:\\missing.bin", FileDisposition::kOpen,
                       FileAccess::kGenericRead, false, true, &file,
                       &action) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(action == FileAction::kDoesNotExist);

    action = FileAction::kOpened;
    CHECK(vfs.OpenFile(nullptr, "test:\\missing\\file.bin", FileDisposition::kOpen,
                       FileAccess::kGenericRead, false, true, &file,
                       &action) == X_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(action == FileAction::kDoesNotExist);
    CHECK(file == nullptr);
    CHECK(vfs.OpenFile(nullptr, "test:\\not_folder\\file.bin", FileDisposition::kOpen,
                       FileAccess::kGenericRead, false, true, &file,
                       &action) == X_STATUS_OBJECT_PATH_NOT_FOUND);
  }

  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST_CASE("VFS opens raw mount roots and creates under their resolved parents", "[filesystem][vfs]") {
  struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("rexglue_vfs_raw_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  } temp;
  std::filesystem::create_directories(temp.path / "folder");
  VirtualFileSystem vfs;
  const std::string mount = "\\Device\\Harddisk0\\Partition1\\Content";
  auto device = std::make_unique<HostPathDevice>(mount, temp.path, false, true);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  auto close = [](rex::filesystem::File* file) { if (file) file->Destroy(); };
  rex::filesystem::File* raw = nullptr;
  FileAction action{};
  REQUIRE(vfs.OpenFile(nullptr, mount, FileDisposition::kOpen, FileAccess::kGenericRead,
                       true, false, &raw, &action) == X_STATUS_SUCCESS);
  std::unique_ptr<rex::filesystem::File, decltype(close)> root(raw, close);
  REQUIRE_FALSE(root->entry()->Delete());
  root.reset();

  const auto path = mount + "\\folder\\new.bin";
  REQUIRE(vfs.OpenFile(nullptr, path, FileDisposition::kCreate, FileAccess::kGenericWrite,
                       false, true, &raw, &action) == X_STATUS_SUCCESS);
  std::unique_ptr<rex::filesystem::File, decltype(close)> first(raw, close);
  REQUIRE(action == FileAction::kCreated);
  REQUIRE(vfs.OpenFile(nullptr, path, FileDisposition::kOpen, FileAccess::kGenericRead,
                       false, true, &raw, &action) == X_STATUS_SUCCESS);
  std::unique_ptr<rex::filesystem::File, decltype(close)> second(raw, close);
  REQUIRE(vfs.DeletePath(path));
  first.reset();
  REQUIRE(std::filesystem::exists(temp.path / "folder" / "new.bin"));
  REQUIRE(second->entry()->name() == "new.bin");
  second.reset();
  REQUIRE_FALSE(std::filesystem::exists(temp.path / "folder" / "new.bin"));

  // This child is deliberately absent from the VFS's cached directory list.
  std::ofstream(temp.path / "folder" / "external.bin") << "keep";
  REQUIRE_FALSE(vfs.DeletePath(mount + "\\folder"));
  REQUIRE(std::filesystem::exists(temp.path / "folder" / "external.bin"));
}
