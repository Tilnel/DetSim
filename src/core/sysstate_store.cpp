/*
 * sysstate_store.cpp - Packed storage implementation
 */

#include "sysstate_store.h"
#include "debug.h"
#include "utils.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Magic number for index file */
#define INDEX_MAGIC 0x53535953 // "SYSS"
#define INDEX_VERSION 2        // Version 2 supports incremental index

/* Magic for incremental index */
#define INCREMENTAL_MAGIC 0x494E4458 // "INDX"

/* Magic and header for data entries */
#define ENTRY_MAGIC 0x53534545 // "EESS" (Entry for SysState)

/* Index flush settings */
#define INDEX_FLUSH_INTERVAL_ENTRIES 10000 // Flush every N entries (was 10)
#define INDEX_FLUSH_INTERVAL_SECONDS 30    // Or every N seconds

/* Packed to avoid alignment padding */
struct __attribute__((packed)) DataEntryHeader
{
  uint32_t magic;
  hash_type hash;
  uint32_t size;
  uint32_t checksum;
};
static_assert(sizeof(DataEntryHeader) == 20,
              "DataEntryHeader size must be 20 bytes");

SysStateStore &SysStateStore::instance()
{
  static SysStateStore instance;
  if (!instance.initialized_)
  {
    instance.init();
  }
  return instance;
}

SysStateStore::~SysStateStore()
{
  LOG_INFO("SysStateStore destructor, initialized=%d, entries=%zu",
           initialized_, index_.size());
  if (initialized_)
  {
    // Final flush: merge incremental to main
    merge_incremental_index();
    flush_index();
    LOG_INFO("SysStateStore flushed index on exit");
  }
}

void SysStateStore::shutdown()
{
  if (!initialized_)
    return;
  initialized_ = false;

  std::lock_guard<std::recursive_mutex> lock(mutex_);

  // Final flush
  merge_incremental_index();
  flush_index_unlocked();

  // Clear all in-memory data
  index_.clear();
  incremental_index_.clear();
  data_cache_.clear();
  data_file_size_ = 0;
  saved_count_ = 0;
  loaded_count_ = 0;

  LOG_INFO("SysStateStore shutdown completed");
}

uint32_t SysStateStore::compute_checksum(const std::vector<uint8_t> &data)
{
  uint32_t sum = 0;
  for (auto b : data)
  {
    sum = (sum * 31) + b;
  }
  return sum;
}

void SysStateStore::init()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (initialized_)
    return;

  /* Ensure sstate directory exists */
  fileutils::ensure_directory_for_file(DATA_FILE);

  /* Load existing index */
  load_index();

  /* Load incremental index (new entries since last full flush) */
  load_incremental_index();

  /* Get current data file size */
  struct stat st;
  if (stat(DATA_FILE, &st) == 0)
  {
    data_file_size_ = st.st_size;
  }
  else
  {
    data_file_size_ = 0;
  }

  /* If no index but data file exists, rebuild index */
  if (index_.empty() && data_file_size_ > 0)
  {
    rebuild_index();
  }

  preload_all_data();

  last_flush_time_ = std::chrono::steady_clock::now();

  initialized_ = true;
  LOG_INFO(
      "SysStateStore initialized: %zu entries, %zu cached, data file %zu bytes",
      index_.size(), data_cache_.size(), data_file_size_);
}

void SysStateStore::load_index()
{
  FILE *fp = fopen(INDEX_FILE, "rb");
  if (!fp)
  {
    /* No existing index, start fresh */
    return;
  }

  /* Read header */
  uint32_t magic, version;
  if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != INDEX_MAGIC)
  {
    LOG_WARN("Invalid index file magic");
    fclose(fp);
    return;
  }
  if (fread(&version, sizeof(version), 1, fp) != 1 || version != INDEX_VERSION)
  {
    LOG_WARN("Unsupported index file version");
    fclose(fp);
    return;
  }

  /* Read number of entries */
  uint64_t count;
  if (fread(&count, sizeof(count), 1, fp) != 1)
  {
    LOG_WARN("Failed to read index count");
    fclose(fp);
    return;
  }

  /* Read entries */
  for (uint64_t i = 0; i < count; i++)
  {
    hash_type hash;
    SysStateEntry entry;

    if (fread(&hash, sizeof(hash), 1, fp) != 1)
      break;
    if (fread(&entry.offset, sizeof(entry.offset), 1, fp) != 1)
      break;
    if (fread(&entry.size, sizeof(entry.size), 1, fp) != 1)
      break;
    if (fread(&entry.checksum, sizeof(entry.checksum), 1, fp) != 1)
      break;

    index_[hash] = entry;
  }

  fclose(fp);
  LOG_INFO("Loaded %zu entries from index", index_.size());
}

