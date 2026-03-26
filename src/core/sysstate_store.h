/*
 * sysstate_store.h - Packed storage for sys_state to avoid filesystem overhead
 *
 * Problem: Each sys_state file is only ~20 bytes, but filesystem uses 4KB
 * blocks Solution: Pack multiple sys_states into a single file with index
 */

#ifndef __SYSSTATE_STORE_H
#define __SYSSTATE_STORE_H

#include "types.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* Entry in the index */
/* Packed to avoid alignment padding */
struct __attribute__((packed)) SysStateEntry
{
  uint64_t offset;   // Offset in data file
  uint32_t size;     // Size of serialized data
  uint32_t checksum; // Simple checksum for validation
};
static_assert(sizeof(SysStateEntry) == 16,
              "SysStateEntry size must be 16 bytes");

class SysStateStore
{
  public:
  static SysStateStore &instance();

  /* Initialize/load existing index */
  void init();

  /* Save sys_state data, returns true if successful */
  bool save(hash_type hash, const std::vector<uint8_t> &data);

  /* Load sys_state data, returns true if found */
  bool load(hash_type hash, std::vector<uint8_t> &data);

  /* Check if hash exists */
  bool exists(hash_type hash);

  /* Find all hashes matching a prefix (for cmd_load auto-completion) */
  std::vector<hash_type> find_by_prefix(const std::string &prefix);

  /* Flush index to disk */
  void flush_index();

  /* Internal: flush without locking (assumes mutex held) */
  void flush_index_unlocked();

  /* Get statistics */
  size_t get_entry_count() const { return index_.size(); }
  size_t get_incremental_count() const { return incremental_index_.size(); }
  size_t get_data_file_size() const { return data_file_size_; }

  /* Compact data file (rewrite to remove holes from duplicate saves) */
  void compact();

  /* Rebuild index from data file (recovery mode) */
  void rebuild_index();

  /* Shutdown and cleanup */
  void shutdown();

  /* Incremental index operations */
  void load_incremental_index();
  void append_to_incremental_index(hash_type hash, const SysStateEntry &entry);
  void merge_incremental_index();
  bool should_flush_index();

  private:
  SysStateStore() = default;
  ~SysStateStore();

  /* Prevent copies */
  SysStateStore(const SysStateStore &) = delete;
  SysStateStore &operator=(const SysStateStore &) = delete;

  /* Load index from disk */
  void load_index();

  /* Compute simple checksum */
  uint32_t compute_checksum(const std::vector<uint8_t> &data);

  void preload_all_data();

  /* File paths */
  static constexpr const char *DATA_FILE = "sstate/packed.dat";
  static constexpr const char *INDEX_FILE = "sstate/packed.idx";
  static constexpr const char *INCREMENTAL_INDEX_FILE = "sstate/packed.inc";

  /* In-memory index: hash -> entry */
  std::unordered_map<hash_type, SysStateEntry> index_;

  /* Incremental index: entries not yet merged to main index */
  std::unordered_map<hash_type, SysStateEntry> incremental_index_;

  std::unordered_map<hash_type, std::vector<uint8_t>> data_cache_;

  /* Current data file size (for append) */
  uint64_t data_file_size_ = 0;

  /* Mutex for thread safety (recursive to avoid deadlocks from nested calls) */
  mutable std::recursive_mutex mutex_;

  /* Whether initialized */
  bool initialized_ = false;

  /* Statistics */
  size_t saved_count_ = 0;
  size_t loaded_count_ = 0;

  /* Last full index flush time */
  std::chrono::steady_clock::time_point last_flush_time_;
};

#endif /* __SYSSTATE_STORE_H */
