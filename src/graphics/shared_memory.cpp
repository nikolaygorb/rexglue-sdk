/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/hash.h>
#include <rex/math.h>
#include <rex/memory.h>

REXCVAR_DEFINE_INT32(gpu_memory_watch_bucket_log2, 22, "GPU",
                     "Size of the buckets of guest memory watches (textures and other GPU copies "
                     "of guest memory), log2 of bytes: a write to guest memory tests every watch "
                     "of its buckets. 22 (4 MB) was the fixed size; smaller buckets test fewer "
                     "watches per write and link a few more nodes per watch")
    .range(18, 22)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(shared_memory_hot_pages, false, "GPU",
                    "Don't write-protect pages the CPU rewrites every frame (such as dynamic "
                    "vertex data): compare them with their last upload when requested instead. "
                    "Vulkan and Direct3D 12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Idea from the WWE 2K14 recomp project (gpu_upload_skip_unchanged, session
// 7b): with clear_memory_page_state most re-uploads carry the bytes the buffer
// already holds.
REXCVAR_DEFINE_BOOL(shared_memory_skip_unchanged_uploads, false, "GPU",
                    "Don't upload a shared memory page whose guest bytes hash (XXH3) the same "
                    "as its last upload, when the GPU hasn't written that page of the buffer "
                    "since: the page is made valid and protected as by an upload. Removes most "
                    "of the re-uploads of clear_memory_page_state. Implies "
                    "shared_memory_hash_uploads. Vulkan and Direct3D 12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(shared_memory_hash_uploads, false, "GPU",
                    "Hash shared memory uploads without skipping any, so the upload line of "
                    "frame_stats_interval counts the pages clear_memory_page_state's frame-end "
                    "reset uploaded again with changed bytes: the ones that would be stale with "
                    "it off. Vulkan and Direct3D 12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Idea from the WWE 2K14 recomp project (gpu_upload_skip_parallel_hash,
// session 19).
REXCVAR_DEFINE_INT32(shared_memory_hash_threads, 0, "GPU",
                     "With shared_memory_skip_unchanged_uploads or shared_memory_hash_uploads: "
                     "hash large requests on this many worker threads besides the GPU thread "
                     "(0 = only the GPU thread). Same result; for games whose GPU thread is "
                     "the limit")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(shared_memory_hash_parallel_min_pages, 512, "GPU",
                     "shared_memory_hash_threads: fewest 4 KB pages hashed at once that are "
                     "spread over the workers (smaller batches are hashed on the GPU thread: "
                     "waking the workers costs more than it saves)")
    .range(64, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

// Hashes a batch of pages on worker threads and the calling thread together.
// One batch at a time, from the GPU thread.
class SharedMemory::HashWorkers {
 public:
  explicit HashWorkers(uint32_t thread_count) {
    threads_.reserve(thread_count);
    for (uint32_t i = 0; i < thread_count; ++i) {
      threads_.emplace_back([this] { WorkerMain(); });
    }
  }
  ~HashWorkers() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& thread : threads_) {
      thread.join();
    }
  }

  // Calls work(begin, end) over [0, count) in chunks, returning when all are
  // done.
  void Run(uint32_t count, const std::function<void(uint32_t, uint32_t)>& work) {
    {
      std::lock_guard lock(mutex_);
      work_ = &work;
      count_ = count;
      next_.store(0, std::memory_order_relaxed);
      workers_busy_ = uint32_t(threads_.size());
      ++generation_;
    }
    work_cv_.notify_all();
    DoChunks(work, count);
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return workers_busy_ == 0; });
    work_ = nullptr;
  }

 private:
  static constexpr uint32_t kChunkPages = 32;

  void DoChunks(const std::function<void(uint32_t, uint32_t)>& work, uint32_t count) {
    for (;;) {
      uint32_t begin = next_.fetch_add(kChunkPages, std::memory_order_relaxed);
      if (begin >= count) {
        return;
      }
      work(begin, std::min(begin + kChunkPages, count));
    }
  }

  void WorkerMain() {
    uint64_t seen_generation = 0;
    for (;;) {
      const std::function<void(uint32_t, uint32_t)>* work;
      uint32_t count;
      {
        std::unique_lock lock(mutex_);
        work_cv_.wait(lock, [&] { return stop_ || generation_ != seen_generation; });
        if (stop_) {
          return;
        }
        seen_generation = generation_;
        work = work_;
        count = count_;
      }
      DoChunks(*work, count);
      {
        std::lock_guard lock(mutex_);
        if (--workers_busy_ == 0) {
          done_cv_.notify_one();
        }
      }
    }
  }

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  const std::function<void(uint32_t, uint32_t)>* work_ = nullptr;
  uint32_t count_ = 0;
  std::atomic<uint32_t> next_{0};
  uint32_t workers_busy_ = 0;
  uint64_t generation_ = 0;
  bool stop_ = false;
};

