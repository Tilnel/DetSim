/*
 * state_store_packed.cpp - Packed storage implementation
 *
 * Append-only segmented storage with global index
 */

#include "state_store_packed.h"
#include "debug.h"
#include "utils.h"

#include <zstd.h>
#include <zstd_errors.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

/* Magic numbers */
#define INDEX_MAGIC 0x53544149 // "STAT"
#define INDEX_VERSION 1
#define INCREMENTAL_MAGIC 0x494E4458 // "INDX"
#define DATA_MAGIC 0x53544154        // "STAT"

/* Index flush settings */
#define INDEX_FLUSH_INTERVAL_ENTRIES 10000
#define INDEX_FLUSH_INTERVAL_SECONDS 30

static uint64_t compute_xxhash(const void *data, size_t len)
{
  return XXHash64::hash(data, len);
}

StateStorePacked::~StateStorePacked()
{
  if (initialized_)
  {
    shutdown();
  }
}

StateStorePacked &StateStorePacked::instance()
{
  static StateStorePacked instance;
  return instance;
}

void StateStorePacked::init(const Config &config)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (initialized_.exchange(true))
  {
    LOG_WARN("StateStorePacked already initialized");
    return;
  }

  config_ = config;

  /* Ensure storage directory exists */
  fileutils::ensure_directory_for_file(get_global_index_path());

  /* Load existing segments info */
  load_segments_info();

  /* Load global index */
  load_index();

  /* Load incremental index */
  load_incremental_index();

  /* If index is empty but segments exist, rebuild index from segments */
  if (index_.empty() && !segments_.empty())
  {
    LOG_WARN("Index empty but %zu segments found, rebuilding index...",
             segments_.size());
    rebuild_index();
  }

  /* Ensure we have an active segment */
  ensure_active_segment();

  last_flush_time_ = std::chrono::steady_clock::now();

  LOG_INFO("StateStorePacked initialized: %zu entries, %zu segments, path=%s",
           index_.size(), segments_.size(), config_.storage_path.c_str());
}

void StateStorePacked::init()
{
  Config default_config;
  init(default_config);
}

void StateStorePacked::shutdown()
{
  if (!initialized_.exchange(false))
  {
    return;
  }
  shutdown_.store(true);

  std::lock_guard<std::recursive_mutex> lock(mutex_);

  /* Flush remaining data */
  if (active_segment_fd_ >= 0)
  {
    close(active_segment_fd_);
    active_segment_fd_ = -1;
  }

  merge_incremental_index();
  flush_index_unlocked();
  save_segments_info();

  /* Clear caches */
  {
    std::lock_guard<std::mutex> ctx_lock(ctx_cache_mutex_);
    ctx_caches_.clear();
  }

  index_.clear();
  incremental_index_.clear();
  segments_.clear();

  LOG_INFO("StateStorePacked shutdown completed");
}

/* ==============================================================================
 * File paths
 * ==============================================================================
 */

std::string StateStorePacked::get_segment_data_path(uint32_t segment_id)
{
  return config_.storage_path + "/segments/" + std::to_string(segment_id) +
         ".dat";
}

std::string StateStorePacked::get_segment_index_path(uint32_t segment_id)
{
  return config_.storage_path + "/segments/" + std::to_string(segment_id) +
         ".idx";
}

std::string StateStorePacked::get_global_index_path()
{
  return config_.storage_path + "/global.idx";
}

std::string StateStorePacked::get_incremental_index_path()
{
  return config_.storage_path + "/global.inc";
}

std::string StateStorePacked::get_segments_info_path()
{
  return config_.storage_path + "/segments.info";
}

/* ==============================================================================
 * Segment management
 * ==============================================================================
 */

void StateStorePacked::load_segments_info()
{
  FILE *fp = fopen(get_segments_info_path().c_str(), "rb");
  if (!fp)
  {
    /* No existing segments info, start fresh */
    active_segment_id_ = 0;
    return;
  }

  uint32_t count;
  if (fread(&count, sizeof(count), 1, fp) != 1)
  {
    fclose(fp);
    active_segment_id_ = 0;
    return;
  }

  for (uint32_t i = 0; i < count; i++)
  {
    SegmentInfo info;
    if (fread(&info.id, sizeof(info.id), 1, fp) != 1)
      break;
    if (fread(&info.data_size, sizeof(info.data_size), 1, fp) != 1)
      break;
    if (fread(&info.entry_count, sizeof(info.entry_count), 1, fp) != 1)
      break;
    segments_.push_back(info);
  }

  fclose(fp);

  /* Determine active segment */
  if (!segments_.empty())
  {
    active_segment_id_ = segments_.back().id;
  }
  else
  {
    active_segment_id_ = 0;
  }

  LOG_INFO("Loaded %zu segments, active=%u", segments_.size(),
           active_segment_id_);
}

