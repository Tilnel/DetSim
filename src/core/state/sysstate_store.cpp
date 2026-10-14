#include "sysstate_store.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr uint32_t ENTRY_MAGIC = 0x53534545;
struct __attribute__((packed)) DataEntryHeader {
  uint32_t magic;
  hash_type hash;
  uint32_t size;
  uint32_t checksum;
};
static_assert(sizeof(DataEntryHeader) == 20);

[[noreturn]] void io_error(const char *operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
void read_exact(int fd, void *buffer, size_t size, uint64_t offset) {
  auto *out = static_cast<uint8_t *>(buffer);
  while (size) {
    ssize_t n = pread(fd, out, size, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) io_error("metadata pread");
    if (!n) throw std::runtime_error("truncated metadata record");
    out += n; size -= n; offset += n;
  }
}
void write_exact(int fd, const void *buffer, size_t size, uint64_t offset) {
  auto *in = static_cast<const uint8_t *>(buffer);
  while (size) {
    ssize_t n = pwrite(fd, in, size, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) io_error("metadata pwrite");
    if (!n) throw std::runtime_error("zero-length metadata write");
    in += n; size -= n; offset += n;
  }
}
void sync_file(int fd, bool directory = false) {
  int rc;
  do { rc = directory ? fsync(fd) : fdatasync(fd); } while (rc < 0 && errno == EINTR);
  if (rc < 0) io_error("metadata sync");
}
}