namespace {
// The bits of 64-page block i that are in [page_first, page_last].
uint64_t PageBlockMask(uint32_t i, uint32_t page_first, uint32_t page_last) {
  uint64_t mask = UINT64_MAX;
  if (i == page_first >> 6) {
    mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
  }
  if (i == page_last >> 6 && (page_last & 63) != 63) {
    mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
  }
  return mask;
}
}  // namespace

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
  watch_bucket_size_log2_ =
      std::max(std::min(uint32_t(std::max(REXCVAR_GET(gpu_memory_watch_bucket_log2), 0)),
                        kWatchBucketSizeLog2Max),
               std::max(kWatchBucketSizeLog2Min, page_size_log2_));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  // Never valid yet, so not invalidated by the reset only.
  page_invalidated_.assign(num_system_page_flags_, UINT64_MAX);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  page_invalidated_.clear();
  page_invalidated_.shrink_to_fit();
  num_system_page_flags_ = 0;

  upload_hashing_enabled_ = false;
  upload_skip_unchanged_ = false;
  page_upload_hashes_.clear();
  page_upload_hashes_.shrink_to_fit();
  page_upload_hash_valid_.clear();
  page_upload_hash_valid_.shrink_to_fit();
  page_stats_upload_frame_.clear();
  page_stats_upload_frame_.shrink_to_fit();

  hot_pages_enabled_ = false;
  hot_pages_.clear();
  hot_pages_.shrink_to_fit();
  hot_page_count_ = 0;
  page_last_upload_frame_.clear();
  page_last_upload_frame_.shrink_to_fit();
  page_last_change_frame_.clear();
  page_last_change_frame_.shrink_to_fit();
  page_upload_streak_.clear();
  page_upload_streak_.shrink_to_fit();
  page_last_compare_sync_point_.clear();
  page_last_compare_sync_point_.shrink_to_fit();
  page_cpu_written_.clear();
  page_cpu_written_.shrink_to_fit();
  hot_page_shadows_.clear();
}

void SharedMemory::EnableHotPages() {
  if (!REXCVAR_GET(shared_memory_hot_pages)) {
    return;
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  hot_pages_.assign(num_system_page_flags_, 0);
  page_last_upload_frame_.assign(page_count, 0);
  page_last_change_frame_.assign(page_count, 0);
  page_upload_streak_.assign(page_count, 0);
  page_last_compare_sync_point_.assign(page_count, 0);
  page_cpu_written_.assign(num_system_page_flags_, 0);
  hot_pages_enabled_ = true;
}

void SharedMemory::EnableUploadHashing() {
  upload_skip_unchanged_ = REXCVAR_GET(shared_memory_skip_unchanged_uploads);
  if (!upload_skip_unchanged_ && !REXCVAR_GET(shared_memory_hash_uploads)) {
    return;
  }
  page_upload_hashes_.assign(kBufferSize >> page_size_log2_, 0);
  page_upload_hash_valid_.assign(num_system_page_flags_, 0);
  upload_hashing_enabled_ = true;
  if (int32_t hash_threads = REXCVAR_GET(shared_memory_hash_threads); hash_threads > 0) {
    hash_workers_ = std::make_unique<HashWorkers>(uint32_t(hash_threads));
  }
}

void SharedMemory::HashPages(uint32_t page_first, uint32_t page_count,
                             const std::function<void(uint32_t, uint32_t)>& work) {
  if (hash_workers_ &&
      page_count >= uint32_t(REXCVAR_GET(shared_memory_hash_parallel_min_pages))) {
    hash_workers_->Run(page_count, [&](uint32_t begin, uint32_t end) {
      work(page_first + begin, page_first + end);
    });
    return;
  }
  work(page_first, page_first + page_count);
}

uint64_t SharedMemory::HashPage(uint32_t page) const {
  const size_t page_size = size_t(1) << page_size_log2_;
  return XXH3_64bits(memory_.TranslatePhysical(page << page_size_log2_), page_size);
}

void SharedMemory::InvalidateUploadHashes(uint32_t page_first, uint32_t page_last) {
  if (!upload_hashing_enabled_) {
    return;
  }
  for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
    page_upload_hash_valid_[i] &= ~PageBlockMask(i, page_first, page_last);
  }
}