void SysStateStore::flush_index_unlocked()
{
  /* Internal version - assumes mutex already held */
  fileutils::ensure_directory_for_file(INDEX_FILE);

  /* Write to temp file first */
  std::string temp_file = std::string(INDEX_FILE) + ".tmp";
  FILE *fp = fopen(temp_file.c_str(), "wb");
  if (!fp)
  {
    LOG_ERROR("Failed to open index file for writing");
    return;
  }

  /* Write header */
  uint32_t magic = INDEX_MAGIC;
  uint32_t version = INDEX_VERSION;
  fwrite(&magic, sizeof(magic), 1, fp);
  fwrite(&version, sizeof(version), 1, fp);

  /* Write entry count */
  uint64_t count = index_.size();
  fwrite(&count, sizeof(count), 1, fp);

  /* Write entries */
  for (const auto &[hash, entry] : index_)
  {
    fwrite(&hash, sizeof(hash), 1, fp);
    fwrite(&entry.offset, sizeof(entry.offset), 1, fp);
    fwrite(&entry.size, sizeof(entry.size), 1, fp);
    fwrite(&entry.checksum, sizeof(entry.checksum), 1, fp);
  }

  fclose(fp);

  /* Atomic rename */
  if (rename(temp_file.c_str(), INDEX_FILE) != 0)
  {
    LOG_ERROR("Failed to rename index file");
    unlink(temp_file.c_str());
  }
}

void SysStateStore::flush_index()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  flush_index_unlocked();
}

/* Check if we should flush the full index */
bool SysStateStore::should_flush_index()
{
  // Check entry count interval
  if (saved_count_ % INDEX_FLUSH_INTERVAL_ENTRIES == 0)
  {
    return true;
  }

  // Check time interval
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::seconds>(now - last_flush_time_)
          .count();
  if (elapsed >= INDEX_FLUSH_INTERVAL_SECONDS)
  {
    return true;
  }

  return false;
}

/* Append a single entry to incremental index (fast, append-only) */
void SysStateStore::append_to_incremental_index(hash_type hash,
                                                const SysStateEntry &entry)
{
  // Add to in-memory incremental index
  incremental_index_[hash] = entry;

  // Append to incremental index file (no rewrite, very fast)
  FILE *fp = fopen(INCREMENTAL_INDEX_FILE, "ab");
  if (!fp)
  {
    LOG_ERROR("Failed to open incremental index file for append");
    return;
  }

  fwrite(&hash, sizeof(hash), 1, fp);
  fwrite(&entry.offset, sizeof(entry.offset), 1, fp);
  fwrite(&entry.size, sizeof(entry.size), 1, fp);
  fwrite(&entry.checksum, sizeof(entry.checksum), 1, fp);

  fclose(fp);
}

/* Load incremental index on startup */
void SysStateStore::load_incremental_index()
{
  FILE *fp = fopen(INCREMENTAL_INDEX_FILE, "rb");
  if (!fp)
  {
    /* No incremental index yet, that's fine */
    return;
  }

  /* Read entries */
  while (!feof(fp))
  {
    hash_type hash;
    SysStateEntry entry;

    if (fread(&hash, sizeof(hash), 1, fp) != 1)
      break;
    if (fread(&entry.offset, sizeof(entry.offset), 1, fp) != 1)
      break;
    if (fread(&entry.size, sizeof(entry.size), 1, fp) != 1)
      break;
    if (fread(&entry.checksum, sizeof(entry.checksum), 1, fp) != 1)
      break;

    incremental_index_[hash] = entry;
    index_[hash] = entry; // Also add to main index in memory
  }

  fclose(fp);

  LOG_INFO("Loaded %zu entries from incremental index",
           incremental_index_.size());
}

/* Merge incremental index into main index and clear incremental */
void SysStateStore::merge_incremental_index()
{
  if (incremental_index_.empty())
  {
    return;
  }

  // Merge into main index (in-memory only, main index file is rewritten by
  // flush_index_unlocked)
  for (const auto &[hash, entry] : incremental_index_)
  {
    index_[hash] = entry;
  }

  // Clear incremental index file
  FILE *fp = fopen(INCREMENTAL_INDEX_FILE, "wb");
  if (fp)
  {
    fclose(fp);
  }

  incremental_index_.clear();
  last_flush_time_ = std::chrono::steady_clock::now();

  LOG_DEBUG("Merged incremental index, total entries now: %zu", index_.size());
}