void StateStorePacked::save_segments_info()
{
  fileutils::ensure_directory_for_file(get_segments_info_path());

  std::string temp_path = get_segments_info_path() + ".tmp";
  FILE *fp = fopen(temp_path.c_str(), "wb");
  if (!fp)
  {
    LOG_ERROR("Failed to open segments info for writing");
    return;
  }

  uint32_t count = segments_.size();
  fwrite(&count, sizeof(count), 1, fp);

  for (const auto &info : segments_)
  {
    fwrite(&info.id, sizeof(info.id), 1, fp);
    fwrite(&info.data_size, sizeof(info.data_size), 1, fp);
    fwrite(&info.entry_count, sizeof(info.entry_count), 1, fp);
  }

  fclose(fp);

  if (rename(temp_path.c_str(), get_segments_info_path().c_str()) != 0)
  {
    LOG_ERROR("Failed to rename segments info file");
    unlink(temp_path.c_str());
  }
}

void StateStorePacked::ensure_active_segment()
{
  if (active_segment_fd_ >= 0)
  {
    return;
  }

  std::string data_path = get_segment_data_path(active_segment_id_);
  fileutils::ensure_directory_for_file(data_path);

  active_segment_fd_ =
      open(data_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (active_segment_fd_ < 0)
  {
    LOG_ERROR("Failed to open active segment %u: %s", active_segment_id_,
              strerror(errno));
    return;
  }

  /* O_APPEND ensures writes append, but file position may be at start.
   * Seek to end so lseek(SEEK_CUR) returns correct offset for index.
   */
  if (lseek(active_segment_fd_, 0, SEEK_END) < 0)
  {
    LOG_ERROR("Failed to seek to end of active segment %u: %s",
              active_segment_id_, strerror(errno));
    close(active_segment_fd_);
    active_segment_fd_ = -1;
    return;
  }

  /* Find or create segment info */
  SegmentInfo *info = nullptr;
  for (auto &seg : segments_)
  {
    if (seg.id == active_segment_id_)
    {
      info = &seg;
      break;
    }
  }

  if (!info)
  {
    SegmentInfo new_info;
    new_info.id = active_segment_id_;
    new_info.data_size = 0;
    new_info.entry_count = 0;
    segments_.push_back(new_info);
  }

  LOG_DEBUG("Active segment %u opened", active_segment_id_);
}

void StateStorePacked::rotate_segment()
{
  if (active_segment_fd_ >= 0)
  {
    close(active_segment_fd_);
    active_segment_fd_ = -1;
  }

  active_segment_id_++;

  ensure_active_segment();

  LOG_INFO("Rotated to new segment %u", active_segment_id_);
}

/* ==============================================================================
 * Index management
 * ==============================================================================
 */

void StateStorePacked::load_index()
{
  FILE *fp = fopen(get_global_index_path().c_str(), "rb");
  if (!fp)
  {
    return;
  }

  uint32_t magic, version;
  if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != INDEX_MAGIC)
  {
    LOG_WARN("Invalid global index magic");
    fclose(fp);
    return;
  }

  if (fread(&version, sizeof(version), 1, fp) != 1 || version != INDEX_VERSION)
  {
    LOG_WARN("Unsupported global index version");
    fclose(fp);
    return;
  }

  uint64_t count;
  if (fread(&count, sizeof(count), 1, fp) != 1)
  {
    LOG_WARN("Failed to read index count");
    fclose(fp);
    return;
  }

  for (uint64_t i = 0; i < count; i++)
  {
    hash_type hash;
    PackedIndexEntry entry;

    if (fread(&hash, sizeof(hash), 1, fp) != 1)
      break;
    if (fread(&entry.segment_id, sizeof(entry.segment_id), 1, fp) != 1)
      break;
    if (fread(&entry.offset, sizeof(entry.offset), 1, fp) != 1)
      break;
    if (fread(&entry.compressed_size, sizeof(entry.compressed_size), 1, fp) !=
        1)
      break;
    if (fread(&entry.original_size, sizeof(entry.original_size), 1, fp) != 1)
      break;
    if (fread(&entry.checksum, sizeof(entry.checksum), 1, fp) != 1)
      break;

    index_[hash] = entry;
  }

  fclose(fp);
  LOG_INFO("Loaded %zu entries from global index", index_.size());
}

