/*
 * state_store.h - Transparent state storage with tiered cache and prefetch
 *
 * Design:
 * - L1 Hot Cache (raw): Fast access, limited by hot_cache_size
 * - L2 Warm Cache (compressed): Higher capacity, warm_cache_size
 * - Disk: Persistent storage
 * - Prefetch: Background loading of upcoming states from queue
 */

#ifndef __STATE_STORE_H
#define __STATE_STORE_H

#include "types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Platform-specific includes for memory trimming
#ifdef __linux__
#include <malloc.h>
#endif

// Forward declare ZSTD types
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;

// hash_type is now defined in types.h as uint64_t

/* ==============================================================================
 * Cache Entry (unified for L1/L2)
 * ==============================================================================
 */

struct StateEntry
{
  hash_type hash{0};

  std::vector<uint8_t> raw_data;
  std::shared_ptr<std::vector<uint8_t>> raw_data_ptr;

  void init_shared_ptr()
  {
    raw_data_ptr = std::shared_ptr<std::vector<uint8_t>>(
        std::shared_ptr<std::vector<uint8_t>>(), &raw_data);
  }

  std::vector<uint8_t> compressed_data;

  enum class State
  {
    RAW,        // Has uncompressed data (in L1)
    COMPRESSED, // Has compressed data only (in L2)
    PERSISTING, // Being written to disk
    PERSISTED,  // On disk only
    LOADING     // Being loaded from disk
  };
  std::atomic<State> state{State::RAW};

  // For LRU tracking
  std::atomic<uint64_t> last_access{0};

  // For async notification
  std::condition_variable cv;
  std::mutex mutex;

  StateEntry() = default;
  explicit StateEntry(hash_type h) : hash(h), state(State::RAW), last_access(0)
  {
  }
};

using StateEntryPtr = std::shared_ptr<StateEntry>;

/* ==============================================================================
 * Prefetch Task
 * ==============================================================================
 */

struct PrefetchTask
{
  hash_type hash;
  std::chrono::steady_clock::time_point submit_time;
};

/* ==============================================================================
 * State Store
 * ==============================================================================
 */

class StateStore
{
  public:
  struct Config
  {
    // L1 Hot Cache: raw uncompressed data (fast access)
    size_t hot_cache_size = 512ULL * 1024 * 1024; // 512MB default

    // L2 Warm Cache: compressed data (higher capacity)
    size_t warm_cache_size = 2048ULL * 1024 * 1024; // 2GB default

    // Compression settings
    int compression_level = 1;
    int io_threads = 1;

    // Prefetch settings
    size_t prefetch_window = 100;       // Number of upcoming states to prefetch
    size_t prefetch_threads = 1;        // Prefetch worker threads
    size_t max_inflight_prefetch = 200; // Max concurrent prefetch tasks

    // Memory management
    bool enable_malloc_trim =
        false; // Call malloc_trim on L2 eviction (Linux only)
    size_t malloc_trim_threshold =
        64ULL * 1024 * 1024; // Trigger trim every 64MB evicted

    // Malloc tuning (Linux glibc only)
    int malloc_arena_max = 4; // Limit malloc arenas (default: 8*cores)
    size_t malloc_mmap_threshold =
        1024ULL * 1024 * 1024; // Only mmap for >1GB allocations

    // Storage path for packed storage
    std::string packed_storage_path = "memory";
  };

  StateStore();
  ~StateStore();

  void init(const Config &config);
  void init();

  // Save data: returns hash immediately, background write to disk
  hash_type save(const void *data, size_t len);

  // Load data: check L1 -> L2 -> disk
  ssize_t load(hash_type hash, std::vector<uint8_t> &out_data);

  // Load data returning shared_ptr for zero-copy access (returns nullptr if not
  // found)
  std::shared_ptr<const std::vector<uint8_t>> load_shared(hash_type hash);

  // Check existence
  bool exists(hash_type hash);

  // Wait for all pending IO operations to complete
  void wait_for_completion();

  // Queue management for transparent prefetch
  void disable_prefetch(void);
  void enable_prefetch(int prefetch_window);

  // Shutdown
  void shutdown();

  static StateStore &instance();

  // ==================================================================
  // Queue Management (with transparent prefetch)
  // ==================================================================

  // Queue operations - prefetch happens automatically on pop
  void queue_push_back(hash_type hash);
  void queue_pop_front();
  hash_type queue_try_pop_front();
  size_t queue_size() const;
  void queue_clear();

  // Get a hash at specific offset from front (for prefetch planning)
  hash_type queue_peek(size_t offset) const;

  // Check if hash is in L1/L2/prefetching
  bool in_l1(hash_type hash) const;
  bool in_l2(hash_type hash) const;
  bool in_preflight(hash_type hash) const;

  // Statistics
  struct Stats
  {
    uint64_t load_requests = 0;   // Total load() calls
    uint64_t l1_hits = 0;         // L1 cache hits
    uint64_t l2_hits = 0;         // L2 cache hits
    uint64_t disk_reads = 0;      // Disk reads
    uint64_t prefetch_issued = 0; // Prefetch tasks issued
    uint64_t prefetch_hits = 0;   // Prefetch hits (loaded before needed)

    // Save path statistics
    uint64_t save_calls = 0;      // Total save() calls
    uint64_t save_dedup_hot = 0;  // Deduplicated at hot cache
    uint64_t save_dedup_warm = 0; // Deduplicated at warm cache
    uint64_t save_dedup_disk = 0; // Deduplicated at disk
    uint64_t save_new = 0;        // New states (actually saved)

