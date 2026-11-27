// Copyright (c) 2026 ReXGlue fork contributors. BSD 3-Clause; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace rex::memory {
class Memory;
}
namespace rex::system::xam {

// Title-owned XAM allocations share a bounded backing region, with host metadata.
class WorkspaceHeap {
 public:
  static uint32_t ResolveSize(uint32_t header_size);
  static std::unique_ptr<WorkspaceHeap> Create(memory::Memory& memory, uint32_t header_size = 0);
  ~WorkspaceHeap();
  WorkspaceHeap(const WorkspaceHeap&) = delete;
  WorkspaceHeap& operator=(const WorkspaceHeap&) = delete;
  uint32_t Allocate(uint32_t size, bool zero = true);
  bool Free(uint32_t address);
  bool Contains(uint32_t address) const { return address >= base_ && address - base_ < size_; }
  uint32_t size() const { return size_; }

 private:
  WorkspaceHeap(memory::Memory& memory, uint32_t base, uint32_t size);
  memory::Memory& memory_;
  uint32_t base_, size_;
  std::mutex mutex_;
  std::map<uint32_t, uint32_t> free_, live_;
};
}  // namespace rex::system::xam
