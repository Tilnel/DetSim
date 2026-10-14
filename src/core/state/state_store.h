// Immutable snapshot buffers, bounded LRU caches, and asynchronous persistence.
#ifndef __STATE_STORE_H
#define __STATE_STORE_H
#include "types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    // Bound accepted, unfinished write data; one oversized snapshot is allowed.
    size_t max_pending_bytes = 64ULL * 1024 * 1024;

    // Deprecated allocation knobs, ignored; retained for source compatibility.
    bool enable_malloc_trim =
        false;
    size_t malloc_trim_threshold =
        64ULL * 1024 * 1024;

    int malloc_arena_max = 4;
    size_t malloc_mmap_threshold =
        1024ULL * 1024 * 1024;

    // Storage path for packed storage
    std::string packed_storage_path = "memory";
    bool read_only = false;
  };

  StateStore();
  StateStore(const StateStore &) = delete;
  StateStore &operator=(const StateStore &) = delete;
  ~StateStore();

  void init(const Config &config);
  void init();

  // Save accepts immutable bytes with bounded backpressure; wait confirms durability.
  hash_type save(const void *data, size_t len);
  // Optional byte-equality hint; only resident immutable buffers are consulted.
  hash_type save(const void *data, size_t len, hash_type reference_hash);

  // Load data: check L1 -> L2 -> disk
  ssize_t load(hash_type hash, std::vector<uint8_t> &out_data);

  // Load data returning shared_ptr for zero-copy access (returns nullptr if not
  // found)
  std::shared_ptr<const std::vector<uint8_t>> load_shared(hash_type hash);

  // Check existence
  bool exists(hash_type hash);

  // Wait for accepted writes, then sync data and index. Throws on failure.
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
    uint64_t save_reference_hits = 0; // Equal resident hint avoided content hashing
    uint64_t save_calls = 0;      // Total save() calls
    uint64_t save_dedup_hot = 0;  // Deduplicated at hot cache
    uint64_t save_dedup_warm = 0; // Deduplicated at warm cache
    uint64_t save_dedup_disk = 0; // Deduplicated at disk
    uint64_t save_new = 0;        // New states (actually saved)

    // Eviction statistics
    uint64_t evict_calls = 0;   // Total cache entries evicted
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
  size_t get_l1_capacity() const;
  size_t get_l2_capacity() const;
  size_t get_l1_entry_count() const;
  size_t get_l2_entry_count() const;

  // Queue sizes for monitoring
  size_t get_io_queue_size() const;
  size_t get_prefetch_queue_size() const;
  size_t get_io_queue_capacity() const { return max_io_queue_size_; }

  private:
  using Buffer = std::shared_ptr<const std::vector<uint8_t>>;
  struct CacheEntry {
    Buffer bytes;
    size_t original_size;
    std::list<hash_type>::iterator position;
  };
  using Cache = std::unordered_map<hash_type, CacheEntry>;
  Config config_;
  // API calls may overlap; init/shutdown exclude all calls and join workers.
  mutable std::shared_mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable work_cv_, completion_cv_, space_cv_, prefetch_cv_;
  std::atomic<bool> ever_initialized_{false};
  bool initialized_ = false, stopping_ = false;
  std::exception_ptr failure_;
  Cache hot_cache_, warm_cache_;
  std::list<hash_type> hot_lru_, warm_lru_;
  std::atomic<size_t> hot_memory_usage_{0}, warm_memory_usage_{0};
  Stats stats_;
  std::unordered_map<hash_type, Buffer> pending_;
  std::deque<hash_type> io_queue_;
  size_t pending_bytes_ = 0;
  static constexpr size_t max_io_queue_size_ = 50;
  std::vector<std::thread> io_threads_, prefetch_threads_;
  std::deque<hash_type> state_queue_, prefetch_queue_;
  std::unordered_set<hash_type> prefetch_inflight_;

  void ensure_initialized();
  void check_ready() const; // mutex_ held
  void record_failure(std::exception_ptr failure); // mutex_ held
  void io_worker();
  void prefetch_worker();
  void submit_prefetch_tasks();
  void insert_cache(Cache &, std::list<hash_type> &, std::atomic<size_t> &,
                    size_t capacity, hash_type, Buffer, size_t original_size);
  Buffer find_cache(Cache &, std::list<hash_type> &, hash_type);
  Buffer load_buffer(hash_type); // caller holds lifecycle shared lock
};

extern "C" {
  void state_store_init(void);
  uint64_t state_store_save(const void *data, size_t len);
  ssize_t state_store_load(uint64_t hash, void *data, size_t max_len);
  int state_store_exists(uint64_t hash);
}
#endif