void SharedMemory::AdvanceHotPageFrame() {
  ++hot_page_frame_;
  if (!hot_page_count_ || (hot_page_frame_ % kHotPageSweepFrames)) {
    return;
  }
  // Hot pages are only compared (and demoted when static) when requested, so
  // demote the ones no longer requested here.
  auto global_lock = global_critical_region_.Acquire();
  std::vector<uint32_t> idle_pages;
  for (const auto& [page, shadow] : hot_page_shadows_) {
    if (hot_page_frame_ - page_last_upload_frame_[page] > kHotPageIdleFrames) {
      idle_pages.push_back(page);
    }
  }
  for (uint32_t page : idle_pages) {
    DemoteHotPages(page, page);
  }
}

void SharedMemory::CopyPagesForUpload(uint8_t* dst, uint32_t page_first, uint32_t page_count) {
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  uint32_t run_first = page_first;
  auto copy_run = [&](uint32_t run_end) {
    if (run_end > run_first) {
      std::memcpy(dst + ((run_first - page_first) << page_size_log2_),
                  memory_.TranslatePhysical(run_first << page_size_log2_),
                  (run_end - run_first) << page_size_log2_);
    }
  };
  if (hot_page_count_) {
    for (uint32_t page = page_first; page < page_first + page_count; ++page) {
      if (!IsPageHot(page)) {
        continue;
      }
      copy_run(page);
      auto it = hot_page_shadows_.find(page);
      assert_true(it != hot_page_shadows_.end());
      std::memcpy(dst + ((page - page_first) << page_size_log2_), it->second.get(), page_size);
      run_first = page + 1;
    }
  }
  copy_run(page_first + page_count);

  if (!upload_hashing_enabled_ || !page_count) {
    return;
  }
  // Hashed from guest memory, not from the write-combined upload buffer. The
  // pages were made valid (and protected) before the copy, so if a page is
  // still valid now, no write changed it during the copy and the hashing, and
  // the hash is of the copied bytes. Hot pages are never valid.
  uint32_t page_last = page_first + page_count - 1;
  HashPages(page_first, page_count, [this](uint32_t begin, uint32_t end) {
    for (uint32_t page = begin; page < end; ++page) {
      page_upload_hashes_[page] = HashPage(page);
    }
  });
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
    uint64_t mask = PageBlockMask(i, page_first, page_last);
    page_upload_hash_valid_[i] =
        (page_upload_hash_valid_[i] & ~mask) | (system_page_flags_valid_[i] & mask);
  }
}

bool SharedMemory::IsPageWatched(uint32_t page) const {
  WatchNode* node = watch_buckets_[page << page_size_log2_ >> watch_bucket_size_log2_];
  for (; node != nullptr; node = node->bucket_node_next) {
    if (page >= node->page_first && page <= node->page_last) {
      return true;
    }
  }
  return false;
}

void SharedMemory::DemoteHotPages(uint32_t page_first, uint32_t page_last) {
  if (!hot_page_count_) {
    return;
  }
  // Demoted pages are not valid (hot pages never are), so they are uploaded and
  // protected again when requested next time; protect them now already, so
  // writes before that reach the watches. One call per run of pages.
  uint32_t run_first = UINT32_MAX;
  auto protect_run = [&](uint32_t run_end) {
    if (run_first != UINT32_MAX) {
      if (memory_invalidation_callback_handle_) {
        memory().EnablePhysicalMemoryAccessCallbacks(
            run_first << page_size_log2_, (run_end - run_first) << page_size_log2_, true, false);
      }
      run_first = UINT32_MAX;
    }
  };
  uint32_t page = page_first;
  for (; page <= page_last && hot_page_count_; ++page) {
    uint64_t& block = hot_pages_[page >> 6];
    uint64_t bit = uint64_t(1) << (page & 63);
    if (!(block & bit)) {
      protect_run(page);
      continue;
    }
    block &= ~bit;
    --hot_page_count_;
    hot_page_shadows_.erase(page);
    page_upload_streak_[page] = 0;
    if (run_first == UINT32_MAX) {
      run_first = page;
    }
  }
  protect_run(page);
}

