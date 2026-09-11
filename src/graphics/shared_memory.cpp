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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <execinfo.h>
#endif

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/diagnostic_gate.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics {

// Counters for the REX_DIAG_VALID_SWAP_RACE log-only diagnostic. Namespace
// scope so both sides of the race can report each other's activity.
namespace {
std::atomic<uint64_t> diag_invalidation_calls{0};
std::atomic<uint64_t> diag_swap_calls{0};
bool DiagSwapRaceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("REX_DIAG_VALID_SWAP_RACE");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

// REX_WATCH_SHMEM=<hexbase>:<hexlen>: log every CPU invalidation and every
// CPU to GPU re-upload of shared memory intersecting the range. Log only.
struct WatchShmemRange {
  bool enabled = false;
  uint32_t base = 0;
  uint32_t length = 0;
};
const WatchShmemRange& GetWatchShmemRange() {
  static const WatchShmemRange range = [] {
    WatchShmemRange r;
    const char* spec = std::getenv("REX_WATCH_SHMEM");
    if (!spec) {
      return r;
    }
    char* sep = nullptr;
    r.base = uint32_t(strtoull(spec, &sep, 16));
    if (!sep || *sep != ':') {
      return r;
    }
    r.length = uint32_t(strtoull(sep + 1, nullptr, 16));
    r.enabled = r.length != 0;
    return r;
  }();
  return range;
}
}  // namespace

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  valid_buffer_a_.assign(num_system_page_flags_, 0);
  valid_buffer_b_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  system_page_flags_gpu_written_outside_buffer_.assign(num_system_page_flags_, 0);
  active_valid_flags_.store(valid_buffer_a_.data(), std::memory_order_relaxed);
  staging_valid_flags_.store(valid_buffer_b_.data(), std::memory_order_relaxed);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);

  if (diag::VerifyTextures()) {
    upload_page_hashes_.assign(kBufferSize >> page_size_log2_, 0);
    upload_page_hash_known_.assign(num_system_page_flags_, 0);
    verify_reported_pages_.assign(num_system_page_flags_, 0);
    verify_checked_pages_.assign(num_system_page_flags_, 0);
    REXGPU_INFO(
        "REX_VERIFY_TEXTURES: checking requested shared memory pages and bound textures "
        "against guest memory at every draw");
  }
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  ReleaseTraceDownloadRanges();

  if (!upload_page_hashes_.empty()) {
    REXGPU_INFO("REX_VERIFY_TEXTURES: shared memory pages checked={} stale={}",
                verify_pages_checked_, verify_pages_stale_);
    upload_page_hashes_.clear();
    upload_page_hashes_.shrink_to_fit();
    upload_page_hash_known_.clear();
    verify_reported_pages_.clear();
    verify_checked_pages_.clear();
  }

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

  active_valid_flags_.store(nullptr, std::memory_order_relaxed);
  staging_valid_flags_.store(nullptr, std::memory_order_relaxed);
  valid_buffer_a_.clear();
  valid_buffer_a_.shrink_to_fit();
  valid_buffer_b_.clear();
  valid_buffer_b_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  system_page_flags_gpu_written_outside_buffer_.clear();
  system_page_flags_gpu_written_outside_buffer_.shrink_to_fit();
  num_system_page_flags_ = 0;
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  uint64_t* active = active_valid_flags_.load(std::memory_order_relaxed);
  uint64_t* staging = staging_valid_flags_.load(std::memory_order_relaxed);
  if (active && num_system_page_flags_) {
    std::memset(active, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (staging && num_system_page_flags_) {
    std::memset(staging, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (!system_page_flags_valid_and_gpu_written_.empty()) {
    std::memset(system_page_flags_valid_and_gpu_written_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
  }
  if (!system_page_flags_gpu_written_outside_buffer_.empty()) {
    std::memset(system_page_flags_gpu_written_outside_buffer_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
  }

  // Force a refresh on the next frame-end sync.
  dirty_blocks_.store(UINT32_MAX, std::memory_order_relaxed);
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  // REX_DIAG_VALID_SWAP_RACE=1: count calls and periodically report both
  // sides, counting before the early-out so "called but never dirty" shows.
  if (DiagSwapRaceEnabled()) {
    uint64_t swap_n = diag_swap_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (swap_n % 600 == 1) {
      REXGPU_WARN(
          "[diag-swap-race] alive: swap call #{} (dirty={}), {} CPU invalidation callback(s) so "
          "far",
          swap_n, gpu_written_data_dirty_.load(std::memory_order_relaxed),
          diag_invalidation_calls.load(std::memory_order_relaxed));
    }
  }
  if (!gpu_written_data_dirty_.load(std::memory_order_relaxed)) {
    return;
  }

  uint64_t* staging = staging_valid_flags_.load(std::memory_order_acquire);
  if (!staging || !num_system_page_flags_) {
    gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
    dirty_blocks_.store(0, std::memory_order_relaxed);
    return;
  }

  uint32_t dirty_mask = dirty_blocks_.exchange(0, std::memory_order_relaxed);
  uint32_t dirty_count = rex::bit_count(dirty_mask);
  if (dirty_count == 0 || dirty_count > 16) {
    std::memcpy(staging, system_page_flags_valid_and_gpu_written_.data(),
                num_system_page_flags_ * sizeof(uint64_t));
  } else {
    while (dirty_mask) {
      uint32_t block_index;
      rex::bit_scan_forward(dirty_mask, &block_index);
      dirty_mask &= ~(uint32_t(1) << block_index);
      uint32_t entry_offset = block_index * 64;
      if (entry_offset >= num_system_page_flags_) {
        continue;
      }
      uint32_t entry_count = std::min(uint32_t(64), num_system_page_flags_ - entry_offset);
      std::memcpy(staging + entry_offset,
                  system_page_flags_valid_and_gpu_written_.data() + entry_offset,
                  entry_count * sizeof(uint64_t));
    }
  }

  // REX_DIAG_SWAP_STALL_US=<n>: widen the copy-to-swap window so the race
  // detector is exercised. Instrument check only; never set it otherwise.
  static const uint32_t diag_swap_stall_us = [] {
    const char* value = std::getenv("REX_DIAG_SWAP_STALL_US");
    return value ? uint32_t(std::strtoul(value, nullptr, 10)) : uint32_t(0);
  }();
  if (diag_swap_stall_us) {
    std::this_thread::sleep_for(std::chrono::microseconds(diag_swap_stall_us));
  }

  uint64_t* old_active = active_valid_flags_.exchange(staging, std::memory_order_acq_rel);
  staging_valid_flags_.store(old_active, std::memory_order_release);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
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
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

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
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

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
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
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

  if (diag::RangeCoversLoggedTexture(start, length)) {
    REXGPU_INFO("TEXWATCHED gpu-write {:08X}+{:X}", start, length);
  }
  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

void SharedMemory::RangeWrittenByGpuOutsideBuffer(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Same modification callbacks as RangeWrittenByGpu, so textures reading the
  // range are refreshed and scaled-resolve tracking is preserved.
  FireWatches(page_first, page_last, true);

  // The data went to a separate buffer, so this buffer's copy is now behind.
  // Mark it outdated, and record which pages hold newer data elsewhere.
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;
  uint32_t dirty_blocks_mask = 0;
  {
    auto global_lock = global_critical_region_.Acquire();
    uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint64_t invalidate_bits = UINT64_MAX;
      if (i == block_first) {
        invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (i == block_last && (page_last & 63) != 63) {
        invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      if (valid_flags) {
        valid_flags[i] &= ~invalidate_bits;
      }
      system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
      system_page_flags_gpu_written_outside_buffer_[i] |= invalidate_bits;
      dirty_blocks_mask |= uint32_t(1) << (i >> 6);
    }
    gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
    dirty_blocks_.fetch_or(dirty_blocks_mask, std::memory_order_relaxed);
  }

  // Re-arm write protection as a GPU write to this buffer would: CPU write
  // detection is one-shot, and the separate buffer's consumers rely on it.
  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        page_first << page_size_log2_, (page_last - page_first + 1) << page_size_log2_, true,
        false);
  }
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

uint32_t SharedMemory::CountOutdatedPages(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return 0;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();
  const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  if (!valid_flags) {
    return 0;
  }
  uint32_t count = 0;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t range_bits = UINT64_MAX;
    if (i == block_first) {
      range_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      range_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    count += uint32_t(rex::bit_count(~valid_flags[i] & range_bits));
  }
  return count;
}

void SharedMemory::RecordUploadedPages(uint32_t start, const uint8_t* source, uint32_t length) {
  if (upload_page_hashes_.empty() || !length || start >= kBufferSize) {
    return;
  }
  uint32_t page_size = uint32_t(1) << page_size_log2_;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + std::min(length, kBufferSize - start) - 1) >> page_size_log2_;
  bool page_aligned = ((start | length) & (page_size - 1)) == 0;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint64_t bit = uint64_t(1) << (page & 63);
    if (!page_aligned) {
      // Only whole pages can be compared later; forget any earlier hash.
      upload_page_hash_known_[page >> 6] &= ~bit;
      continue;
    }
    upload_page_hashes_[page] =
        XXH3_64bits(source + ((page << page_size_log2_) - start), page_size);
    upload_page_hash_known_[page >> 6] |= bit;
    verify_reported_pages_[page >> 6] &= ~bit;
  }
}

uint64_t SharedMemory::HashGuestRangeCpuPages(uint32_t start, uint32_t length,
                                              uint32_t* gpu_written_pages_out) const {
  uint64_t hash = 0;
  uint32_t gpu_written_pages = 0;
  if (length && start < kBufferSize) {
    length = std::min(length, kBufferSize - start);
    uint32_t end = start + length;
    uint32_t page_first = start >> page_size_log2_;
    uint32_t page_last = (end - 1) >> page_size_log2_;
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t bit = uint64_t(1) << (page & 63);
      if ((system_page_flags_valid_and_gpu_written_[page >> 6] |
           system_page_flags_gpu_written_outside_buffer_[page >> 6]) &
          bit) {
        ++gpu_written_pages;
        continue;
      }
      uint32_t page_start = std::max(page << page_size_log2_, start);
      uint32_t page_end = std::min((page + 1) << page_size_log2_, end);
      hash = XXH3_64bits_withSeed(memory_.TranslatePhysical(page_start), page_end - page_start,
                                  hash);
    }
  }
  if (gpu_written_pages_out) {
    *gpu_written_pages_out = gpu_written_pages;
  }
  return hash;
}

uint32_t SharedMemory::VerifyRangeAgainstUpload(uint32_t start, uint32_t length) {
  if (upload_page_hashes_.empty() || !length || start >= kBufferSize) {
    return 0;
  }
  length = std::min(length, kBufferSize - start);
  uint64_t submission = diag::CurrentSubmission();
  if (submission != verify_checked_submission_) {
    verify_checked_submission_ = submission;
    std::fill(verify_checked_pages_.begin(), verify_checked_pages_.end(), uint64_t(0));
    // A title that hard-exits never runs the shutdown summary, and silence is
    // only evidence if the checks are known to have run.
    if (submission && !(submission % 600)) {
      REXGPU_INFO("REX_VERIFY_TEXTURES alive: shared memory pages checked={} stale={} (submission {})",
                  verify_pages_checked_, verify_pages_stale_, submission);
    }
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t stale_pages = 0;

  auto global_lock = global_critical_region_.Acquire();
  const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  if (!valid_flags) {
    return 0;
  }
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint32_t block = page >> 6;
    uint64_t bit = uint64_t(1) << (page & 63);
    if (verify_checked_pages_[block] & bit) {
      continue;
    }
    verify_checked_pages_[block] |= bit;
    // An invalid page is uploaded before the GPU reads it, a GPU-written page
    // is meant to differ, and a never-uploaded page has nothing to compare.
    if (!(valid_flags[block] & bit) ||
        ((system_page_flags_valid_and_gpu_written_[block] |
          system_page_flags_gpu_written_outside_buffer_[block]) &
         bit) ||
        !(upload_page_hash_known_[block] & bit)) {
      continue;
    }
    ++verify_pages_checked_;
    uint64_t guest_hash = XXH3_64bits(memory_.TranslatePhysical(page << page_size_log2_),
                                      size_t(1) << page_size_log2_);
    if (guest_hash == upload_page_hashes_[page]) {
      continue;
    }
    ++stale_pages;
    ++verify_pages_stale_;
    if (verify_reported_pages_[block] & bit) {
      continue;
    }
    verify_reported_pages_[block] |= bit;
    constexpr uint32_t kMaxLines = 512;
    if (verify_lines_logged_ < kMaxLines) {
      REXGPU_WARN(
          "SMSTALE sub={} draw={} page={:08X} (requested {:08X}+{:X}): the valid GPU copy holds "
          "bytes hashing {:016X}, guest memory now hashes {:016X}",
          submission, diag::CurrentDrawIndex(), page << page_size_log2_, start, length,
          upload_page_hashes_[page], guest_hash);
    } else if (verify_lines_logged_ == kMaxLines) {
      REXGPU_WARN("SMSTALE: further lines suppressed; see the shutdown summary for counts");
    }
    ++verify_lines_logged_;
  }
  return stale_pages;
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

  {
    auto global_lock = global_critical_region_.Acquire();
    uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      if (valid_flags) {
        valid_flags[i] |= valid_bits;
      }
      uint64_t old_gpu_written = system_page_flags_valid_and_gpu_written_[i];
      uint64_t new_gpu_written =
          written_by_gpu ? (old_gpu_written | valid_bits) : (old_gpu_written & ~valid_bits);
      if (new_gpu_written != old_gpu_written) {
        system_page_flags_valid_and_gpu_written_[i] = new_gpu_written;
        gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
        dirty_blocks_.fetch_or(uint32_t(1) << (i >> 6), std::memory_order_relaxed);
      }
      if (written_by_gpu) {
        // This buffer holds the newest GPU data for the range now. An upload
        // from main memory does not supersede data in a separate buffer.
        system_page_flags_gpu_written_outside_buffer_[i] &= ~valid_bits;
      }
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
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

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
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

  auto verify_requested = [this, &merged_ranges]() {
    if (!diag::VerifyTextures()) {
      return;
    }
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      VerifyRangeAgainstUpload(range.first, range.second);
    }
  };

  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_acquire);
  if (valid_flags) {
    bool all_valid = true;
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      if (!range.second) {
        continue;
      }
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags[i];
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }
        if (block_valid != UINT64_MAX) {
          all_valid = false;
          break;
        }
      }
      if (!all_valid) {
        break;
      }
    }
    if (all_valid) {
      COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
      COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                        uint32_t(merged_ranges.size()));
      COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count", 0);
      verify_requested();
      return true;
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
    valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags ? valid_flags[i] : 0;
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
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    verify_requested();
    return true;
  }

  // REX_WATCH_SHMEM: report CPU to GPU uploads that overwrite the watched
  // range, which is where GPU-resolved content in the buffer is destroyed.
  {
    const WatchShmemRange& watch = GetWatchShmemRange();
    if (watch.enabled) {
      for (const std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
        uint32_t up_start = upload_range.first << page_size_log2_;
        uint32_t up_end = (upload_range.first + upload_range.second) << page_size_log2_;
        if (up_start < watch.base + watch.length && up_end > watch.base) {
          static std::atomic<int> hits{0};
          int hit = hits.fetch_add(1);
          if (hit < 200) {
            REXGPU_WARN(
                "[watch-shmem] CPU->GPU upload #{} of [0x{:08X}, 0x{:08X}) intersects watched "
                "[0x{:08X}, +0x{:X}) - buffer content there is replaced with CPU RAM bytes",
                hit, up_start, up_end, watch.base, watch.length);
          }
        }
      }
    }
  }

  if (!UploadRanges(upload_ranges_)) {
    return false;
  }
  verify_requested();
  return true;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
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
  if (diag::RangeCoversLoggedTexture(physical_address_start, length)) {
    REXGPU_INFO("TEXWATCHED cpu-invalidate {:08X}+{:X} exact={}", physical_address_start, length,
                exact_range);
  }

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
    // Pages with GPU-written data in a separate buffer bound the widening the
    // same way GPU-written pages here do; they cannot be recovered from RAM.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first] |
                                   system_page_flags_gpu_written_outside_buffer_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last] |
                                 system_page_flags_gpu_written_outside_buffer_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  uint32_t dirty_blocks_mask = 0;
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (valid_flags) {
      valid_flags[i] &= ~invalidate_bits;
    }
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
    system_page_flags_gpu_written_outside_buffer_[i] &= ~invalidate_bits;
    dirty_blocks_mask |= uint32_t(1) << (i >> 6);
  }
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
  dirty_blocks_.fetch_or(dirty_blocks_mask, std::memory_order_relaxed);

  // DIAGNOSTIC (REX_WATCH_SHMEM): report CPU invalidations of the watched
  // range, with a host backtrace naming the faulting guest code path.
  {
    const WatchShmemRange& watch = GetWatchShmemRange();
    if (watch.enabled) {
      uint32_t inv_start = page_first << page_size_log2_;
      uint32_t inv_end = ((page_last + 1) << page_size_log2_);
      if (inv_start < watch.base + watch.length && inv_end > watch.base) {
        static std::atomic<int> hits{0};
        int hit = hits.fetch_add(1);
        if (hit < 200) {
          REXGPU_WARN(
              "[watch-shmem] CPU invalidation #{} of [0x{:08X}, 0x{:08X}) (requested [0x{:08X}, "
              "+0x{:X}), exact={}) intersects watched [0x{:08X}, +0x{:X})",
              hit, inv_start, inv_end, physical_address_start, length, exact_range, watch.base,
              watch.length);
#if defined(__linux__)
          if (hit < 16) {
            void* frames[16];
            int frame_count = backtrace(frames, 16);
            char** symbols = backtrace_symbols(frames, frame_count);
            if (symbols) {
              for (int i = 0; i < frame_count; ++i) {
                REXGPU_WARN("[watch-shmem]   frame {}: {}", i, symbols[i]);
              }
              free(symbols);
            }
          }
#endif
        }
      }
    }
  }

  FireWatches(page_first, page_last, false);

  // REX_DIAG_VALID_SWAP_RACE=1: detect the frame-close valid-flag buffer swap
  // racing this invalidation, as it runs outside the critical region.
  if (DiagSwapRaceEnabled()) {
    diag_invalidation_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t* active_now = active_valid_flags_.load(std::memory_order_acquire);
    if (active_now && active_now != valid_flags) {
      uint32_t lost_pages = 0;
      uint32_t first_lost_page = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t check_bits = UINT64_MAX;
        if (i == block_first) {
          check_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
        }
        if (i == block_last && (page_last & 63) != 63) {
          check_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
        }
        uint64_t lost = active_now[i] & check_bits;
        if (lost) {
          lost_pages += uint32_t(rex::bit_count(lost));
          if (first_lost_page == UINT32_MAX) {
            uint32_t lost_bit;
            rex::bit_scan_forward(lost, &lost_bit);
            first_lost_page = (i << 6) + lost_bit;
          }
        }
      }
      if (lost_pages) {
        REXGPU_WARN(
            "[diag-swap-race] CPU invalidation [0x{:08X}, +0x{:X}) (pages [0x{:08X}, 0x{:08X}]) "
            "raced the frame-close valid-flags swap: {} page clear(s) LOST on the newly published "
            "buffer, first at 0x{:08X} - reads of those pages will reuse stale GPU-buffer bytes "
            "until they are invalidated again",
            physical_address_start, length, page_first << page_size_log2_,
            ((page_last + 1) << page_size_log2_) - 1, lost_pages,
            first_lost_page << page_size_log2_);
      } else {
        REXGPU_WARN(
            "[diag-swap-race] CPU invalidation [0x{:08X}, +0x{:X}) raced the frame-close "
            "valid-flags swap; no clears lost (the swap copied the master flags after they were "
            "cleared)",
            physical_address_start, length);
      }
    }
  }

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();
  assert_true(trace_download_ranges_.empty());
  assert_zero(trace_download_page_count_);

  // Invalidate the entire memory CPU->GPU memory copy so all the history
  // doesn't have to be written into every frame trace, and collect the list of
  // ranges with data modified on the GPU.

  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t previously_valid_block = valid_flags ? valid_flags[i] : 0;
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    if (valid_flags) {
      valid_flags[i] = gpu_written_block;
    }

    // Fire watches on the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t fire_watches_block_page;
      if (!rex::bit_scan_forward(fire_watches_range_start == UINT32_MAX ? fire_watches_block
                                                                        : fire_watches_break_block,
                                 &fire_watches_block_page)) {
        break;
      }
      uint32_t fire_watches_page = (i << 6) + fire_watches_block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = fire_watches_page;
      } else {
        FireWatches(fire_watches_range_start, fire_watches_page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t fire_watches_block_mask = ~((uint64_t(1) << fire_watches_block_page) - 1);
      fire_watches_block &= fire_watches_block_mask;
      fire_watches_break_block &= fire_watches_block_mask;
    }

    // Add to the GPU-written ranges.
    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t gpu_written_block_page;
      if (!rex::bit_scan_forward(
              gpu_written_range_start == UINT32_MAX ? gpu_written_block : gpu_written_break_block,
              &gpu_written_block_page)) {
        break;
      }
      uint32_t gpu_written_page = (i << 6) + gpu_written_block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = gpu_written_page;
      } else {
        uint32_t gpu_written_range_length = gpu_written_page - gpu_written_range_start;
        // Call EnsureHostGpuMemoryAllocated in case the page was marked as
        // GPU-written not as a result to an actual write to the shared memory
        // buffer, but, for instance, by resolving with resolution scaling (to a
        // separate buffer).
        if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                         gpu_written_range_length << page_size_log2_)) {
          trace_download_ranges_.push_back(
              std::make_pair(gpu_written_range_start << page_size_log2_,
                             gpu_written_range_length << page_size_log2_));
          trace_download_page_count_ += gpu_written_range_length;
        }
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t gpu_written_block_mask = ~((uint64_t(1) << gpu_written_block_page) - 1);
      gpu_written_block &= gpu_written_block_mask;
      gpu_written_break_block &= gpu_written_block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    uint32_t gpu_written_range_length = page_count - gpu_written_range_start;
    if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                     gpu_written_range_length << page_size_log2_)) {
      trace_download_ranges_.push_back(std::make_pair(gpu_written_range_start << page_size_log2_,
                                                      gpu_written_range_length << page_size_log2_));
      trace_download_page_count_ += gpu_written_range_length;
    }
  }
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
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
