#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution /*mode*/) {
  // Windows resolves all imports at load time; mode is informational only.
  Close();
  handle_ = static_cast<void*>(LoadLibraryW(path.c_str()));
  if (!handle_) {
    last_error_ = "LoadLibraryW error " + std::to_string(GetLastError());
    return false;
  }
  last_error_.clear();
  return true;
}

bool DynamicLibrary::Attach(const std::filesystem::path& path, SymbolResolution /*mode*/) {
  // Flag 0 increments the module refcount, so FreeLibrary in Close() stays balanced.
  Close();
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(0, path.c_str(), &module)) {
    last_error_ = "GetModuleHandleExW error " + std::to_string(GetLastError());
    return false;
  }
  handle_ = static_cast<void*>(module);
  last_error_.clear();
  return true;
}

void DynamicLibrary::Close() {
  if (handle_) {
    FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
}

}  // namespace rex::platform