void SharedMemory::FilterHotPageUploads(const std::unique_lock<std::recursive_mutex>& global_lock) {
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  hot_page_upload_ranges_.clear();
  auto include = [this](uint32_t page) {
    if (!hot_page_upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last = hot_page_upload_ranges_.back();
      if (last.first + last.second == page) {
        ++last.second;
        return;
      }
    }
    hot_page_upload_ranges_.emplace_back(page, 1);
  };
  for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
    for (uint32_t page = range.first; page < range.first + range.second; ++page) {
      const uint8_t* guest = memory_.TranslatePhysical(page << page_size_log2_);
      uint64_t& cpu_written_block = page_cpu_written_[page >> 6];
      const uint64_t page_bit = uint64_t(1) << (page & 63);
      const bool cpu_written = (cpu_written_block & page_bit) != 0;
      cpu_written_block &= ~page_bit;
      if (IsPageHot(page)) {
        page_last_upload_frame_[page] = hot_page_frame_;
        // Compared since the last point where it could have changed: the
        // buffer already holds it.
        if (page_last_compare_sync_point_[page] == hot_page_sync_point_) {
          continue;
        }
        page_last_compare_sync_point_[page] = hot_page_sync_point_;
        uint8_t* shadow = hot_page_shadows_[page].get();
        if (std::memcmp(guest, shadow, page_size) == 0) {
          if (hot_page_frame_ - page_last_change_frame_[page] > kHotPageIdleFrames) {
            // Static now: upload and protect it like other pages.
            DemoteHotPages(page, page);
            include(page);
          }
          continue;
        }
        std::memcpy(shadow, guest, page_size);
        page_last_change_frame_[page] = hot_page_frame_;
        // No access violation reported this write, so notify the global
        // watches here.
        for (const GlobalWatch* global_watch : global_watches_) {
          global_watch->callback(global_lock, global_watch->callback_context,
                                 page << page_size_log2_, (page << page_size_log2_) + page_size - 1,
                                 false);
        }
        include(page);
        continue;
      }
      // Only uploads after a CPU write count: with clear_memory_page_state,
      // every page a frame uses is uploaded again each frame, written or not.
      uint32_t last_upload_frame = page_last_upload_frame_[page];
      if (cpu_written && last_upload_frame != hot_page_frame_) {
        uint8_t& streak = page_upload_streak_[page];
        streak = last_upload_frame + 1 == hot_page_frame_ ? uint8_t(std::min(streak + 1, 255))
                                                          : uint8_t(1);
        page_last_upload_frame_[page] = hot_page_frame_;
        if (streak >= kHotPageUploadStreak &&
            hot_page_count_ < (kMaxHotPageBytes >> page_size_log2_) &&
            !((system_page_flags_valid_and_gpu_written_[page >> 6] >> (page & 63)) & 1) &&
            !IsPageWatched(page)) {
          auto shadow = std::make_unique<uint8_t[]>(page_size);
          std::memcpy(shadow.get(), guest, page_size);
          hot_page_shadows_[page] = std::move(shadow);
          hot_pages_[page >> 6] |= page_bit;
          ++hot_page_count_;
          page_last_change_frame_[page] = hot_page_frame_;
          page_last_compare_sync_point_[page] = hot_page_sync_point_;
        }
      }
      include(page);
    }
  }
  upload_ranges_.swap(hot_page_upload_ranges_);
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();
  DemoteHotPages(0, (kBufferSize >> page_size_log2_) - 1);

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
  // The buffer is not touched, so the upload hashes stay.
  std::fill(page_invalidated_.begin(), page_invalidated_.end(), UINT64_MAX);
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  {
    // Done with clear_memory_page_state off too, unlike the frame-end reset.
    auto global_lock = global_critical_region_.Acquire();
    std::fill(page_invalidated_.begin(), page_invalidated_.end(), UINT64_MAX);
  }
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> watch_bucket_size_log2_;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> watch_bucket_size_log2_;

  auto global_lock = global_critical_region_.Acquire();

  DemoteHotPages(watch_page_first, watch_page_last);

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    node->page_first = watch_page_first;
    node->page_last = watch_page_last;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> watch_bucket_size_log2_;
  uint32_t bucket_last = address_last >> watch_bucket_size_log2_;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      // The node's copy of the range's pages first: most nodes are of other
      // ranges.
      bool overlaps = page_first <= node->page_last && page_last >= node->page_first;
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (overlaps) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