void SysStateStore::preload_all_data()
{
  if (index_.empty())
  {
    return;
  }

  int fd = open(DATA_FILE, O_RDONLY);
  if (fd < 0)
  {
    LOG_WARN("Failed to open data file for preloading");
    return;
  }

  size_t loaded = 0;
  for (const auto &[hash, entry] : index_)
  {
    if (data_cache_.count(hash))
    {
      continue;
    }

    off_t header_pos = entry.offset - sizeof(DataEntryHeader);
    if (lseek(fd, header_pos, SEEK_SET) != header_pos)
    {
      continue;
    }

    DataEntryHeader header;
    if (read(fd, &header, sizeof(header)) != sizeof(header))
    {
      continue;
    }

    if (header.magic != ENTRY_MAGIC || header.hash != hash)
    {
      continue;
    }

    std::vector<uint8_t> data(entry.size);
    ssize_t read_bytes = read(fd, data.data(), entry.size);
    if (read_bytes != (ssize_t)entry.size)
    {
      continue;
    }

    if (compute_checksum(data) == entry.checksum)
    {
      data_cache_[hash] = std::move(data);
      loaded++;
    }
  }

  close(fd);
  LOG_INFO("Preloaded %zu entries into memory cache", loaded);
}

bool SysStateStore::save(hash_type hash, const std::vector<uint8_t> &data)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  /* Check if already exists */
  if (index_.count(hash))
  {
    return true; // Already saved
  }

  /* Prepare entry with header */
  DataEntryHeader header;
  header.magic = ENTRY_MAGIC;
  header.hash = hash;
  header.size = data.size();
  header.checksum = compute_checksum(data);

  SysStateEntry entry;
  entry.offset = data_file_size_ + sizeof(DataEntryHeader);
  entry.size = data.size();
  entry.checksum = header.checksum;

  /* Append to data file */
  int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open data file for writing");
    return false;
  }

  /* Write header */
  ssize_t written = write(fd, &header, sizeof(header));
  if (written != sizeof(header))
  {
    LOG_ERROR("Failed to write entry header");
    close(fd);
    return false;
  }

  /* Write data */
  written = write(fd, data.data(), data.size());
  close(fd);

  if (written != (ssize_t)data.size())
  {
    LOG_ERROR("Failed to write data");
    return false;
  }

  index_[hash] = entry;
  data_cache_[hash] = data;
  data_file_size_ += sizeof(DataEntryHeader) + entry.size;
  saved_count_++;

  /* Append to incremental index (fast) */
  append_to_incremental_index(hash, entry);

  /* Flush full index periodically (much less frequent now) */
  if (should_flush_index())
  {
    merge_incremental_index();
    flush_index_unlocked();
  }

  return true;
}

bool SysStateStore::load(hash_type hash, std::vector<uint8_t> &data)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto cache_it = data_cache_.find(hash);
  if (cache_it != data_cache_.end())
  {
    data = cache_it->second;
    loaded_count_++;
    return true;
  }

  auto it = index_.find(hash);
  if (it == index_.end())
  {
    return false;
  }

  const SysStateEntry &entry = it->second;

  int fd = open(DATA_FILE, O_RDONLY);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open data file for reading");
    return false;
  }

  off_t header_pos = entry.offset - sizeof(DataEntryHeader);
  if (lseek(fd, header_pos, SEEK_SET) != header_pos)
  {
    LOG_ERROR("Failed to seek in data file");
    close(fd);
    return false;
  }

  DataEntryHeader header;
  if (read(fd, &header, sizeof(header)) != sizeof(header))
  {
    LOG_ERROR("Failed to read entry header");
    close(fd);
    return false;
  }

  if (header.magic != ENTRY_MAGIC || header.hash != hash)
  {
    LOG_ERROR("Entry header mismatch for hash %016lx", hash);
    close(fd);
    return false;
  }

  data.resize(entry.size);
  ssize_t read_bytes = read(fd, data.data(), entry.size);
  close(fd);

  if (read_bytes != (ssize_t)entry.size)
  {
    LOG_ERROR("Failed to read data");
    return false;
  }

  if (compute_checksum(data) != entry.checksum)
  {
    LOG_ERROR("Checksum mismatch for hash %016lx", hash);
    return false;
  }

  data_cache_[hash] = data;
  loaded_count_++;
  return true;
}

bool SysStateStore::exists(hash_type hash)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return index_.count(hash) > 0;
}