void StateStorePacked::load_incremental_index()
{
  FILE *fp = fopen(get_incremental_index_path().c_str(), "rb");
  if (!fp)
  {
    return;
  }

  uint32_t magic;
  if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != INCREMENTAL_MAGIC)
  {
    fclose(fp);
    return;
  }

  while (!feof(fp))
  {
    hash_type hash;
    PackedIndexEntry entry;

    if (fread(&hash, sizeof(hash), 1, fp) != 1)
      break;
    if (fread(&entry.segment_id, sizeof(entry.segment_id), 1, fp) != 1)
      break;
    if (fread(&entry.offset, sizeof(entry.offset), 1, fp) != 1)
      break;
    if (fread(&entry.compressed_size, sizeof(entry.compressed_size), 1, fp) !=
        1)
      break;
    if (fread(&entry.original_size, sizeof(entry.original_size), 1, fp) != 1)
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

void StateStorePacked::flush_index_unlocked()
{
  fileutils::ensure_directory_for_file(get_global_index_path());

  std::string temp_path = get_global_index_path() + ".tmp";
  FILE *fp = fopen(temp_path.c_str(), "wb");
  if (!fp)
  {
    LOG_ERROR("Failed to open global index for writing");
    return;
  }

  uint32_t magic = INDEX_MAGIC;
  uint32_t version = INDEX_VERSION;
  fwrite(&magic, sizeof(magic), 1, fp);
  fwrite(&version, sizeof(version), 1, fp);

  uint64_t count = index_.size();
  fwrite(&count, sizeof(count), 1, fp);

  for (const auto &[hash, entry] : index_)
  {
    fwrite(&hash, sizeof(hash), 1, fp);
    fwrite(&entry.segment_id, sizeof(entry.segment_id), 1, fp);
    fwrite(&entry.offset, sizeof(entry.offset), 1, fp);
    fwrite(&entry.compressed_size, sizeof(entry.compressed_size), 1, fp);
    fwrite(&entry.original_size, sizeof(entry.original_size), 1, fp);
    fwrite(&entry.checksum, sizeof(entry.checksum), 1, fp);
  }

  fclose(fp);

  if (rename(temp_path.c_str(), get_global_index_path().c_str()) != 0)
  {
    LOG_ERROR("Failed to rename global index file");
    unlink(temp_path.c_str());
  }
}

void StateStorePacked::flush_index()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  flush_index_unlocked();
}

void StateStorePacked::append_to_incremental_index(
    hash_type hash, const PackedIndexEntry &entry)
{
  incremental_index_[hash] = entry;

  std::string inc_path = get_incremental_index_path();

  /* Check if file already exists (to avoid duplicate header) */
  bool file_exists = (access(inc_path.c_str(), F_OK) == 0);

  FILE *fp = fopen(inc_path.c_str(), "ab");
  if (!fp)
  {
    LOG_ERROR("Failed to open incremental index for append");
    return;
  }

  /* Write header only if file is new/empty */
  if (!file_exists || ftell(fp) == 0)
  {
    uint32_t magic = INCREMENTAL_MAGIC;
    fwrite(&magic, sizeof(magic), 1, fp);
  }

  fwrite(&hash, sizeof(hash), 1, fp);
  fwrite(&entry.segment_id, sizeof(entry.segment_id), 1, fp);
  fwrite(&entry.offset, sizeof(entry.offset), 1, fp);
  fwrite(&entry.compressed_size, sizeof(entry.compressed_size), 1, fp);
  fwrite(&entry.original_size, sizeof(entry.original_size), 1, fp);
  fwrite(&entry.checksum, sizeof(entry.checksum), 1, fp);

  fflush(fp);
  fclose(fp);
}