const uint8_t* SharedMemory::TranslatePhysical(uint32_t address) const {
  return memory_.TranslatePhysical<const uint8_t*>(address);
}

bool SharedMemory::IsRangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t gpu_written_bits = system_page_flags_valid_and_gpu_written_[i];
    if (i == block_first) {
      gpu_written_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      gpu_written_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (gpu_written_bits) {
      return true;
    }
  }
  return false;
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  bool any_hot = false;
  {
    auto global_lock = global_critical_region_.Acquire();

    if (written_by_gpu) {
      // The buffer now holds the newer data: nothing to compare with.
      DemoteHotPages(valid_page_first, valid_page_last);
      InvalidateUploadHashes(valid_page_first, valid_page_last);
    }
    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      if (hot_page_count_ && (hot_pages_[i] & valid_bits)) {
        valid_bits &= ~hot_pages_[i];
        any_hot = true;
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
      page_invalidated_[i] &= ~valid_bits;
    }
  }

  if (memory_invalidation_callback_handle_) {
    if (!any_hot) {
      memory().EnablePhysicalMemoryAccessCallbacks(
          valid_page_first << page_size_log2_,
          (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
    } else {
      // Protect only the runs of pages that aren't hot.
      uint32_t run_first = UINT32_MAX;
      for (uint32_t page = valid_page_first; page <= valid_page_last + 1; ++page) {
        bool protect = page <= valid_page_last && !IsPageHot(page);
        if (protect && run_first == UINT32_MAX) {
          run_first = page;
        } else if (!protect && run_first != UINT32_MAX) {
          memory().EnablePhysicalMemoryAccessCallbacks(
              run_first << page_size_log2_, (page - run_first) << page_size_log2_, true, false);
          run_first = UINT32_MAX;
        }
      }
    }
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> watch_bucket_size_log2_;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                                 RequestKind kind) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Lockless fast path: nothing to upload if every requested page is already
  // valid, which is the common case. Writers still change the flags under the
  // global lock; a page invalidated right after this check is the same race as
  // one invalidated right after the locked check below. Host memory is still
  // allocated first: a range marked valid as a whole (memexport with unknown
  // ranges) may not be backed by sparse memory yet.
  bool all_valid = true;
  for (size_t i = 0; i < count && all_valid; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length ||
        !EnsureHostGpuMemoryAllocated(start, length)) {
      return false;
    }
    all_valid = IsRangeValid(start, length);
  }
  if (all_valid) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i];
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
    if (hot_pages_enabled_ && !upload_ranges_.empty()) {
      FilterHotPageUploads(global_lock);
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  if (TraceWriter* trace = TraceWriter::active()) {
    for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
      trace->WriteMemoryRead(range.first << page_size_log2_, range.second << page_size_log2_);
    }
  }

  bool prevalidated = false;
  if (bool gather_stats = upload_stats_enabled_; upload_hashing_enabled_ || gather_stats) {
    prevalidated = FilterUnchangedUploads(gather_stats);
    if (gather_stats) {
      CountUploads(kind);
    }
    if (upload_ranges_.empty()) {
      return true;
    }
  }

  if (!UploadRanges(upload_ranges_)) {
    if (prevalidated) {
      // UploadRanges makes pages valid only once it has buffer space for them,
      // but the ones made valid for the comparison may be left without their
      // copy: request them again next time.
      auto global_lock = global_critical_region_.Acquire();
      for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
        uint32_t page_last = range.first + range.second - 1;
        for (uint32_t i = range.first >> 6; i <= page_last >> 6; ++i) {
          uint64_t mask = PageBlockMask(i, range.first, page_last);
          system_page_flags_valid_[i] &= ~mask;
          page_invalidated_[i] |= mask;
        }
      }
    }
    return false;
  }
  return true;
}

