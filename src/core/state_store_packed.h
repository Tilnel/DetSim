/*
 * state_store_packed.h - Packed storage for state snapshots
 *
 * Design:
 * - Append-only segmented storage to avoid millions of small files
 * - Global index maps hash -> (segment_id, offset, size)
 * - Incremental index for fast writes
 * - ZSTD compression per entry
 * - Reference: sysstate_store.h
 */

#ifndef __STATE_STORE_PACKED_H
#define __STATE_STORE_PACKED_H

#include "types.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declare ZSTD types
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;

/* Entry in the global index */
/* Packed to avoid alignment padding */
struct __attribute__((packed)) PackedIndexEntry
{
  uint32_t segment_id; // Which segment file
  uint64_t offset;     // Offset within segment data file
  uint32_t compressed_size;
  uint32_t original_size; // Uncompressed size for pre-allocation
  uint32_t checksum;
};
static_assert(sizeof(PackedIndexEntry) == 24,
              "PackedIndexEntry size must be 24 bytes");

/* Data entry header (stored in segment file) */
/* Packed to avoid alignment padding - total size must be exactly 24 bytes */
struct __attribute__((packed)) PackedDataHeader
{
  uint32_t magic; // 0x53544154 "STAT"
  hash_type hash; // 64-bit hash
  uint32_t compressed_size;
  uint32_t original_size;
  uint32_t checksum;
};
static_assert(sizeof(PackedDataHeader) == 24,
              "PackedDataHeader size must be 24 bytes");

/* Segment info */
/* Packed to avoid alignment padding */
struct __attribute__((packed)) SegmentInfo
{
  uint32_t id;
  uint64_t data_size;   // Current size of data file
  uint64_t entry_count; // Number of entries in this segment
};
static_assert(sizeof(SegmentInfo) == 20, "SegmentInfo size must be 20 bytes");

class StateStorePacked
{
  public:
  static StateStorePacked &instance();

  struct Config
  {
    // Segment settings
    size_t max_segment_size = 1024ULL * 1024 * 1024; // 1GB default
    size_t max_entries_per_segment = 1000000;        // 1M entries default

    // Compression settings
    int compression_level = 1;

    // Index flush settings
    size_t index_flush_interval_entries = 10000;
    size_t index_flush_interval_seconds = 30;

    // Storage path
    std::string storage_path = "memory_packed";
  };

  /* Initialize/load existing index */
  void init(const Config &config);
  void init(); // Use default config

  /* Save state data with precomputed hash
   * NOTE: data can be either raw or compressed. If compressed, set
   * is_compressed=true. hash must be precomputed from the RAW data (by caller).
   * original_size is required if is_compressed=true (size of uncompressed
   * data).
   */
  hash_type save(const void *data, size_t len, hash_type hash,
                 bool is_compressed = false, size_t original_size = 0);

  /* Load state data, returns bytes read or -1 */
  ssize_t load(hash_type hash, std::vector<uint8_t> &data);

  /* Load compressed data directly (for prefetch), returns bytes read or -1 */
  ssize_t load_compressed(hash_type hash,
                          std::vector<uint8_t> &compressed_data);

  /* Get original size for a hash (for decompression) */
  size_t get_original_size(hash_type hash);

  /* Check if hash exists */
  bool exists(hash_type hash);

  /* Wait for persistence (compatibility with StateStore) */
  bool wait_persisted(hash_type hash, uint64_t timeout_ms = 0);

  /* Flush index to disk */
  void flush_index();

  /* Compact data files (rewrite to remove holes) */
  void compact();

  /* Rebuild index from data files (recovery mode) */
  void rebuild_index();

  /* Shutdown and cleanup */
  void shutdown();

  size_t get_entry_count() const { return index_.size(); }
  size_t get_segment_count() const { return segments_.size(); }
  size_t get_total_data_size() const;
  bool verify_and_repair();
  void dump_debug_info();

  /* Find hashes by prefix (for auto-completion) */
  std::vector<hash_type> find_by_prefix(const std::string &prefix);

  /* Save segment info (for shutdown) */
  void save_segments_info();

  /* Compression - shared with StateStore */
  std::vector<uint8_t> compress(const std::vector<uint8_t> &data);
  std::vector<uint8_t> decompress(const std::vector<uint8_t> &data,
                                  size_t original_size);

  private:
  StateStorePacked() = default;
  ~StateStorePacked();

  /* Prevent copies */
  StateStorePacked(const StateStorePacked &) = delete;
  StateStorePacked &operator=(const StateStorePacked &) = delete;

  /* File paths */
  std::string get_segment_data_path(uint32_t segment_id);
  std::string get_segment_index_path(uint32_t segment_id);
  std::string get_global_index_path();
  std::string get_incremental_index_path();
  std::string get_segments_info_path();

  /* Load/save index */
  void load_index();
  void load_incremental_index();
  void flush_index_unlocked();
  void append_to_incremental_index(hash_type hash,
                                   const PackedIndexEntry &entry);
  void merge_incremental_index();
  bool should_flush_index();

  /* Segment management */
  void ensure_active_segment();
  void rotate_segment();
  void load_segments_info();

  /* Checksum */
  uint32_t compute_checksum(const std::vector<uint8_t> &data);

  /* Data members */
  Config config_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};

  /* Global index: hash -> entry */
  std::unordered_map<hash_type, PackedIndexEntry> index_;

  /* Incremental index: entries not yet merged to main index */
  std::unordered_map<hash_type, PackedIndexEntry> incremental_index_;

  /* Segment management */
  std::vector<SegmentInfo> segments_;
  uint32_t active_segment_id_ = 0;
  int active_segment_fd_ = -1; // File descriptor for active segment (O_APPEND)

  /* Statistics */
  std::atomic<size_t> saved_count_{0};
  std::atomic<size_t> loaded_count_{0};

  /* Last flush time */
  std::chrono::steady_clock::time_point last_flush_time_;

  /* Mutex for thread safety */
  mutable std::recursive_mutex mutex_;

  /* ZSTD contexts (per-thread caching like StateStore) */
  struct ZSTDContextCache
  {
    ZSTD_CCtx *cctx = nullptr;
    ZSTD_DCtx *dctx = nullptr;
    std::mutex mutex;

    ~ZSTDContextCache() { reset(); }
    void reset();
    ZSTD_CCtx *getCCtx();
    ZSTD_DCtx *getDCtx();
  };

  mutable std::mutex ctx_cache_mutex_;
  std::unordered_map<std::thread::id, std::unique_ptr<ZSTDContextCache>>
      ctx_caches_;

  ZSTDContextCache *get_thread_ctx_cache();
};

/* C-style interface for compatibility */
#ifdef __cplusplus
extern "C"
{
#endif

  void state_store_packed_init(void);
  uint64_t state_store_packed_save(const void *data, size_t len);
  ssize_t state_store_packed_load(uint64_t hash, void *data, size_t max_len);
  int state_store_packed_exists(uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif /* __STATE_STORE_PACKED_H */