    // Eviction statistics
    uint64_t evict_calls = 0;   // Total evict_l1_async calls
    uint64_t evict_bytes = 0;   // Total bytes evicted
    uint64_t evict_time_us = 0; // Total time spent in eviction (microseconds)

    // Save path timing statistics
    uint64_t save_time_us = 0;  // Total time spent in save() for new states
    uint64_t save_new_slow = 0; // Count of saves taking >1ms

    double hit_rate() const
    {
      if (load_requests == 0)
        return 0.0;
      return 100.0 * (l1_hits + l2_hits) / load_requests;
    }

    double dedup_rate() const
    {
      if (save_calls == 0)
        return 0.0;
      return 100.0 * (save_dedup_hot + save_dedup_warm + save_dedup_disk) /
             save_calls;
    }
  };

  Stats get_stats() const;
  void reset_stats();
  void print_stats() const;

  // Cache usage info
  size_t get_l1_usage() const { return hot_memory_usage_.load(); }
  size_t get_l2_usage() const { return warm_memory_usage_.load(); }
  size_t get_l1_capacity() const { return config_.hot_cache_size; }
  size_t get_l2_capacity() const { return config_.warm_cache_size; }
  size_t get_l1_entry_count() const;
  size_t get_l2_entry_count() const;

  // Queue sizes for monitoring
  size_t get_io_queue_size() const;
  size_t get_prefetch_queue_size() const;
  size_t get_io_queue_capacity() const { return max_io_queue_size_; }

  private:
  Config config_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  // ==================================================================
  // Memory Caches (physically separated)
  // ==================================================================

  // L1 Hot Cache: hash -> entry with raw_data
  mutable std::recursive_mutex hot_mutex_;
  std::unordered_map<hash_type, StateEntryPtr> hot_cache_;
  std::list<hash_type> hot_lru_;
  std::unordered_map<hash_type, std::list<hash_type>::iterator> hot_lru_index_;
  std::atomic<size_t> hot_memory_usage_{0};

  // L2 Warm Cache: hash -> entry with compressed_data
  mutable std::recursive_mutex warm_mutex_;
  std::unordered_map<hash_type, StateEntryPtr> warm_cache_;
  std::list<hash_type> warm_lru_;
  std::unordered_map<hash_type, std::list<hash_type>::iterator> warm_lru_index_;
  std::atomic<size_t> warm_memory_usage_{0};

  // Unified access counter for both caches
  std::atomic<uint64_t> access_counter_{0};

  // Statistics
  mutable std::mutex stats_mutex_;
  Stats stats_;

  // ==================================================================
  // State Queue (managed by StateStore for transparent prefetch)
  // ==================================================================

  mutable std::mutex queue_mutex_;
  std::deque<hash_type> state_queue_;

  // ==================================================================
  // Prefetch System
  // ==================================================================

  // Inflight tracking (prevent duplicate prefetch)
  mutable std::mutex inflight_mutex_;
  std::unordered_set<hash_type> prefetch_inflight_;
  std::condition_variable inflight_cv_;

  // Prefetch queue
  std::queue<PrefetchTask> prefetch_queue_;
  std::mutex prefetch_mutex_;
  std::condition_variable prefetch_cv_;
  std::vector<std::thread> prefetch_threads_;

  void prefetch_worker();
  void submit_prefetch_tasks();

  // ==================================================================
  // Background I/O (for persistence)
  // ==================================================================

  std::queue<StateEntryPtr> io_queue_;
  std::mutex io_mutex_;
  std::condition_variable io_cv_; // Notify workers new entry available
  std::condition_variable
      io_queue_not_full_cv_;      // Notify producers queue not full
  size_t max_io_queue_size_ = 50; // Limit queue to prevent OOM
  std::vector<std::thread> io_threads_;

  void io_worker();

  // ==================================================================
  // Async Eviction (L1 -> L2)
  // ==================================================================

  std::queue<StateEntryPtr> eviction_queue_;
  std::mutex eviction_mutex_;
  std::condition_variable eviction_cv_;
  std::atomic<size_t> pending_eviction_bytes_{0};
  std::thread eviction_thread_;

  void eviction_worker();
  void mark_for_eviction(StateEntryPtr entry);
  void evict_l1_async(size_t required_bytes);

  // ==================================================================
  // Internal Helpers
  // ==================================================================

  // Entry management
  StateEntryPtr find_or_create_entry(hash_type hash);

  // Cache operations
  void insert_to_l1(hash_type hash, std::vector<uint8_t> &&raw_data);
  void insert_to_l2(hash_type hash, std::vector<uint8_t> &&compressed_data);
  bool move_l2_to_l1(hash_type hash);
  void evict_l2_if_needed(size_t required_bytes);
  void update_hot_lru(hash_type hash);
  void update_warm_lru(hash_type hash);

  // Compression - delegated to StateStorePacked
  std::vector<uint8_t> compress(const std::vector<uint8_t> &data);
  std::vector<uint8_t> decompress(const std::vector<uint8_t> &data,
                                  size_t original_size);

  // Disk operations
  bool write_to_disk(hash_type hash,
                     const std::vector<uint8_t> &compressed_data,
                     size_t original_size);
  bool read_from_disk(hash_type hash, std::vector<uint8_t> &data);
  bool read_compressed_from_disk(hash_type hash,
                                 std::vector<uint8_t> &compressed_data);
  bool exists_on_disk(hash_type hash);
};

/* ==============================================================================
 * C-style Interface
 * ==============================================================================
 */

#ifdef __cplusplus
extern "C"
{
#endif

  void state_store_init(void);
  uint32_t state_store_save(const void *data, size_t len);
  ssize_t state_store_load(uint64_t hash, void *data, size_t max_len);
  int state_store_exists(uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif /* __STATE_STORE_H */