void StateStorePacked::merge_incremental_index()
{
  if (incremental_index_.empty())
  {
    return;
  }

  for (const auto &[hash, entry] : incremental_index_)
  {
    index_[hash] = entry;
  }

  /* Clear incremental file */
  FILE *fp = fopen(get_incremental_index_path().c_str(), "wb");
  if (fp)
  {
    fclose(fp);
  }

  incremental_index_.clear();
  last_flush_time_ = std::chrono::steady_clock::now();

  LOG_DEBUG("Merged incremental index, total entries: %zu", index_.size());
}

bool StateStorePacked::should_flush_index()
{
  if (saved_count_ % INDEX_FLUSH_INTERVAL_ENTRIES == 0)
  {
    return true;
  }

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

/* ==============================================================================
 * Compression
 * ==============================================================================
 */

void StateStorePacked::ZSTDContextCache::reset()
{
  if (cctx)
  {
    ZSTD_freeCCtx(cctx);
    cctx = nullptr;
  }
  if (dctx)
  {
    ZSTD_freeDCtx(dctx);
    dctx = nullptr;
  }
}

ZSTD_CCtx *StateStorePacked::ZSTDContextCache::getCCtx()
{
  if (!cctx)
  {
    cctx = ZSTD_createCCtx();
  }
  return cctx;
}

ZSTD_DCtx *StateStorePacked::ZSTDContextCache::getDCtx()
{
  if (!dctx)
  {
    dctx = ZSTD_createDCtx();
  }
  return dctx;
}

StateStorePacked::ZSTDContextCache *StateStorePacked::get_thread_ctx_cache()
{
  std::thread::id tid = std::this_thread::get_id();

  {
    std::lock_guard<std::mutex> lock(ctx_cache_mutex_);
    auto it = ctx_caches_.find(tid);
    if (it != ctx_caches_.end())
    {
      return it->second.get();
    }
  }

  auto cache = std::make_unique<ZSTDContextCache>();
  auto *ptr = cache.get();

  {
    std::lock_guard<std::mutex> lock(ctx_cache_mutex_);
    ctx_caches_[tid] = std::move(cache);
  }

  return ptr;
}

std::vector<uint8_t>
StateStorePacked::compress(const std::vector<uint8_t> &data)
{
  if (data.empty())
    return {};

  int level = config_.compression_level;
  if (level < 1)
    level = 1;
  if (level > 22)
    level = 22;

  size_t bound = ZSTD_compressBound(data.size());
  if (bound == 0 || bound > SIZE_MAX / 2)
  {
    LOG_ERROR("ZSTD_compressBound failed");
    return {};
  }

  uint8_t *compressed_buf = (uint8_t *)malloc(bound);
  if (!compressed_buf)
  {
    LOG_ERROR("Failed to allocate compression buffer");
    return {};
  }

  auto *cache = get_thread_ctx_cache();
  std::lock_guard<std::mutex> cache_lock(cache->mutex);

  ZSTD_CCtx *cctx = cache->getCCtx();
  if (!cctx)
  {
    LOG_ERROR("Failed to create ZSTD compression context");
    free(compressed_buf);
    return {};
  }

  size_t result = ZSTD_compressCCtx(cctx, compressed_buf, bound, data.data(),
                                    data.size(), level);

  if (ZSTD_isError(result))
  {
    LOG_ERROR("ZSTD compression failed: %s", ZSTD_getErrorName(result));
    free(compressed_buf);
    cache->reset();
    return {};
  }

  std::vector<uint8_t> compressed;
  compressed.reserve(result);
  compressed.assign(compressed_buf, compressed_buf + result);
  free(compressed_buf);

  return compressed;
}

std::vector<uint8_t>
StateStorePacked::decompress(const std::vector<uint8_t> &data,
                             size_t original_size)
{
  if (data.empty())
    return {};

  size_t dst_capacity = original_size;
  if (original_size == 0)
  {
    LOG_WARN(
        "Decompress called with original_size=0, trying to get from frame");
    unsigned long long frame_size =
        ZSTD_getFrameContentSize(data.data(), data.size());
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN &&
        frame_size != ZSTD_CONTENTSIZE_ERROR)
    {
      if (frame_size <= SIZE_MAX)
      {
        dst_capacity = static_cast<size_t>(frame_size);
        LOG_DEBUG("Got frame content size: %zu", dst_capacity);
      }
    }
    else
    {
      LOG_WARN("Could not get frame content size, using fallback");
      dst_capacity = data.size() * 10;
    }
  }

  if (dst_capacity < 64)
    dst_capacity = 64;

  auto *cache = get_thread_ctx_cache();
  std::lock_guard<std::mutex> cache_lock(cache->mutex);

  ZSTD_DCtx *dctx = cache->getDCtx();
  if (!dctx)
  {
    LOG_ERROR("Failed to create ZSTD decompression context");
    return {};
  }

  std::vector<uint8_t> decompressed;
  decompressed.resize(dst_capacity);

  size_t result = ZSTD_decompressDCtx(dctx, decompressed.data(), dst_capacity,
                                      data.data(), data.size());

  if (ZSTD_isError(result))
  {
    LOG_ERROR("ZSTD decompression failed: %s", ZSTD_getErrorName(result));
    cache->reset();
    return {};
  }

  decompressed.resize(result);
  return decompressed;
}

