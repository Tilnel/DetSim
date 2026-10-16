#include "state_store_packed.h"
#include "utils.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

namespace
{
constexpr uint32_t INDEX_MAGIC = 0x53544149, INCREMENTAL_MAGIC = 0x494e4458,
                   DATA_MAGIC = 0x53544154;
struct FD
{
  int value;
  explicit FD(int v) : value(v)
  {
    if (v < 0)
      throw std::runtime_error(std::string("packed open: ") + strerror(errno));
  }
  ~FD() { close(value); }
};
[[noreturn]] void fail(const char *what)
{
  throw std::runtime_error(std::string("packed storage: ") + what);
}
void write_all(int fd, const void *data, size_t n)
{
  auto p = static_cast<const char *>(data);
  while (n)
  {
    ssize_t r = write(fd, p, n);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      fail("write failed");
    p += r;
    n -= r;
  }
}
void read_all(int fd, void *data, size_t n, uint64_t offset)
{
  auto p = static_cast<char *>(data);
  while (n)
  {
    ssize_t r = pread(fd, p, n, offset);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      fail("short or failed read");
    p += r;
    n -= r;
    offset += r;
  }
}
uint64_t file_size(int fd)
{
  struct stat st
  {
  };
  if (fstat(fd, &st) < 0 || st.st_size < 0)
    fail("stat failed");
  return st.st_size;
}
void sync_fd(int fd)
{
  int result;
  do
  {
    result = fsync(fd);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    fail("fsync failed");
}
void sync_dir(const std::string &path)
{
  FD fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  sync_fd(fd.value);
}
struct Context
{
  ZSTD_CCtx *c = ZSTD_createCCtx();
  ZSTD_DCtx *d = ZSTD_createDCtx();
  ~Context()
  {
    ZSTD_freeCCtx(c);
    ZSTD_freeDCtx(d);
  }
};
thread_local Context context;
} // namespace
StateStorePacked &StateStorePacked::instance()
{
  static StateStorePacked s;
  return s;
}
StateStorePacked::~StateStorePacked()
{
  try
  {
    shutdown();
  }
  catch (...)
  {
  }
}
std::string StateStorePacked::get_segment_data_path(uint32_t id)
{
  return config_.storage_path + "/segments/" + std::to_string(id) + ".dat";
}
std::string StateStorePacked::get_global_index_path()
{
  return config_.storage_path + "/global.idx";
}
std::string StateStorePacked::get_incremental_index_path()
{
  return config_.storage_path + "/global.inc";
}
void StateStorePacked::init() { init(Config{}); }
void StateStorePacked::init(const Config &c)
{
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (initialized_)
    return;
  config_ = c;
  compression_level_ = c.compression_level;
  failed_ = false;
  index_.clear();
  segments_.clear();
  try
  {
    if (!c.read_only)
    {
      std::filesystem::create_directories(c.storage_path + "/segments");
      lock_fd_ = open((c.storage_path + "/.packed.lock").c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC, 0644);
      if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) < 0)
        fail("store already open or lock unavailable");
    }
    load_segments_info();
    load_index();
    load_incremental_index();
    if (index_.empty() && !segments_.empty())
    {
      rebuild_index();
      // Publish recovered keys before new appends make the journal nonempty.
      if (!c.read_only)
        flush_index_unlocked();
    }
    for (auto &segment : segments_)
      segment.entry_count = 0;
    std::unordered_map<uint32_t, uint64_t> indexed_bytes;
    // Index entries must reference complete records in discovered segments.
    for (auto &[hash, e] : index_)
    {
      auto it = std::find_if(segments_.begin(), segments_.end(),
                             [&](auto &s) { return s.id == e.segment_id; });
      if (it == segments_.end() || e.offset < sizeof(PackedDataHeader) ||
          !e.compressed_size || !e.original_size || e.offset > it->data_size ||
          e.compressed_size > it->data_size - e.offset)
        fail("index points outside segment");
      ++it->entry_count;
      indexed_bytes[e.segment_id] +=
          sizeof(PackedDataHeader) + e.compressed_size;
    }
    for (const auto &segment : segments_)
      if (indexed_bytes[segment.id] != segment.data_size)
        fail("segment contains unindexed or overlapping records; explicit "
             "recovery required");
    if (!c.read_only)
    {
      ensure_active_segment();
      journal_fd_ = open(get_incremental_index_path().c_str(),
                         O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
      if (journal_fd_ < 0)
        fail("open journal");
      if (!file_size(journal_fd_))
        write_all(journal_fd_, &INCREMENTAL_MAGIC, sizeof(INCREMENTAL_MAGIC));
      // Make newly created store directories discoverable after a barrier.
      auto parent = std::filesystem::absolute(c.storage_path).parent_path();
      for (; !parent.empty(); parent = parent.parent_path())
      {
        sync_dir(parent.string());
        if (parent == parent.root_path())
          break;
      }
    }
    initialized_ = true;
  }
  catch (...)
  {
    if (active_segment_fd_ >= 0)
      close(active_segment_fd_);
    active_segment_fd_ = -1;
    if (journal_fd_ >= 0)
      close(journal_fd_);
    journal_fd_ = -1;
    if (lock_fd_ >= 0)
      close(lock_fd_);
    lock_fd_ = -1;
    index_.clear();
    segments_.clear();
    throw;
  }
}
void StateStorePacked::load_segments_info()
{
  for (auto &item :
       std::filesystem::directory_iterator(config_.storage_path + "/segments"))
  {
    if (item.path().extension() != ".dat")
      continue;
    std::string stem = item.path().stem().string();
    if (stem.empty() ||
        stem.find_first_not_of("0123456789") != std::string::npos)
      fail("invalid segment name");
    uint64_t id = std::stoull(stem);
    if (id > UINT32_MAX || std::to_string(id) != stem)
      fail("invalid segment id");
    segments_.push_back({static_cast<uint32_t>(id), item.file_size(), 0});
  }
  std::sort(segments_.begin(), segments_.end(),
            [](auto &a, auto &b) { return a.id < b.id; });
  active_segment_id_ = segments_.empty() ? 0 : segments_.back().id;
}
void StateStorePacked::ensure_active_segment()
{
  if (active_segment_fd_ >= 0)
    return;
  active_segment_fd_ = open(get_segment_data_path(active_segment_id_).c_str(),
                            O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (active_segment_fd_ < 0)
    fail("open active segment");
  if (segments_.empty() || segments_.back().id != active_segment_id_)
    segments_.push_back({active_segment_id_, file_size(active_segment_fd_), 0});
}
void StateStorePacked::rotate_segment()
{
  sync_fd(active_segment_fd_);
  close(active_segment_fd_);
  active_segment_fd_ = -1;
  if (active_segment_id_ == UINT32_MAX)
    fail("segment id exhausted");
  ++active_segment_id_;
  ensure_active_segment();
}
void StateStorePacked::load_index()
{
  int raw = open(get_global_index_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (raw < 0 && errno == ENOENT)
    return;
  FD fd(raw);
  uint32_t magic, version;
  uint64_t count;
  auto size = file_size(fd.value);
  if (size < 16)
    fail("truncated global index");
  read_all(fd.value, &magic, 4, 0);
  read_all(fd.value, &version, 4, 4);
  read_all(fd.value, &count, 8, 8);
  if (magic != INDEX_MAGIC || version != 1 || count != (size - 16) / 32 ||
      (size - 16) % 32)
    fail("invalid global index");
  for (uint64_t i = 0; i < count; ++i)
  {
    hash_type h;
    PackedIndexEntry e;
    read_all(fd.value, &h, 8, 16 + i * 32);
    read_all(fd.value, &e, 24, 24 + i * 32);
    if (!h || !index_.emplace(h, e).second)
      fail("duplicate index key");
  }
}
void StateStorePacked::load_incremental_index()
{
  int raw = open(get_incremental_index_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (raw < 0 && errno == ENOENT)
    return;
  FD fd(raw);
  auto size = file_size(fd.value);
  if (!size)
    return;
  uint32_t magic;
  read_all(fd.value, &magic, 4, 0);
  if (magic != INCREMENTAL_MAGIC || (size - 4) % 32)
    fail("invalid or truncated journal");
  for (uint64_t off = 4; off < size; off += 32)
  {
    hash_type h;
    PackedIndexEntry e;
    read_all(fd.value, &h, 8, off);
    read_all(fd.value, &e, 24, off + 8);
    if (!h)
      fail("zero journal key");
    auto it = index_.find(h);
    if (it != index_.end() && memcmp(&it->second, &e, sizeof(e)))
      fail("conflicting journal key");
    index_[h] = e;
  }
}
void StateStorePacked::append_to_incremental_index(hash_type h,
                                                   const PackedIndexEntry &e)
{
  uint8_t record[32];
  memcpy(record, &h, 8);
  memcpy(record + 8, &e, 24);
  write_all(journal_fd_, record, sizeof(record));
}
void StateStorePacked::flush_index()
{
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (failed_)
    fail("store failed; reopen required");
  if (!initialized_ || config_.read_only)
    return;
  try
  {
    sync_fd(active_segment_fd_);
    sync_fd(journal_fd_);
    sync_dir(config_.storage_path + "/segments");
    sync_dir(config_.storage_path);
  }
  catch (...)
  {
    failed_ = true;
    throw;
  }
}
void StateStorePacked::flush_index_unlocked()
{
  std::string temp = get_global_index_path() + ".tmp";
  FD fd(open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  uint32_t version = 1;
  uint64_t count = index_.size();
  write_all(fd.value, &INDEX_MAGIC, 4);
  write_all(fd.value, &version, 4);
  write_all(fd.value, &count, 8);
  // Batch index entries rather than issuing six writes for each entry.
  std::vector<uint8_t> batch;
  batch.reserve(65536);
  for (auto &[h, e] : index_)
  {
    size_t pos = batch.size();
    batch.resize(pos + 32);
    memcpy(batch.data() + pos, &h, 8);
    memcpy(batch.data() + pos + 8, &e, 24);
    if (batch.size() == 65536)
    {
      write_all(fd.value, batch.data(), batch.size());
      batch.clear();
    }
  }
  if (!batch.empty())
    write_all(fd.value, batch.data(), batch.size());
  sync_fd(fd.value);
  if (rename(temp.c_str(), get_global_index_path().c_str()) < 0)
    fail("publish index");
  sync_dir(config_.storage_path);
  // Retain the append journal. Truncating it in place creates a crash window
  // with a malformed journal; it is safe and cheap to replay matching entries.
}
void StateStorePacked::save_segments_info() { flush_index(); }
void StateStorePacked::shutdown()
{
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (!initialized_)
    return;
  std::exception_ptr error;
  try
  {
    flush_index();
    if (!config_.read_only)
      flush_index_unlocked();
  }
  catch (...)
  {
    error = std::current_exception();
  }
  if (active_segment_fd_ >= 0)
    close(active_segment_fd_);
  active_segment_fd_ = -1;
  if (journal_fd_ >= 0)
    close(journal_fd_);
  journal_fd_ = -1;
  if (lock_fd_ >= 0)
    close(lock_fd_);
  lock_fd_ = -1;
  initialized_ = false;
  index_.clear();
  segments_.clear();
  if (error)
    std::rethrow_exception(error);
}
std::vector<uint8_t>
StateStorePacked::compress(const std::vector<uint8_t> &data)
{
  if (data.empty())
    return {};
  if (!context.c)
    fail("allocate compression context");
  const size_t capacity = ZSTD_compressBound(data.size());
  // Only the successful n-byte output is published; spare capacity need not be initialized.
  auto result = std::make_unique_for_overwrite<uint8_t[]>(capacity);
  size_t n =
      ZSTD_compressCCtx(context.c, result.get(), capacity, data.data(),
                        data.size(), compression_level_.load());
  if (ZSTD_isError(n))
    fail("compression failed");
  // Cache budgets count encoded bytes, not the bound-sized workspace.
  return std::vector<uint8_t>(result.get(), result.get() + n);
}
std::vector<uint8_t>
StateStorePacked::decompress(const std::vector<uint8_t> &data, size_t size)
{
  if (!size || size > UINT32_MAX || !context.d)
    fail("invalid decompression size/context");
  auto frame = ZSTD_getFrameContentSize(data.data(), data.size());
  if (frame != size ||
      ZSTD_findFrameCompressedSize(data.data(), data.size()) != data.size())
    fail("frame size mismatch");
  std::vector<uint8_t> result(size);
  size_t n = ZSTD_decompressDCtx(context.d, result.data(), size, data.data(),
                                 data.size());
  if (ZSTD_isError(n) || n != size)
    fail("decompression failed/size mismatch");
  return result;
}
uint32_t StateStorePacked::compute_checksum(const std::vector<uint8_t> &data)
{
  uint32_t sum = 0;
  for (auto b : data)
    sum = sum * 31 + b;
  return sum;
}
hash_type StateStorePacked::save(const void *data, size_t len, hash_type hash,
                                 bool encoded, size_t original_size)
{
  if (!data || !len || !hash || len > UINT32_MAX ||
      (encoded && (!original_size || original_size > UINT32_MAX)))
    return 0;
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (!initialized_ || failed_ || config_.read_only)
    fail("store unavailable for writing");
  std::vector<uint8_t> input(static_cast<const uint8_t *>(data),
                             static_cast<const uint8_t *>(data) + len);
  std::vector<uint8_t> raw =
      encoded ? decompress(input, original_size) : std::move(input);
  auto it = index_.find(hash);
  if (it != index_.end())
  {
    std::vector<uint8_t> previous;
    load(hash, previous);
    if (previous != raw)
      fail("hash collision");
    return hash;
  }
  auto compressed = encoded ? std::move(input) : compress(raw);
  if (compressed.size() > UINT32_MAX)
    fail("compressed size overflow");
  try
  {
    if (segments_.back().entry_count &&
        (segments_.back().data_size + sizeof(PackedDataHeader) +
                 compressed.size() >
             config_.max_segment_size ||
         segments_.back().entry_count >= config_.max_entries_per_segment))
      rotate_segment();
    auto offset = file_size(active_segment_fd_);
    PackedDataHeader h{
        DATA_MAGIC, hash, static_cast<uint32_t>(compressed.size()),
        static_cast<uint32_t>(raw.size()), compute_checksum(compressed)};
    write_all(active_segment_fd_, &h, sizeof(h));
    write_all(active_segment_fd_, compressed.data(), compressed.size());
    PackedIndexEntry e{active_segment_id_, offset + sizeof(h),
                       h.compressed_size, h.original_size, h.checksum};
    append_to_incremental_index(hash, e);
    index_.emplace(hash, e);
    segments_.back().data_size = offset + sizeof(h) + compressed.size();
    ++segments_.back().entry_count;
  }
  catch (...)
  {
    failed_ = true;
    throw;
  }
  return hash;
}
ssize_t StateStorePacked::load_compressed(hash_type hash,
                                          std::vector<uint8_t> &data)
{
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  data.clear();
  if (!initialized_ || failed_)
    fail("store unavailable");
  auto it = index_.find(hash);
  if (it == index_.end())
    return -1;
  const auto &e = it->second;
  FD fd(
      open(get_segment_data_path(e.segment_id).c_str(), O_RDONLY | O_CLOEXEC));
  auto size = file_size(fd.value);
  if (e.offset < sizeof(PackedDataHeader) || e.offset > size ||
      e.compressed_size > size - e.offset)
    fail("record outside file");
  PackedDataHeader h;
  read_all(fd.value, &h, sizeof(h), e.offset - sizeof(h));
  if (h.magic != DATA_MAGIC || h.hash != hash ||
      h.compressed_size != e.compressed_size ||
      h.original_size != e.original_size || h.checksum != e.checksum)
    fail("header/index mismatch");
  std::vector<uint8_t> tmp(e.compressed_size);
  read_all(fd.value, tmp.data(), tmp.size(), e.offset);
  if (compute_checksum(tmp) != e.checksum)
    fail("checksum mismatch");
  if (ZSTD_getFrameContentSize(tmp.data(), tmp.size()) != e.original_size ||
      ZSTD_findFrameCompressedSize(tmp.data(), tmp.size()) != tmp.size())
    fail("invalid frame");
  data = std::move(tmp);
  return data.size();
}
ssize_t StateStorePacked::load(hash_type h, std::vector<uint8_t> &data)
{
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  data.clear();
  std::vector<uint8_t> compressed;
  if (load_compressed(h, compressed) < 0)
    return -1;
  data = decompress(compressed, index_.at(h).original_size);
  return data.size();
}
bool StateStorePacked::exists(hash_type h)
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  if (failed_)
    fail("store failed");
  return index_.count(h);
}
size_t StateStorePacked::get_original_size(hash_type h)
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  auto it = index_.find(h);
  return it == index_.end() ? 0 : it->second.original_size;
}
bool StateStorePacked::wait_persisted(hash_type h, uint64_t)
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  if (!exists(h))
    return false;
  flush_index();
  return true;
}
void StateStorePacked::rebuild_index()
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  std::unordered_map<hash_type, PackedIndexEntry> rebuilt;
  for (auto &s : segments_)
  {
    FD fd(open(get_segment_data_path(s.id).c_str(), O_RDONLY | O_CLOEXEC));
    uint64_t offset = 0;
    s.entry_count = 0;
    while (offset < s.data_size)
    {
      PackedDataHeader h;
      read_all(fd.value, &h, sizeof(h), offset);
      offset += sizeof(h);
      if (h.magic != DATA_MAGIC || !h.hash || !h.compressed_size ||
          !h.original_size || offset > s.data_size ||
          h.compressed_size > s.data_size - offset)
        fail("invalid segment during rebuild");
      std::vector<uint8_t> bytes(h.compressed_size);
      read_all(fd.value, bytes.data(), bytes.size(), offset);
      if (compute_checksum(bytes) != h.checksum)
        fail("corrupt record during rebuild");
      decompress(bytes, h.original_size);
      PackedIndexEntry e{s.id, offset, h.compressed_size, h.original_size,
                         h.checksum};
      if (!rebuilt.emplace(static_cast<hash_type>(h.hash), e).second)
        fail("duplicate segment key");
      offset += h.compressed_size;
      ++s.entry_count;
    }
  }
  index_ = std::move(rebuilt);
}
void StateStorePacked::compact()
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  flush_index(); /* Immutable append-only records have no deletions or holes. */
}
size_t StateStorePacked::get_entry_count() const
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  return index_.size();
}
size_t StateStorePacked::get_segment_count() const
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  return segments_.size();
}
size_t StateStorePacked::get_total_data_size() const
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  size_t n = 0;
  for (auto &s : segments_)
    n += s.data_size;
  return n;
}
bool StateStorePacked::verify_and_repair()
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  for (auto &[h, e] : index_)
  {
    std::vector<uint8_t> d;
    load(h, d);
  }
  return true;
}
void StateStorePacked::dump_debug_info() {}
std::vector<hash_type>
StateStorePacked::find_by_prefix(const std::string &prefix)
{
  std::lock_guard<std::recursive_mutex> g(mutex_);
  std::vector<hash_type> result;
  if (prefix.empty() || prefix.size() > 16 ||
      prefix.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return result;
  auto value = std::stoull(prefix, nullptr, 16);
  unsigned shift = 64 - prefix.size() * 4;
  for (auto &[h, e] : index_)
    if ((h >> shift) == value)
      result.push_back(h);
  return result;
}
extern "C"
{
  void state_store_packed_init() { StateStorePacked::instance().init(); }
  uint64_t state_store_packed_save(const void *d, size_t n)
  {
    return StateStorePacked::instance().save(d, n, XXHash64::hash(d, n));
  }
  ssize_t state_store_packed_load(uint64_t h, void *d, size_t n)
  {
    std::vector<uint8_t> b;
    auto r = StateStorePacked::instance().load(h, b);
    if (r < 0 || b.size() > n || (!d && !b.empty()))
      return -1;
    memcpy(d, b.data(), b.size());
    return r;
  }
  int state_store_packed_exists(uint64_t h)
  {
    return StateStorePacked::instance().exists(h);
  }
}
