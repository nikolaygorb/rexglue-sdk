/**
 * @file        launcher/launcher_process.h
 * @brief       Starts the game and waits for it
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rex::launcher {

class GameProcess {
 public:
  GameProcess() = default;
  GameProcess(const GameProcess&) = delete;
  GameProcess& operator=(const GameProcess&) = delete;
  ~GameProcess();

  // Starts `exe` with `args` in `working_dir`, without the launcher's console
  // or handles. false with `error` set when it can't be started.
  bool Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
             const std::filesystem::path& working_dir, std::string& error);

  // Blocks until the game exits; its exit code (a signal on Linux: 128 + it).
  int Wait();

 private:
#ifdef _WIN32
  void* process_ = nullptr;  // HANDLE
#else
  int pid_ = -1;
#endif
};

// "0xC0000005 (access violation)" style text for a Windows exit code, the
// number otherwise.
std::string DescribeExitCode(int code);

// Opens a folder in the system's file manager (Explorer, xdg-open, open).
void OpenFolder(const std::filesystem::path& path);

}  // namespace rex::launcher