/* ==============================================================================
 * Checksum
 * ==============================================================================
 */

uint32_t StateStorePacked::compute_checksum(const std::vector<uint8_t> &data)
{
  uint32_t sum = 0;
  for (auto b : data)
  {
    sum = (sum * 31) + b;
  }
  return sum;
}

/* ==============================================================================
 * Save / Load
 * ==============================================================================
 */

hash_type StateStorePacked::save(const void *data, size_t len, hash_type hash,
                                 bool is_compressed, size_t original_size)
{
  if (!data || len == 0 || hash == 0)
    return 0;

  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto it = index_.find(hash);
  if (it != index_.end())
  {
    std::vector<uint8_t> verify_data;
    ssize_t loaded = load(hash, verify_data);
    if (loaded > 0)
    {
      return hash;
    }
    LOG_WARN("Hash %016lx in index but data unreadable, re-saving", hash);
    index_.erase(it);
  }

  std::vector<uint8_t> compressed;
  size_t orig_size;

  if (is_compressed)
  {
    /* Data is already compressed */
    compressed.resize(len);
    memcpy(compressed.data(), data, len);
    if (original_size > 0)
    {
      orig_size = original_size;
    }
    else
    {
      LOG_ERROR("Save called with is_compressed=true but original_size=0! "
                "hash=%016lx, len=%zu",
                hash, len);
      orig_size = len; /* Fallback - will cause decompression error! */
    }
  }
  else
  {
    /* Need to compress raw data */
    std::vector<uint8_t> raw_data(len);
    memcpy(raw_data.data(), data, len);
    compressed = compress(raw_data);
    orig_size = len;
  }

  if (compressed.empty())
  {
    LOG_ERROR("Failed to compress data for hash %016lx", hash);
    return 0;
  }

  /* Check if we need to rotate segment */
  SegmentInfo *active_info = nullptr;
  for (auto &seg : segments_)
  {
    if (seg.id == active_segment_id_)
    {
      active_info = &seg;
      break;
    }
  }

  if (active_info &&
      (active_info->data_size + sizeof(PackedDataHeader) + compressed.size() >
           config_.max_segment_size ||
       active_info->entry_count >= config_.max_entries_per_segment))
  {
    rotate_segment();
    active_info = &segments_.back();
  }

  ensure_active_segment();

  /* Prepare header */
  PackedDataHeader header;
  header.magic = DATA_MAGIC;
  header.hash = hash;
  header.compressed_size = compressed.size();
  header.original_size = orig_size;
  /* Compute checksum of original data if available, otherwise use compressed */
  if (is_compressed && original_size > 0)
  {
    /* We don't have original data, use a simple checksum of compressed data */
    header.checksum = compute_checksum(compressed);
  }
  else
  {
    std::vector<uint8_t> raw_data(orig_size);
    if (is_compressed)
    {
      /* Decompress to get raw data for checksum - this is expensive! */
      /* For now, use compressed checksum as workaround */
      header.checksum = compute_checksum(compressed);
    }
    else
    {
      memcpy(raw_data.data(), data, orig_size);
      header.checksum = compute_checksum(raw_data);
    }
  }

  /* Write header + compressed data atomically using O_APPEND */
  /* Note: O_APPEND ensures atomic append, but we need to know the offset */
  /* Get current file size (where header will be written) */
  off_t header_offset = lseek(active_segment_fd_, 0, SEEK_CUR);
  if (header_offset < 0)
  {
    LOG_ERROR("Failed to get current offset");
    return 0;
  }

  /* Verify we're at the end of file */
  struct stat st;
  if (fstat(active_segment_fd_, &st) == 0)
  {
    if (header_offset != st.st_size)
    {
      LOG_WARN("Offset mismatch: lseek=%ld, stat=%ld, correcting",
               (long)header_offset, (long)st.st_size);
      header_offset = st.st_size;
    }
  }

  ssize_t written = write(active_segment_fd_, &header, sizeof(header));
  if (written != sizeof(header))
  {
    LOG_ERROR("Failed to write data header");
    return 0;
  }

  written = write(active_segment_fd_, compressed.data(), compressed.size());
  if (written != (ssize_t)compressed.size())
  {
    LOG_ERROR("Failed to write compressed data");
    return 0;
  }

  /* Update index */
  PackedIndexEntry entry;
  entry.segment_id = active_segment_id_;
  entry.offset = header_offset + sizeof(PackedDataHeader);
  entry.compressed_size = compressed.size();
  entry.original_size =
      orig_size; /* BUG FIX: was 'len', should be 'orig_size' */
  entry.checksum = header.checksum;

  index_[hash] = entry;
  saved_count_++;

  /* Update segment info */
  if (active_info)
  {
    active_info->data_size += sizeof(PackedDataHeader) + compressed.size();
    active_info->entry_count++;
  }

  /* Append to incremental index */
  append_to_incremental_index(hash, entry);

  /* Flush full index periodically */
  if (should_flush_index())
  {
    merge_incremental_index();
    flush_index_unlocked();
    save_segments_info();
  }

  if (active_segment_fd_ >= 0)
  {
    fsync(active_segment_fd_);
  }

  LOG_TRACE("Saved hash %016lx to segment %u, offset %lu, size %zu -> %u", hash,
            entry.segment_id, entry.offset, len, entry.compressed_size);

  return hash;
}

