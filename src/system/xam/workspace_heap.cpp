// Based on the workspace behavior of rexglue/rexglue-sdk b025c4a8 / f305a281.
// Copyright (c) 2026 ReXGlue fork contributors. BSD 3-Clause; see LICENSE.
#include <rex/system/xam/workspace_heap.h>

#include <algorithm>
#include <iterator>

#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

namespace rex::system::xam {

uint32_t WorkspaceHeap::ResolveSize(uint32_t header_size) {
  if (!header_size)
    return 384 * 1024;
  const uint64_t size = (uint64_t(std::max(header_size, 256u * 1024)) + 16383) & ~uint64_t(16383);
  return size <= 0x2FFE0000 ? static_cast<uint32_t>(size) : 0;
}

std::unique_ptr<WorkspaceHeap> WorkspaceHeap::Create(memory::Memory& memory, uint32_t header_size) {
  auto global_lock = thread::global_critical_region::AcquireDirect();
  const uint32_t size = ResolveSize(header_size);
  if (!size)
    return nullptr;
  auto* heap = memory.LookupHeapByType(false, 4096);
  uint32_t base = 0;
  if (!heap ||
      !heap->AllocRange(0x10000, 0x2FFEFFFF, size, 4096,
                        memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                        memory::kMemoryProtectRead | memory::kMemoryProtectWrite, true, &base))
    return nullptr;
  return std::unique_ptr<WorkspaceHeap>(new WorkspaceHeap(memory, base, size));
}

WorkspaceHeap::WorkspaceHeap(memory::Memory& memory, uint32_t base, uint32_t size)
    : memory_(memory), base_(base), size_(size) {
  free_.emplace(0, size);
}

WorkspaceHeap::~WorkspaceHeap() {
  auto global_lock = thread::global_critical_region::AcquireDirect();
  memory_.LookupHeap(base_)->Release(base_);
}

uint32_t WorkspaceHeap::Allocate(uint32_t size, bool zero) {
  const uint64_t rounded = (uint64_t(std::max(size, 1u)) + 15) & ~uint64_t(15);
  if (rounded > size_)
    return 0;
  auto global_lock = thread::global_critical_region::AcquireDirect();
  std::lock_guard lock(mutex_);
  auto best = free_.end();
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second >= rounded && (best == free_.end() || it->second < best->second))
      best = it;
  }
  if (best == free_.end())
    return 0;
  const auto [offset, available] = *best;
  const uint32_t count = static_cast<uint32_t>(rounded);
  free_.erase(best);
  if (available > count)
    free_.emplace(offset + count, available - count);
  live_.emplace(offset, count);
  if (zero)
    memory_.Zero(base_ + offset, std::max(size, 1u));
  return base_ + offset;
}

bool WorkspaceHeap::Free(uint32_t address) {
  if (!Contains(address))
    return false;
  auto global_lock = thread::global_critical_region::AcquireDirect();
  std::lock_guard lock(mutex_);
  const uint32_t offset = address - base_;
  auto live = live_.find(offset);
  if (live == live_.end())
    return false;
  uint32_t first = offset, count = live->second;
  live_.erase(live);
  auto next = free_.lower_bound(offset);
  if (next != free_.begin()) {
    const auto previous = std::prev(next);
    if (previous->first + previous->second == first) {
      first = previous->first;
      count += previous->second;
      free_.erase(previous);
    }
  }
  if (next != free_.end() && first + count == next->first) {
    count += next->second;
    free_.erase(next);
  }
  free_.emplace(first, count);
  return true;
}
}  // namespace rex::system::xam
