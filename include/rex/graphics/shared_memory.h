#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/thread/mutex.h>

namespace rex::graphics {

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  void InvalidateAllPages();
  // Call on the GPU thread when a frame ends (hot pages, see EnableHotPages, and
  // the upload statistics).
  void AdvanceHotPageFrame();
  // Call on the GPU thread where guest memory that later commands read may
  // have changed without an access violation (new commands from the guest, a
  // wait for the guest, a packet writing memory): a hot page is compared at
  // most once between two such points.
  void AdvanceHotPageSyncPoint() { ++hot_page_sync_point_; }

  typedef void (*GlobalWatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                      void* context, uint32_t address_first, uint32_t address_last,
                                      bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // What a request reads the pages for, for the upload statistics.
  enum class RequestKind : uint8_t {
    kVertex,
    kIndex,
    kTexture,
    // Destination of a resolve, loaded before the GPU writes part of it.
    kResolve,
    kMemExport,
    kOther,
    kCount,
  };

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable.
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count,
                     RequestKind kind = RequestKind::kOther);
  bool RequestRange(uint32_t start, uint32_t length, RequestKind kind = RequestKind::kOther);

  // Uploads since the last TakeUploadStats, in pages, gathered while enabled
  // (by the command processor while frame_stats_interval is above 0). GPU
  // thread only.
  void SetUploadStatsEnabled(bool enabled) { upload_stats_enabled_ = enabled; }
  struct UploadStats {
    uint64_t uploaded_by_kind[size_t(RequestKind::kCount)] = {};
    // Uploaded again in the frame of their previous upload.
    uint64_t uploaded_again_in_frame = 0;
    // Requested while not valid, with clear_memory_page_state's frame-end reset
    // the only thing that invalidated them since they were last made valid.
    uint64_t requested_after_reset_only = 0;
    // With upload hashing (shared_memory_hash_uploads): requested pages whose
    // bytes equal their last upload (not uploaded with
    // shared_memory_skip_unchanged_uploads), and the pages requested after the
    // reset only whose bytes had changed: stale with clear_memory_page_state
    // off.
    uint64_t unchanged = 0;
    uint64_t changed_after_reset_only = 0;
  };
  UploadStats TakeUploadStats();
  // One log line, per frame over the given number of frames.
  std::string FormatUploadStats(const UploadStats& stats, size_t frames) const;
  // Returns whether every page in the range is currently valid in the host GPU
  // memory copy. Hold the global critical region if relying on this for state
  // transitions such as watch installation.
  bool IsRangeValid(uint32_t start, uint32_t length) const;

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  void RangeWrittenByGpu(uint32_t start, uint32_t length);

  // For looking at guest data on the CPU (texture replacement computes keys of
  // textures from it): the guest's own physical memory (as the CPU wrote it,
  // big-endian), not the GPU's copy.
  const uint8_t* TranslatePhysical(uint32_t address) const;
  // Whether any page of the range holds data written by the GPU (such as a
  // resolve) that the CPU hasn't overwritten since: the guest memory may not
  // have that data.
  bool IsRangeWrittenByGpu(uint32_t start, uint32_t length);

 protected:
  SharedMemory(memory::Memory& memory);
  // Call in implementation-specific initialization.
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  memory::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Hot pages (shared_memory_hot_pages): pages the CPU rewrites every frame,
  // such as dynamic vertex data, are not write-protected. Whenever they are
  // requested they are compared with a shadow copy of their last upload and
  // uploaded only if changed, which saves a protection change per upload and an
  // access violation per CPU write. Call in Initialize if UploadRanges copies
  // the data with CopyPagesForUpload.
  void EnableHotPages();
  // Upload hashing (shared_memory_hash_uploads,
  // shared_memory_skip_unchanged_uploads): the hash of the bytes last uploaded
  // to each page of the buffer, kept until the GPU writes the page, so a page
  // requested again is compared instead of trusting what invalidated it. Call
  // in Initialize if UploadRanges copies the data with CopyPagesForUpload.
  void EnableUploadHashing();
  // Copies pages to upload to dst, hot pages from their shadow copies, so the
  // buffer holds what the next comparison compares with, and records the
  // hashes of the copied pages. Call after MakeRangeValid for the pages.
  void CopyPagesForUpload(uint8_t* dst, uint32_t page_first, uint32_t page_count);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;

 public:
  // For starting a frame trace: invalidates everything not written by the GPU,
  // so the frame uploads again what it uses, and fills
  // trace_download_ranges() with the GPU-written byte ranges to download.
  void PrepareForTraceDownload();
  void ReleaseTraceDownloadRanges();
  const std::vector<std::pair<uint32_t, uint32_t>>& trace_download_ranges() const {
    return trace_download_ranges_;
  }
  uint32_t trace_download_page_count() const { return trace_download_page_count_; }

 private:
  memory::Memory& memory_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;