ssize_t StateStorePacked::load(hash_type hash, std::vector<uint8_t> &data)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto it = index_.find(hash);
  if (it == index_.end())
  {
    LOG_DEBUG("Hash %016lx not found in packed index (total entries: %zu)",
              hash, index_.size());
    return -1;
  }

  const PackedIndexEntry &entry = it->second;

  /* Open segment file */
  std::string segment_path = get_segment_data_path(entry.segment_id);
  int fd = open(segment_path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open segment %u: %s", entry.segment_id,
              strerror(errno));
    return -1;
  }

  /* Seek to header position */
  off_t header_pos = entry.offset - sizeof(PackedDataHeader);
  if (lseek(fd, header_pos, SEEK_SET) != header_pos)
  {
    LOG_ERROR("Failed to seek in segment %u", entry.segment_id);
    close(fd);
    return -1;
  }

  /* Read and verify header */
  PackedDataHeader header;
  if (read(fd, &header, sizeof(header)) != sizeof(header))
  {
    LOG_ERROR("Failed to read header from segment %u", entry.segment_id);
    close(fd);
    return -1;
  }

  if (header.magic != DATA_MAGIC || header.hash != hash)
  {
    LOG_ERROR("Header mismatch in segment %u for hash %016lx", entry.segment_id,
              hash);
    close(fd);
    return -1;
  }

  /* Read compressed data */
  std::vector<uint8_t> compressed(entry.compressed_size);
  ssize_t read_bytes = read(fd, compressed.data(), entry.compressed_size);
  close(fd);

  if (read_bytes != (ssize_t)entry.compressed_size)
  {
    LOG_ERROR("Failed to read compressed data from segment %u",
              entry.segment_id);
    return -1;
  }

  /* Verify checksum if sizes match (optional check) */
  if (header.compressed_size != entry.compressed_size)
  {
    LOG_ERROR("Size mismatch for hash %016lx", hash);
    return -1;
  }

  /* Decompress */
  /* Verify compressed checksum first (stored checksum is for compressed data
   * when data is saved as compressed, since we don't have original data) */
  if (compute_checksum(compressed) != entry.checksum)
  {
    LOG_ERROR("Compressed checksum mismatch for hash %016lx", hash);
    return -1;
  }

  data = decompress(compressed, entry.original_size);
  if (data.empty())
  {
    LOG_ERROR("Failed to decompress data for hash %016lx", hash);
    return -1;
  }

  loaded_count_++;
  LOG_TRACE("Loaded hash %016lx from segment %u, offset %lu", hash,
            entry.segment_id, entry.offset);

  return static_cast<ssize_t>(data.size());
}

