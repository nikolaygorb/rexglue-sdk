/**
 * @file        launcher/launcher_process.cpp
 * @brief       Starts the game and waits for it
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "launcher_process.h"

#include <fmt/format.h>

#include <rex/filesystem.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h: WIN32_LEAN_AND_MEAN leaves it out.
#include <shellapi.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rex::launcher {

#ifdef _WIN32

namespace {

// One argument quoted the way CommandLineToArgvW splits it back.
void AppendQuoted(std::wstring& command_line, const std::wstring& arg) {
  if (!command_line.empty()) {
    command_line.push_back(L' ');
  }
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    command_line += arg;
    return;
  }
  command_line.push_back(L'"');
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      command_line.append(backslashes * 2 + 1, L'\\');
    } else {
      command_line.append(backslashes, L'\\');
    }
    backslashes = 0;
    command_line.push_back(c);
  }
  command_line.append(backslashes * 2, L'\\');
  command_line.push_back(L'"');
}

}  // namespace

GameProcess::~GameProcess() {
  if (process_) {
    CloseHandle(static_cast<HANDLE>(process_));
  }
}

bool GameProcess::Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                        const std::filesystem::path& working_dir, std::string& error) {
  std::wstring command_line;
  AppendQuoted(command_line, exe.wstring());
  for (const std::string& arg : args) {
    AppendQuoted(command_line, rex::to_path(arg).wstring());
  }
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  const std::wstring dir = working_dir.wstring();
  if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir.c_str(), &startup_info, &process_info)) {
    error = fmt::format("Can't start {} (error {})", rex::path_to_utf8(exe.filename()),
                        GetLastError());
    return false;
  }
  CloseHandle(process_info.hThread);
  process_ = process_info.hProcess;
  return true;
}

int GameProcess::Wait() {
  if (!process_) {
    return -1;
  }
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
  return static_cast<int>(code);
}

std::string DescribeExitCode(int code) {
  const auto value = static_cast<uint32_t>(code);
  switch (value) {
    case 0xC0000005:
      return "0xC0000005 (access violation)";
    case 0xC00000FD:
      return "0xC00000FD (stack overflow)";
    case 0xC0000409:
      return "0xC0000409 (stack buffer overrun / fail fast)";
    case 0xC000001D:
      return "0xC000001D (illegal instruction)";
    case 0x80000003:
      return "0x80000003 (breakpoint)";
    default:
      break;
  }
  if (value >= 0xC0000000) {
    return fmt::format("0x{:08X}", value);
  }
  return std::to_string(code);
}

void OpenFolder(const std::filesystem::path& path) {
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else

GameProcess::~GameProcess() = default;

bool GameProcess::Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                        const std::filesystem::path& working_dir, std::string& error) {
  // Everything the child needs is built before fork: after it, only
  // async-signal-safe calls.
  const std::string exe_path = exe.string();
  const std::string dir = working_dir.string();
  std::vector<std::string> storage;
  storage.push_back(exe_path);
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    error = fmt::format("Can't start {}: {}", exe.filename().string(), std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    if (chdir(dir.c_str()) != 0) {
      _exit(127);
    }
    execv(exe_path.c_str(), argv.data());
    _exit(127);
  }
  pid_ = pid;
  return true;
}

int GameProcess::Wait() {
  if (pid_ < 0) {
    return -1;
  }
  int status = 0;
  while (waitpid(pid_, &status, 0) < 0) {
    if (errno != EINTR) {
      pid_ = -1;
      return -1;
    }
  }
  pid_ = -1;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

std::string DescribeExitCode(int code) {
  if (code > 128 && code < 128 + 64) {
    return fmt::format("{} (signal {})", code, code - 128);
  }
  return std::to_string(code);
}

void OpenFolder(const std::filesystem::path& path) {
  const std::string target = path.string();
  const pid_t pid = fork();
  if (pid == 0) {
    // A grandchild runs the opener and the child exits at once, so the
    // launcher has nothing left to reap.
    if (fork() == 0) {
#ifdef __APPLE__
      execlp("open", "open", target.c_str(), static_cast<char*>(nullptr));
#else
      execlp("xdg-open", "xdg-open", target.c_str(), static_cast<char*>(nullptr));
#endif
      _exit(127);
    }
    _exit(0);
  }
  if (pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);
  }
}

#endif

}  // namespace rex::launcher