  // Ranges that need to be uploaded, generated by GetRangesToUpload (a
  // persistently allocated vector).
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // Upload hashing (see EnableUploadHashing). Only used on the GPU thread.
  bool upload_hashing_enabled_ = false;
  bool upload_skip_unchanged_ = false;
  std::vector<uint64_t> page_upload_hashes_;
  // Pages whose entry in page_upload_hashes_ is the hash of what the buffer
  // holds.
  std::vector<uint64_t> page_upload_hash_valid_;
  bool IsPageUploadHashValid(uint32_t page) const {
    return upload_hashing_enabled_ && ((page_upload_hash_valid_[page >> 6] >> (page & 63)) & 1);
  }
  uint64_t HashPage(uint32_t page) const;
  // shared_memory_hash_threads: calls work(begin, end) over the pages, on the
  // workers too when there are enough pages.
  class HashWorkers;
  std::unique_ptr<HashWorkers> hash_workers_;
  void HashPages(uint32_t page_first, uint32_t page_count,
                 const std::function<void(uint32_t, uint32_t)>& work);
  void InvalidateUploadHashes(uint32_t page_first, uint32_t page_last);
  // Counts upload_ranges_ for the statistics and compares the pages that have
  // a hash. With skipping, makes the unchanged ones valid without a copy and
  // drops them from upload_ranges_; returns whether pages left in
  // upload_ranges_ were made valid before their copy.
  bool FilterUnchangedUploads(bool gather_stats);
  std::vector<std::pair<uint32_t, uint32_t>> unchanged_filter_ranges_;
  std::vector<uint8_t> unchanged_filter_flags_;

  bool upload_stats_enabled_ = false;
  UploadStats upload_stats_;
  // The frame (hot_page_frame_) each page was last uploaded in, for the
  // statistics; allocated when first needed.
  std::vector<uint32_t> page_stats_upload_frame_;
  void CountUploads(RequestKind kind);

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  rex::thread::global_critical_region global_critical_region_;

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  // Hot pages (see EnableHotPages). A page becomes hot after being uploaded
  // after a CPU write in kHotPageUploadStreak frames in a row (uploads after
  // clear_memory_page_state's per-frame reset don't count, they would make
  // every page used in a frame hot), and stops being hot when unchanged or
  // not requested for kHotPageIdleFrames, when watched, or when written by the
  // GPU. Hot pages are never valid. The state only changes on the GPU thread,
  // in the global critical region; the GPU thread may read it outside of it.
  static constexpr uint8_t kHotPageUploadStreak = 3;
  static constexpr uint32_t kHotPageIdleFrames = 120;
  // Frames between checks for hot pages that are no longer requested.
  static constexpr uint32_t kHotPageSweepFrames = 64;
  // Memory for the shadow copies.
  static constexpr uint32_t kMaxHotPageBytes = 64 * 1024 * 1024;
  bool hot_pages_enabled_ = false;
  uint32_t hot_page_frame_ = 1;
  std::vector<uint64_t> hot_pages_;
  uint32_t hot_page_count_ = 0;
  // Only used on the GPU thread, so changed outside the global critical region.
  uint32_t hot_page_sync_point_ = 1;
  // For hot pages, the last frame they were requested in; for other pages, the
  // last frame they were uploaded in after a CPU write.
  std::vector<uint32_t> page_last_upload_frame_;
  std::vector<uint32_t> page_last_change_frame_;
  std::vector<uint8_t> page_upload_streak_;
  // For hot pages, the sync point they were last compared at.
  std::vector<uint32_t> page_last_compare_sync_point_;
  // Pages invalidated by a CPU write since their last upload.
  std::vector<uint64_t> page_cpu_written_;
  std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> hot_page_shadows_;
  std::vector<std::pair<uint32_t, uint32_t>> hot_page_upload_ranges_;
  bool IsPageHot(uint32_t page) const {
    return hot_pages_enabled_ && ((hot_pages_[page >> 6] >> (page & 63)) & 1);
  }
  // The functions below must be called in the global critical region.
  bool IsPageWatched(uint32_t page) const;
  // Makes the pages in the range normal again and protects them.
  void DemoteHotPages(uint32_t page_first, uint32_t page_last);
  // Compares requested hot pages with their shadow copies, dropping unchanged
  // ones from upload_ranges_, and makes pages hot.
  void FilterHotPageUploads(const std::unique_lock<std::recursive_mutex>& global_lock);

  // Pages whose contents in the buffer are in sync with guest memory.
  std::vector<uint64_t> system_page_flags_valid_;
  // Subset of valid pages containing data written by the GPU.
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  // Pages invalidated by something other than clear_memory_page_state's
  // frame-end reset since they were last made valid (upload statistics).
  std::vector<uint64_t> page_invalidated_;
  // GPU-written byte ranges for a frame trace: <start, length>.
  std::vector<std::pair<uint32_t, uint32_t>> trace_download_ranges_;
  uint32_t trace_download_page_count_ = 0;
  uint32_t num_system_page_flags_ = 0;

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
        // The range's pages, so a write tests each node of a bucket without
        // loading its range (one cache line per node instead of two).
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchNode* next_free;
    };
  };
  // The bucket size is gpu_memory_watch_bucket_log2 (log2 of bytes; 22, 4 MB,
  // was the fixed size), the array is sized for the smallest.
  static constexpr uint32_t kWatchBucketSizeLog2Min = 18;
  static constexpr uint32_t kWatchBucketSizeLog2Max = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2Min);
  uint32_t watch_bucket_size_log2_ = kWatchBucketSizeLog2Max;
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::graphics