bool SharedMemory::FilterUnchangedUploads(bool gather_stats) {
  // No skipping while a frame trace records what the frame uploads.
  const bool skip = upload_skip_unchanged_ && !TraceWriter::active();
  if (!skip && !gather_stats) {
    return false;
  }
  bool prevalidated = false;
  unchanged_filter_ranges_.clear();
  auto keep = [this](uint32_t page_start, uint32_t page_count) {
    if (!unchanged_filter_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last = unchanged_filter_ranges_.back();
      if (last.first + last.second == page_start) {
        last.second += page_count;
        return;
      }
    }
    unchanged_filter_ranges_.emplace_back(page_start, page_count);
  };
  // For the statistics; read without the lock, so a write invalidating a page
  // right now may be missed, as by the lockless check in RequestRanges.
  auto reset_only = [this](uint32_t page) {
    return !((page_invalidated_[page >> 6] >> (page & 63)) & 1);
  };
  auto hashed = [this](uint32_t page) { return IsPageUploadHashValid(page) && !IsPageHot(page); };
  for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
    uint32_t range_end = range.first + range.second;
    uint32_t run_first = range.first;
    while (run_first < range_end) {
      // A run of pages that all have a hash, or none of which have one.
      bool run_hashed = hashed(run_first);
      uint32_t run_end = run_first + 1;
      while (run_end < range_end && hashed(run_end) == run_hashed) {
        ++run_end;
      }
      if (!run_hashed) {
        if (gather_stats) {
          for (uint32_t page = run_first; page < run_end; ++page) {
            upload_stats_.requested_after_reset_only += reset_only(page);
          }
        }
        keep(run_first, run_end - run_first);
        run_first = run_end;
        continue;
      }
      // Bit 0: invalidated by the reset only, bit 1: unchanged. The reset-only
      // bits are taken before MakeRangeValid clears them.
      unchanged_filter_flags_.assign(run_end - run_first, 0);
      if (gather_stats) {
        for (uint32_t page = run_first; page < run_end; ++page) {
          unchanged_filter_flags_[page - run_first] = uint8_t(reset_only(page));
        }
      }
      if (skip) {
        // Valid and protected before the comparison, as before an upload's
        // copy, so a write from here on invalidates the page again.
        MakeRangeValid(run_first << page_size_log2_, (run_end - run_first) << page_size_log2_,
                       false);
      }
      // Each page's flags byte is written by one thread only.
      HashPages(run_first, run_end - run_first, [this, run_first](uint32_t begin, uint32_t end) {
        for (uint32_t page = begin; page < end; ++page) {
          if (HashPage(page) == page_upload_hashes_[page]) {
            unchanged_filter_flags_[page - run_first] |= 2;
          }
        }
      });
      if (gather_stats) {
        for (uint8_t flags : unchanged_filter_flags_) {
          upload_stats_.requested_after_reset_only += flags & 1;
          upload_stats_.unchanged += flags >> 1;
          upload_stats_.changed_after_reset_only += flags == 1;
        }
      }
      if (!skip) {
        keep(run_first, run_end - run_first);
        run_first = run_end;
        continue;
      }
      auto global_lock = global_critical_region_.Acquire();
      for (uint32_t page = run_first; page < run_end; ++page) {
        // Not valid anymore if written during the comparison.
        if (!(unchanged_filter_flags_[page - run_first] & 2) ||
            !((system_page_flags_valid_[page >> 6] >> (page & 63)) & 1)) {
          keep(page, 1);
          prevalidated = true;
        }
      }
      run_first = run_end;
    }
  }
  upload_ranges_.swap(unchanged_filter_ranges_);
  return prevalidated;
}

void SharedMemory::CountUploads(RequestKind kind) {
  if (page_stats_upload_frame_.empty()) {
    page_stats_upload_frame_.assign(kBufferSize >> page_size_log2_, 0);
  }
  for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
    upload_stats_.uploaded_by_kind[size_t(kind)] += range.second;
    for (uint32_t page = range.first; page < range.first + range.second; ++page) {
      uint32_t& upload_frame = page_stats_upload_frame_[page];
      upload_stats_.uploaded_again_in_frame += upload_frame == hot_page_frame_;
      upload_frame = hot_page_frame_;
    }
  }
}

SharedMemory::UploadStats SharedMemory::TakeUploadStats() {
  UploadStats stats = upload_stats_;
  upload_stats_ = UploadStats();
  return stats;
}