SysStateStore &SysStateStore::instance() {
  static SysStateStore store;
  store.init(); // The initialization flag is always protected by mutex_.
  return store;
}
SysStateStore::~SysStateStore() {
  // Explicit flush/shutdown reports errors; a destructor must never terminate.
  try { shutdown(); } catch (...) {}
}
void SysStateStore::init() {
  std::lock_guard<std::mutex> lock(mutex_);
  init_unlocked();
}
void SysStateStore::init_unlocked() {
  if (initialized_) { check_ready(); return; }
  bool made_directory = mkdir("sstate", 0755) == 0;
  if (!made_directory && errno != EEXIST) io_error("metadata mkdir");
  directory_fd_ = open("sstate", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd_ < 0) io_error("metadata open directory");
  try {
    if (made_directory) {
      int parent = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (parent < 0) io_error("metadata open parent directory");
      try { sync_file(parent, true); } catch (...) { close(parent); throw; }
      close(parent);
    }
    fd_ = open(DATA_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) io_error("metadata open data");
    if (flock(fd_, LOCK_EX | LOCK_NB) < 0) io_error("metadata store already owned");
    scan_unlocked();
    failed_ = false;
    initialized_ = true;
  } catch (...) {
    if (fd_ >= 0) close(fd_);
    close(directory_fd_);
    fd_ = directory_fd_ = -1;
    index_.clear(); clear_cache(); data_file_size_ = 0;
    throw;
  }
}
void SysStateStore::check_ready() const {
  if (!initialized_) throw std::runtime_error("metadata store is shut down");
  if (failed_) throw std::runtime_error("metadata store has a previous I/O failure");
}
uint32_t SysStateStore::compute_checksum(const std::vector<uint8_t> &data) {
  uint32_t sum = 0;
  for (uint8_t byte : data) sum = sum * 31 + byte;
  return sum;
}
std::vector<uint8_t> SysStateStore::read_entry(hash_type hash, const SysStateEntry &entry) const {
  if (entry.offset < sizeof(DataEntryHeader) || entry.offset > data_file_size_ ||
      entry.size > data_file_size_ - entry.offset)
    throw std::runtime_error("metadata record out of bounds");
  DataEntryHeader header;
  read_exact(fd_, &header, sizeof(header), entry.offset - sizeof(header));
  if (header.magic != ENTRY_MAGIC || header.hash != hash ||
      header.size != entry.size || header.checksum != entry.checksum)
    throw std::runtime_error("metadata record header mismatch");
  std::vector<uint8_t> data(entry.size);
  read_exact(fd_, data.data(), data.size(), entry.offset);
  if (compute_checksum(data) != entry.checksum)
    throw std::runtime_error("metadata checksum mismatch");
  return data;
}
void SysStateStore::scan_unlocked() {
  struct stat st;
  if (fstat(fd_, &st) < 0) io_error("metadata fstat");
  if (!S_ISREG(st.st_mode) || st.st_size < 0)
    throw std::runtime_error("metadata data file is not regular");
  const uint64_t size = st.st_size;
  decltype(index_) rebuilt;
  uint64_t offset = 0;
  // Validate the entire append log before publishing its index; a partial tail
  // is an explicit failure, never a silent truncation or partial recovery.
  while (offset < size) {
    if (size - offset < sizeof(DataEntryHeader)) throw std::runtime_error("truncated metadata header");
    DataEntryHeader header;
    read_exact(fd_, &header, sizeof(header), offset);
    offset += sizeof(header);
    if (header.magic != ENTRY_MAGIC || header.size > size - offset)
      throw std::runtime_error("invalid metadata record");
    std::vector<uint8_t> data(header.size);
    read_exact(fd_, data.data(), data.size(), offset);
    if (compute_checksum(data) != header.checksum) throw std::runtime_error("metadata checksum mismatch");
    SysStateEntry entry{offset, header.size, header.checksum};
    auto [it, inserted] = rebuilt.emplace(hash_type(header.hash), entry);
    if (!inserted) {
      std::vector<uint8_t> previous(it->second.size);
      read_exact(fd_, previous.data(), previous.size(), it->second.offset);
      if (previous != data) throw std::runtime_error("conflicting metadata records share a hash");
    }
    offset += header.size;
  }
  clear_cache();
  index_.swap(rebuilt);
  data_file_size_ = size;
}
bool SysStateStore::save(hash_type hash, const std::vector<uint8_t> &data) {
  std::lock_guard<std::mutex> lock(mutex_);
  check_ready();
  auto found = index_.find(hash);
  if (found != index_.end()) {
    const auto *cached = cached_data(hash);
    if (cached) {
      if (*cached != data) throw std::runtime_error("conflicting metadata records share a hash");
    } else if (read_entry(hash, found->second) != data) {
      throw std::runtime_error("conflicting metadata records share a hash");
    }
    return true;
  }
  if (data.size() > std::numeric_limits<uint32_t>::max() ||
      data_file_size_ > uint64_t(std::numeric_limits<off_t>::max()) - sizeof(DataEntryHeader) - data.size())
    throw std::runtime_error("metadata record too large");
  DataEntryHeader header{ENTRY_MAGIC, hash, static_cast<uint32_t>(data.size()), compute_checksum(data)};
  std::vector<uint8_t> record(sizeof(header) + data.size());
  memcpy(record.data(), &header, sizeof(header));
  if (!data.empty()) memcpy(record.data() + sizeof(header), data.data(), data.size());
  try {
    write_exact(fd_, record.data(), record.size(), data_file_size_);
    index_.emplace(hash, SysStateEntry{data_file_size_ + sizeof(header), header.size, header.checksum});
    data_file_size_ += record.size();
    cache_data(hash, data);
  } catch (...) {
    // Even an allocation failure after the write needs reopen/revalidation.
    failed_ = true;
    throw;
  }
  return true;
}
bool SysStateStore::load(hash_type hash, std::vector<uint8_t> &data) {
  std::lock_guard<std::mutex> lock(mutex_);
  check_ready();
  auto found = index_.find(hash);
  if (found == index_.end()) return false;
  if (const auto *cached = cached_data(hash)) { data = *cached; return true; }
  auto result = read_entry(hash, found->second);
  cache_data(hash, result);
  data.swap(result);
  return true;
}
bool SysStateStore::exists(hash_type hash) {
  std::lock_guard<std::mutex> lock(mutex_);
  check_ready();
  return index_.count(hash) != 0;
}
std::vector<hash_type> SysStateStore::find_by_prefix(const std::string &prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  check_ready();
  if (prefix.empty() || prefix.size() > 16) return {};
  uint64_t value = 0;
  for (char c : prefix) {
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return {};
    value = (value << 4) | digit;
  }
  const unsigned shift = (16 - prefix.size()) * 4;
  const uint64_t mask = UINT64_MAX << shift;
  value <<= shift;
  std::vector<hash_type> matches;
  for (const auto &[hash, entry] : index_) if ((hash & mask) == value) matches.push_back(hash);
  std::sort(matches.begin(), matches.end());
  return matches;
}
void SysStateStore::flush_unlocked() {
  check_ready();
  try { sync_file(fd_); sync_file(directory_fd_, true); }
  catch (...) { failed_ = true; throw; }
}
void SysStateStore::flush_index() {
  std::lock_guard<std::mutex> lock(mutex_);
  flush_unlocked();
}
size_t SysStateStore::get_entry_count() const {
  std::lock_guard<std::mutex> lock(mutex_); return index_.size();
}
size_t SysStateStore::get_data_file_size() const {
  std::lock_guard<std::mutex> lock(mutex_); return data_file_size_;
}
void SysStateStore::rebuild_index() {
  std::lock_guard<std::mutex> lock(mutex_);
  check_ready();
  try { scan_unlocked(); } catch (...) { failed_ = true; throw; }
}
void SysStateStore::compact() { rebuild_index(); }
void SysStateStore::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) return;
  std::exception_ptr error;
  try { flush_unlocked(); } catch (...) { error = std::current_exception(); }
  if (close(fd_) < 0 && !error)
    error = std::make_exception_ptr(std::system_error(errno, std::generic_category(), "metadata close"));
  close(directory_fd_);
  fd_ = directory_fd_ = -1;
  initialized_ = false;
  index_.clear(); clear_cache(); data_file_size_ = 0;
  if (error) std::rethrow_exception(error);
}