ssize_t StateStorePacked::load_compressed(hash_type hash,
                                          std::vector<uint8_t> &compressed_data)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto it = index_.find(hash);
  if (it == index_.end())
  {
    return -1;
  }

  const PackedIndexEntry &entry = it->second;

  /* Open segment file */
  std::string segment_path = get_segment_data_path(entry.segment_id);
  int fd = open(segment_path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    return -1;
  }

  /* Seek to data position (skip header) */
  if (lseek(fd, entry.offset, SEEK_SET) != static_cast<off_t>(entry.offset))
  {
    close(fd);
    return -1;
  }

  /* Read compressed data directly */
  compressed_data.resize(entry.compressed_size);
  ssize_t read_bytes = read(fd, compressed_data.data(), entry.compressed_size);
  close(fd);

  if (read_bytes != static_cast<ssize_t>(entry.compressed_size))
  {
    return -1;
  }

  loaded_count_++;
  return static_cast<ssize_t>(entry.compressed_size);
}

bool StateStorePacked::exists(hash_type hash)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  bool found = index_.count(hash) > 0;
  if (!found)
  {
    LOG_DEBUG("Hash %016lx not found in packed index (total entries: %zu)",
              hash, index_.size());
  }
  return found;
}

size_t StateStorePacked::get_original_size(hash_type hash)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = index_.find(hash);
  if (it != index_.end())
  {
    return it->second.original_size;
  }
  return 0;
}

bool StateStorePacked::wait_persisted(hash_type hash, uint64_t timeout_ms)
{
  /* In packed storage, save() is synchronous for disk write, so if save()
   * returned successfully, data is persisted. This is for API compatibility.
   */
  return exists(hash);
}

/* ==============================================================================
 * Rebuild and Compact
 * ==============================================================================
 */

void StateStorePacked::rebuild_index()
{
  LOG_INFO("Rebuilding index from segment files...");

  std::lock_guard<std::recursive_mutex> lock(mutex_);

  index_.clear();
  incremental_index_.clear();

  for (const auto &seg_info : segments_)
  {
    std::string segment_path = get_segment_data_path(seg_info.id);
    int fd = open(segment_path.c_str(), O_RDONLY);
    if (fd < 0)
    {
      LOG_WARN("Cannot open segment %u for rebuilding", seg_info.id);
      continue;
    }

    off_t offset = 0;
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      close(fd);
      continue;
    }

    off_t file_size = st.st_size;
    size_t entries_found = 0;

    while (offset < file_size)
    {
      PackedDataHeader header;
      if (pread(fd, &header, sizeof(header), offset) != sizeof(header))
      {
        break;
      }

      if (header.magic != DATA_MAGIC)
      {
        LOG_WARN("Invalid magic at offset %ld in segment %u", (long)offset,
                 seg_info.id);
        break;
      }

      PackedIndexEntry entry;
      entry.segment_id = seg_info.id;
      entry.offset = offset + sizeof(PackedDataHeader);
      entry.compressed_size = header.compressed_size;
      entry.original_size = header.original_size;
      entry.checksum = header.checksum;

      index_[header.hash] = entry;
      entries_found++;

      offset += sizeof(PackedDataHeader) + header.compressed_size;
    }

    close(fd);
    LOG_INFO("Rebuilt index for segment %u: %zu entries", seg_info.id,
             entries_found);
  }

  LOG_INFO("Total rebuilt index: %zu entries", index_.size());

  /* Save rebuilt index */
  flush_index_unlocked();
}

