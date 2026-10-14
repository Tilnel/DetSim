#ifndef __SYSSTATE_STORE_H
#define __SYSSTATE_STORE_H

#include "types.h"
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct __attribute__((packed)) SysStateEntry
{
  uint64_t offset;
  uint32_t size;
  uint32_t checksum;
};
static_assert(sizeof(SysStateEntry) == 16);

// The checksummed append log is authoritative. Historical packed.idx/packed.inc
// files are no longer used: they could lose records between journal truncation
// and checkpoint replacement. Metadata payloads use a bounded, lazy LRU cache.
class SysStateStore
{
public:
  static SysStateStore &instance();
  void init();
  // Read-only tools share record validation without creating a store or indexes.
  static void inspect(const std::string &directory,
      const std::function<void(hash_type, const SysStateEntry &, const std::vector<uint8_t> &)> &visitor);
  bool save(hash_type hash, const std::vector<uint8_t> &data);
  bool load(hash_type hash, std::vector<uint8_t> &data);
  bool exists(hash_type hash);
  std::vector<hash_type> find_by_prefix(const std::string &prefix);

  // Completion/durability barrier for every successful save before this call.
  void flush_index();
  size_t get_entry_count() const;
  size_t get_data_file_size() const;
  size_t get_incremental_count() const { return 0; }
  // No records are deleted or overwritten, so compact only validates the log.
  void compact();
  void rebuild_index();
  void shutdown();

private:
  SysStateStore() = default;
  ~SysStateStore();
  SysStateStore(const SysStateStore &) = delete;
  SysStateStore &operator=(const SysStateStore &) = delete;
  void init_unlocked();
  void check_ready() const;
  void flush_unlocked();
  void scan_unlocked();
  void cache_data(hash_type hash, const std::vector<uint8_t> &data);
  void clear_cache();
  const std::vector<uint8_t> *cached_data(hash_type hash);
  std::vector<uint8_t> read_entry(hash_type hash, const SysStateEntry &entry) const;
  static uint32_t compute_checksum(const std::vector<uint8_t> &data);

  static constexpr const char *DATA_FILE = "sstate/packed.dat";
  std::unordered_map<hash_type, SysStateEntry> index_;
  struct CachedData {
    std::vector<uint8_t> data;
    std::list<hash_type>::iterator position;
  };
  static constexpr size_t CACHE_MAX_ENTRIES = 16384;
  static constexpr size_t CACHE_MAX_BYTES = 1024 * 1024;
  std::unordered_map<hash_type, CachedData> cache_;
  std::list<hash_type> cache_lru_;
  size_t cache_bytes_ = 0;
  uint64_t data_file_size_ = 0;
  mutable std::mutex mutex_;
  int fd_ = -1;
  int directory_fd_ = -1;
  bool initialized_ = false;
  bool failed_ = false;
};
#endif