void SysStateStore::clear_cache() {
  cache_.clear(); cache_lru_.clear(); cache_bytes_ = 0;
}
const std::vector<uint8_t> *SysStateStore::cached_data(hash_type hash) {
  auto found = cache_.find(hash);
  if (found == cache_.end()) return nullptr;
  cache_lru_.splice(cache_lru_.begin(), cache_lru_, found->second.position);
  return &found->second.data;
}
void SysStateStore::cache_data(hash_type hash, const std::vector<uint8_t> &data) {
  if (data.size() > CACHE_MAX_BYTES || cache_.count(hash)) return;
  while (cache_.size() >= CACHE_MAX_ENTRIES || cache_bytes_ + data.size() > CACHE_MAX_BYTES) {
    auto victim = cache_.find(cache_lru_.back());
    cache_bytes_ -= victim->second.data.size();
    cache_.erase(victim); cache_lru_.pop_back();
  }
  cache_lru_.push_front(hash);
  try { cache_.emplace(hash, CachedData{data, cache_lru_.begin()}); }
  catch (...) { cache_lru_.pop_front(); throw; }
  cache_bytes_ += data.size();
}

void SysStateStore::inspect(const std::string &directory,
    const std::function<void(hash_type, const SysStateEntry &, const std::vector<uint8_t> &)> &visitor) {
  SysStateStore reader;
  reader.fd_ = open((directory + "/packed.dat").c_str(), O_RDONLY | O_CLOEXEC);
  if (reader.fd_ < 0) io_error("metadata inspect open");
  try {
    if (flock(reader.fd_, LOCK_SH | LOCK_NB) < 0) io_error("metadata inspect store in use");
    reader.scan_unlocked();
    std::vector<hash_type> hashes;
    hashes.reserve(reader.index_.size());
    for (const auto &[hash, entry] : reader.index_) hashes.push_back(hash);
    std::sort(hashes.begin(), hashes.end());
    for (hash_type hash : hashes) {
      const auto &entry = reader.index_.at(hash);
      visitor(hash, entry, reader.read_entry(hash, entry));
    }
  } catch (...) { close(reader.fd_); reader.fd_ = -1; throw; }
  close(reader.fd_); reader.fd_ = -1;
}