std::vector<hash_type> SysStateStore::find_by_prefix(const std::string &prefix)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<hash_type> matches;

  // Convert prefix to hash for comparison
  hash_type prefix_hash = 0;
  if (sscanf(prefix.c_str(), "%lx", &prefix_hash) < 1)
  {
    return matches;
  }

  // Calculate mask based on prefix length
  // Each hex digit = 4 bits, hash is 64-bit (16 hex digits max)
  size_t hex_digits = prefix.length();
  if (hex_digits > 16)
    hex_digits = 16; // hash is 64-bit (16 hex digits max)

  int shift_bits = 64 - (hex_digits * 4);
  hash_type prefix_masked =
      (shift_bits >= 64) ? 0 : (prefix_hash << shift_bits);
  hash_type mask = (shift_bits >= 64) ? 0xFFFFFFFFFFFFFFFFull
                                      : (0xFFFFFFFFFFFFFFFFull << shift_bits);

  // Search in main index
  for (const auto &[hash, entry] : index_)
  {
    if ((hash & mask) == prefix_masked)
    {
      matches.push_back(hash);
    }
  }

  // Search in incremental index
  for (const auto &[hash, entry] : incremental_index_)
  {
    if ((hash & mask) == prefix_masked)
    {
      // Avoid duplicates
      if (std::find(matches.begin(), matches.end(), hash) == matches.end())
      {
        matches.push_back(hash);
      }
    }
  }

  return matches;
}

void SysStateStore::rebuild_index()
{
  LOG_INFO("Rebuilding index from data file...");

  int fd = open(DATA_FILE, O_RDONLY);
  if (fd < 0)
  {
    LOG_ERROR("Cannot open data file for rebuilding");
    return;
  }

  index_.clear();
  off_t offset = 0;
  struct stat st;

  if (fstat(fd, &st) != 0)
  {
    close(fd);
    return;
  }

  off_t file_size = st.st_size;
  size_t entries_found = 0;

  while (offset < file_size)
  {
    /* Read header */
    DataEntryHeader header;
    if (pread(fd, &header, sizeof(header), offset) != sizeof(header))
    {
      break;
    }

    /* Verify magic */
    if (header.magic != ENTRY_MAGIC)
    {
      LOG_WARN("Invalid magic at offset %ld, stopping rebuild", offset);
      break;
    }

    /* Create entry */
    SysStateEntry entry;
    entry.offset = offset + sizeof(DataEntryHeader);
    entry.size = header.size;
    entry.checksum = header.checksum;

    index_[header.hash] = entry;
    entries_found++;

    /* Move to next entry */
    offset += sizeof(DataEntryHeader) + header.size;
  }

  close(fd);
  data_file_size_ = file_size;

  LOG_INFO("Rebuilt index with %zu entries", entries_found);

  /* Save the rebuilt index (mutex already held) */
  flush_index_unlocked();
}

void SysStateStore::compact()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  LOG_INFO("Compacting sysstate store...");

  /* Create new data file */
  std::string new_data_file = std::string(DATA_FILE) + ".new";
  int new_fd = open(new_data_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (new_fd < 0)
  {
    LOG_ERROR("Failed to create new data file");
    return;
  }

  /* Rewrite all entries */
  uint64_t new_offset = 0;
  std::unordered_map<hash_type, SysStateEntry> new_index;

  for (auto &[hash, entry] : index_)
  {
    /* Read old data */
    int old_fd = open(DATA_FILE, O_RDONLY);
    if (old_fd < 0)
      continue;

    std::vector<uint8_t> data(entry.size);
    if (pread(old_fd, data.data(), entry.size, entry.offset) ==
        (ssize_t)entry.size)
    {
      /* Prepare new header */
      DataEntryHeader header;
      header.magic = ENTRY_MAGIC;
      header.hash = hash;
      header.size = entry.size;
      header.checksum = entry.checksum;

      /* Write to new file */
      if (write(new_fd, &header, sizeof(header)) == sizeof(header))
      {
        if (write(new_fd, data.data(), entry.size) == (ssize_t)entry.size)
        {
          /* Update entry */
          SysStateEntry new_entry = entry;
          new_entry.offset = new_offset + sizeof(DataEntryHeader);
          new_index[hash] = new_entry;
          new_offset += sizeof(DataEntryHeader) + entry.size;
        }
      }
    }
    close(old_fd);
  }

  close(new_fd);

  /* Replace old file */
  if (rename(new_data_file.c_str(), DATA_FILE) == 0)
  {
    index_ = std::move(new_index);
    data_file_size_ = new_offset;
    flush_index();
    LOG_INFO("Compaction complete: %zu entries, %zu bytes", index_.size(),
             data_file_size_);
  }
  else
  {
    LOG_ERROR("Failed to rename compacted file");
    unlink(new_data_file.c_str());
  }
}