std::string SharedMemory::FormatUploadStats(const UploadStats& stats, size_t frames) const {
  const double mb_per_frame_per_page =
      frames ? double(uint64_t(1) << page_size_log2_) / double(1 << 20) / double(frames) : 0.0;
  auto mb = [&](uint64_t pages) { return double(pages) * mb_per_frame_per_page; };
  const uint64_t* kinds = stats.uploaded_by_kind;
  uint64_t uploaded = 0;
  for (uint64_t pages : stats.uploaded_by_kind) {
    uploaded += pages;
  }
  std::string line = fmt::format(
      "{:.2f} MB per frame (vertex {:.2f}, index {:.2f}, texture {:.2f}, resolve {:.2f}, "
      "memexport {:.2f}, other {:.2f}), {:.2f} of it again in the same frame; {:.2f} MB per frame "
      "requested again after clear_memory_page_state's reset only",
      mb(uploaded), mb(kinds[size_t(RequestKind::kVertex)]), mb(kinds[size_t(RequestKind::kIndex)]),
      mb(kinds[size_t(RequestKind::kTexture)]), mb(kinds[size_t(RequestKind::kResolve)]),
      mb(kinds[size_t(RequestKind::kMemExport)]), mb(kinds[size_t(RequestKind::kOther)]),
      mb(stats.uploaded_again_in_frame), mb(stats.requested_after_reset_only));
  if (upload_hashing_enabled_) {
    line += fmt::format(
        "; hashed pages: {:.2f} MB per frame changed after the reset only (stale with "
        "clear_memory_page_state off), {:.2f} MB unchanged ({})",
        mb(stats.changed_after_reset_only), mb(stats.unchanged),
        upload_skip_unchanged_ ? "not uploaded" : "uploaded anyway");
  }
  return line;
}

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();

  // Invalidate the whole CPU->GPU copy, so whatever the frame uses is uploaded
  // again and lands in the trace, and collect the ranges the GPU wrote, which
  // are only on the host GPU and must be downloaded into the trace.
  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  // Hot pages unchanged during the frame wouldn't be uploaded, so not recorded.
  DemoteHotPages(0, (kBufferSize >> page_size_log2_) - 1);
  const auto add_gpu_written_range = [this](uint32_t page_first, uint32_t page_end) {
    uint32_t length = page_end - page_first;
    // The page may be marked as GPU-written without a write to the shared
    // memory buffer, by a resolve with resolution scaling for instance.
    if (EnsureHostGpuMemoryAllocated(page_first << page_size_log2_, length << page_size_log2_)) {
      trace_download_ranges_.emplace_back(page_first << page_size_log2_, length << page_size_log2_);
      trace_download_page_count_ += length;
    }
  };
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t previously_valid_block = system_page_flags_valid_[i];
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    system_page_flags_valid_[i] = gpu_written_block;
    page_invalidated_[i] |= ~gpu_written_block;

    // Fire the watches of the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t block_page;
      if (!rex::bit_scan_forward(fire_watches_range_start == UINT32_MAX ? fire_watches_block
                                                                        : fire_watches_break_block,
                                 &block_page)) {
        break;
      }
      uint32_t page = (i << 6) + block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = page;
      } else {
        FireWatches(fire_watches_range_start, page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t block_mask = ~((uint64_t(1) << block_page) - 1);
      fire_watches_block &= block_mask;
      fire_watches_break_block &= block_mask;
    }

    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t block_page;
      if (!rex::bit_scan_forward(
              gpu_written_range_start == UINT32_MAX ? gpu_written_block : gpu_written_break_block,
              &block_page)) {
        break;
      }
      uint32_t page = (i << 6) + block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = page;
      } else {
        add_gpu_written_range(gpu_written_range_start, page);
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t block_mask = ~((uint64_t(1) << block_page) - 1);
      gpu_written_block &= block_mask;
      gpu_written_break_block &= block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    add_gpu_written_range(gpu_written_range_start, page_count);
  }
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length, RequestKind kind) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1, kind);
}

bool SharedMemory::IsRangeValid(uint32_t start, uint32_t length) const {
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }

  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  const uint32_t block_first = page_first >> 6;
  const uint32_t block_last = page_last >> 6;

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t valid_mask = UINT64_MAX;
    if (i == block_first) {
      valid_mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      valid_mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_[i] & valid_mask) != valid_mask) {
      return false;
    }
  }
  return true;
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
    page_invalidated_[i] |= invalidate_bits;
    if (hot_pages_enabled_) {
      page_cpu_written_[i] |= invalidate_bits;
    }
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