void StateStorePacked::compact()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  LOG_INFO("Compacting packed storage...");

  /* Close active segment */
  if (active_segment_fd_ >= 0)
  {
    close(active_segment_fd_);
    active_segment_fd_ = -1;
  }

  /* Create new segments */
  std::vector<SegmentInfo> new_segments;
  uint32_t new_segment_id = 0;

  for (const auto &[hash, entry] : index_)
  {
    /* Read old data */
    std::string old_path = get_segment_data_path(entry.segment_id);
    int old_fd = open(old_path.c_str(), O_RDONLY);
    if (old_fd < 0)
      continue;

    std::vector<uint8_t> compressed(entry.compressed_size);
    off_t read_offset = entry.offset; /* Entry points to data, not header */
    if (pread(old_fd, compressed.data(), entry.compressed_size, read_offset) !=
        (ssize_t)entry.compressed_size)
    {
      close(old_fd);
      continue;
    }
    close(old_fd);

    /* Open new segment */
    std::string new_path = get_segment_data_path(new_segment_id);
    fileutils::ensure_directory_for_file(new_path);
    int new_fd = open(new_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (new_fd < 0)
      continue;

    /* Write header and data */
    PackedDataHeader header;
    header.magic = DATA_MAGIC;
    header.hash = hash;
    header.compressed_size = entry.compressed_size;
    header.original_size = entry.original_size;
    header.checksum = entry.checksum;

    if (write(new_fd, &header, sizeof(header)) == sizeof(header))
    {
      if (write(new_fd, compressed.data(), entry.compressed_size) ==
          (ssize_t)entry.compressed_size)
      {
        /* Success - update entry */
        off_t new_offset = lseek(new_fd, 0, SEEK_CUR) - entry.compressed_size;

        if (new_segments.empty() || new_segments.back().id != new_segment_id)
        {
          SegmentInfo info;
          info.id = new_segment_id;
          info.data_size = 0;
          info.entry_count = 0;
          new_segments.push_back(info);
        }

        auto &info = new_segments.back();
        info.data_size += sizeof(PackedDataHeader) + entry.compressed_size;
        info.entry_count++;

        /* Check if we need to rotate */
        if (info.data_size >= config_.max_segment_size ||
            info.entry_count >= config_.max_entries_per_segment)
        {
          new_segment_id++;
        }
      }
    }

    close(new_fd);
  }

  /* Replace old segments with new */
  for (const auto &old_seg : segments_)
  {
    std::string old_path = get_segment_data_path(old_seg.id);
    unlink(old_path.c_str());
  }

  segments_ = std::move(new_segments);
  active_segment_id_ = segments_.empty() ? 0 : segments_.back().id;

  /* Rebuild index with new offsets */
  rebuild_index();

  LOG_INFO("Compaction complete: %zu segments", segments_.size());
}

std::vector<hash_type>
StateStorePacked::find_by_prefix(const std::string &prefix)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<hash_type> matches;

  hash_type prefix_hash = 0;
  if (sscanf(prefix.c_str(), "%lx", &prefix_hash) < 1)
  {
    return matches;
  }

  size_t hex_digits = prefix.length();
  if (hex_digits > 16)
    hex_digits = 16;

  int shift_bits = 64 - (hex_digits * 4);
  hash_type prefix_masked =
      (shift_bits >= 64) ? 0 : (prefix_hash << shift_bits);
  hash_type mask = (shift_bits >= 64) ? 0xFFFFFFFFFFFFFFFFull
                                      : (0xFFFFFFFFFFFFFFFFull << shift_bits);

  for (const auto &[hash, entry] : index_)
  {
    if ((hash & mask) == prefix_masked)
    {
      matches.push_back(hash);
    }
  }

  return matches;
}

size_t StateStorePacked::get_total_data_size() const
{
  size_t total = 0;
  for (const auto &seg : segments_)
  {
    total += seg.data_size;
  }
  return total;
}

/* ==============================================================================
 * C Interface
 * ==============================================================================
 */

extern "C"
{

  void state_store_packed_init(void) { StateStorePacked::instance().init(); }

  uint64_t state_store_packed_save(const void *data, size_t len)
  {
    // C interface: compute hash from raw data
    hash_type hash = compute_xxhash(data, len);
    return StateStorePacked::instance().save(data, len, hash);
  }

  ssize_t state_store_packed_load(uint64_t hash, void *data, size_t max_len)
  {
    std::vector<uint8_t> buffer;
    ssize_t result = StateStorePacked::instance().load(hash, buffer);
    if (result < 0)
      return -1;

    size_t copy_len = std::min(static_cast<size_t>(result), max_len);
    memcpy(data, buffer.data(), copy_len);
    return result;
  }

  int state_store_packed_exists(uint64_t hash)
  {
    return StateStorePacked::instance().exists(hash) ? 1 : 0;
  }

} // extern "C"
