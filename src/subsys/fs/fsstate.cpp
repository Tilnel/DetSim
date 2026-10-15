#include "fsstate.h"
#include "cereal/archives/binary.hpp"
#include "cereal/cereal.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/deque.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/utility.hpp"
#include "cereal/types/vector.hpp"
#include "debug.h"
#include "monitor.h"
#include "guest.h"
#include "io.h"
#include "state_store.h"
#include "utils.h"
#include <mutex>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <iostream>
#include <limits>
#include <climits>
#include <libgen.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

static timespec modeled_fs_time()
{
  const int node = ptmc_state.cursor;
  return node >= 0 && node < NP
      ? timespec{ptmc_state.time[node].tv_sec, ptmc_state.time[node].tv_usec * 1000}
      : timespec{};
}

namespace {
thread_local bool external_file_content = false;
constexpr size_t external_file_content_threshold = 4096;
}

FileContentSnapshotScope::FileContentSnapshotScope() : previous_(external_file_content) {
  external_file_content = true;
}
FileContentSnapshotScope::~FileContentSnapshotScope() { external_file_content = previous_; }

struct FileContent::Payload {
  std::vector<uint8_t> owned;
  std::shared_ptr<const std::vector<uint8_t>> restored;
  std::mutex registration_mutex;
  hash_type key = 0;
  uint64_t generation = 0;
};
const std::vector<uint8_t>& FileContent::bytes() const {
  static const std::vector<uint8_t> empty;
  return !payload_ ? empty : payload_->restored ? *payload_->restored : payload_->owned;
}
void FileContent::replace(std::vector<uint8_t> bytes) {
  auto next = std::make_shared<Payload>();
  next->owned = std::move(bytes);
  payload_ = std::move(next);
}
std::vector<uint8_t>& FileContent::writable() {
  if (!payload_ || !payload_.unique() || payload_->restored)
    replace(bytes());
  payload_->key = 0;
  payload_->generation = 0;
  return payload_->owned;
}
void FileContent::write(size_t offset, const void* source, size_t count) {
  if (!count) return;
  if (count > std::numeric_limits<size_t>::max() - offset)
    throw std::length_error("Virtual file write extent overflow");
  if (!source) throw std::invalid_argument("Null virtual file write source");
  // Keep an aliased read view alive if the source came from this payload.
  const auto* start = static_cast<const char*>(source);
  const bool aliases = std::less_equal<const char*>{}(data(), start) &&
      std::less<const char*>{}(start, end());
  auto retained = aliases ? payload_ : nullptr;
  auto& destination = writable();
  if (offset + count > destination.size()) destination.resize(offset + count);
  memcpy(destination.data() + offset, source, count);
}
template<class Archive> void FileContent::save(Archive& ar) const {
  const uint8_t tag = external_file_content && size() >= external_file_content_threshold ? 1 : 0;
  ar(tag);
  if (!tag) { ar(bytes()); return; }
  auto& store = StateStore::instance();
  std::lock_guard lock(payload_->registration_mutex);
  // Readiness must be checked even when an immutable registration is cached.
  const uint64_t generation = store.snapshot_generation();
  if (payload_->generation != generation || !payload_->key) {
    const hash_type key = store.save(data(), size());
    if (store.snapshot_generation() != generation)
      throw std::runtime_error("Store changed while saving virtual file content");
    payload_->key = key;
    payload_->generation = generation;
  }
  const uint64_t length = size();
  ar(length, payload_->key);
}
template<class Archive> void FileContent::load(Archive& ar) {
  uint8_t tag;
  ar(tag);
  if (!tag) {
    std::vector<uint8_t> value;
    ar(value);
    replace(std::move(value));
    return;
  }
  if (tag != 1) throw std::runtime_error("Invalid virtual file content tag");
  uint64_t length;
  hash_type key;
  ar(length, key);
  if (length < external_file_content_threshold || !key)
    throw std::runtime_error("Invalid virtual file content reference");
  auto& store = StateStore::instance();
  const uint64_t generation = store.snapshot_generation();
  auto value = store.load_shared(key);
  if (!value || value->size() != length)
    throw std::runtime_error("Missing or invalid virtual file content");
  if (store.snapshot_generation() != generation)
    throw std::runtime_error("Store changed while loading virtual file content");
  auto next = std::make_shared<Payload>();
  next->restored = std::move(value);
  next->key = key;
  next->generation = generation;
  payload_ = std::move(next);
}
template void FileContent::save<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive&) const;
template void FileContent::load<cereal::BinaryInputArchive>(cereal::BinaryInputArchive&);

/* --- Serialization Implementations --- */

template <class Archive>
void VFSNode::serialize(Archive &ar)
{
#ifdef FSSTATE_DETAILED_METADATA
  ar(CEREAL_NVP(content), CEREAL_NVP(metadata.st_dev),
     CEREAL_NVP(metadata.st_ino), CEREAL_NVP(metadata.st_mode),
     CEREAL_NVP(metadata.st_nlink), CEREAL_NVP(metadata.st_uid),
     CEREAL_NVP(metadata.st_gid), CEREAL_NVP(metadata.st_rdev),
     CEREAL_NVP(metadata.st_size), CEREAL_NVP(metadata.st_blksize),
     CEREAL_NVP(metadata.st_blocks), CEREAL_NVP(metadata.st_atime),
     CEREAL_NVP(metadata.st_mtim.tv_sec), CEREAL_NVP(metadata.st_mtim.tv_nsec),
     CEREAL_NVP(metadata.st_ctime));
#else
  ar(CEREAL_NVP(content), CEREAL_NVP(metadata.st_size),
     CEREAL_NVP(metadata.st_mode), CEREAL_NVP(metadata.st_mtim.tv_sec),
     CEREAL_NVP(metadata.st_mtim.tv_nsec));
#endif
  if (metadata.st_mtim.tv_nsec < 0 || metadata.st_mtim.tv_nsec >= 1000000000)
    throw std::runtime_error("Invalid virtual file modification nanoseconds");
}

template <class Archive>
void OpenFileDescription::serialize(Archive &ar)
{
  ar(CEREAL_NVP(path), CEREAL_NVP(offset));
}

template <class Archive>
void FileSystemState::serialize(Archive &ar)
{
  /* Note: fd_manager_ is shared and not serialized per instance */
  ar(filesystem, open_files, cwd, mappings, pipes_, pipe_fds_, creation_mask_, virtual_file_mappings);
  uint64_t previous_end = 0;
  bool first = true;
  for (const auto& [start, end] : virtual_file_mappings) {
    if (start >= end || start % 4096 || end % 4096 || (!first && start <= previous_end))
      throw std::runtime_error("Invalid virtual file mapping provenance");
    first = false;
    previous_end = end;
  }
  if (creation_mask_ & ~0777)
    throw std::runtime_error("Invalid virtual filesystem creation mask");
}

/* --- FileSystemState Method Implementations --- */

FileSystemState::FileSystemState() : cwd(""), fd_manager_()
{
  ensure_dir_exists("/");
  // Embedded NUL keys cannot be named by a guest pathname.
  for (int fd = 0; fd < 3; ++fd)
  {
    std::string path(1, '\0');
    path += "stdio" + std::to_string(fd);
    VFSNode node{};
    node.metadata.st_mode = S_IFCHR | 0600;
    filesystem.emplace(path, std::move(node));
    open_files.emplace(fd, OpenFileDescription{path, 0});
  }
}

FileSystemState::FileSystemState(FdManagerPtr fd_mgr) : FileSystemState()
{
  set_fd_manager(std::move(fd_mgr));
}

bool FileSystemState::overlaps_virtual_file_mapping(uint64_t start, uint64_t end) const
{
  if (start >= end) return false;
  auto next = virtual_file_mappings.lower_bound(start);
  if (next != virtual_file_mappings.end() && next->first < end) return true;
  return next != virtual_file_mappings.begin() && std::prev(next)->second > start;
}

void FileSystemState::remove_virtual_file_mapping(uint64_t start, uint64_t end)
{
  if (start >= end) return;
  if (start % 4096 || end % 4096)
    throw std::runtime_error("Unaligned virtual file mapping removal");
  auto next = virtual_file_mappings.lower_bound(start);
  if (next != virtual_file_mappings.begin()) --next;
  while (next != virtual_file_mappings.end() && next->first < end) {
    const auto [left, right] = *next;
    if (right <= start) { ++next; continue; }
    next = virtual_file_mappings.erase(next);
    if (left < start) virtual_file_mappings.emplace(left, start);
    if (right > end) {
      virtual_file_mappings.emplace(end, right);
      break;
    }
  }
}

void FileSystemState::add_virtual_file_mapping(uint64_t start, uint64_t end)
{
  if (start >= end || start % 4096 || end % 4096)
    throw std::runtime_error("Invalid virtual file mapping extent");
  auto next = virtual_file_mappings.lower_bound(start);
  if (next != virtual_file_mappings.begin() && std::prev(next)->second >= start) --next;
  while (next != virtual_file_mappings.end() && next->first <= end) {
    start = std::min(start, next->first);
    end = std::max(end, next->second);
    next = virtual_file_mappings.erase(next);
  }
  virtual_file_mappings.emplace(start, end);
}

int FileSystemState::allocate_fd(FdType type, int status_flags, int descriptor_flags)
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(type, status_flags, descriptor_flags);
}

void FileSystemState::release_fd(int fd)
{
  if (fd_manager_)
  {
    fd_manager_->release_fd(fd);
  }
}

static std::string join_paths(const std::string &base, const std::string &rel)
{
  if (rel.empty())
    return base;
  if (rel[0] == '/')
    return rel;
  if (base.empty() || base == "/")
    return std::string("/") + rel;
  std::string out = base;
  if (out.back() != '/')
    out += '/';
  out += rel;
  return out;
}

void FileSystemState::ensure_dir_exists(const std::string &path)
{
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0)
    ensure_dir_exists(path.substr(0, slash));
  else if (path != "/")
    ensure_dir_exists("/");
  // Ordered overlays can leave an existing directory below a replaced parent.
  // Validate the whole chain before trusting or creating this directory.
  if (const auto existing = filesystem.find(path); existing != filesystem.end()) {
    if (!S_ISDIR(existing->second.metadata.st_mode))
      throw std::runtime_error("VFS ancestor is not a directory: " + path);
    return;
  }

  VFSNode dir{};
  dir.metadata.st_mode = S_IFDIR | 0755;
  dir.metadata.st_size = 0;
  filesystem[path] = std::move(dir);
}

void FileSystemState::init_from_mappings(
    const std::vector<std::pair<std::string, std::string>> &maps)
{
  mappings = maps;

  ensure_dir_exists("/");

  // Capture objects in mapping order. Never follow a captured final symlink or
  // turn an unreadable/unsupported object into a successful empty file.
  for (const auto& [host_base, target_base] : mappings) {
    if (target_base.empty() || target_base.front() != '/')
      throw std::runtime_error("VFS mapping target must be absolute: " + target_base);
    std::vector<std::pair<std::string, std::string>> pending{
        {host_base, resolve_path(AT_FDCWD, target_base)}};
    while (!pending.empty()) {
      auto [host, target] = std::move(pending.back());
      pending.pop_back();
      const auto fail = [&host](const char* operation) {
        throw std::runtime_error(std::string("Cannot capture VFS ") + operation + ": " + host);
      };
      struct stat st{};
      if (lstat(host.c_str(), &st) < 0) fail("metadata");
      if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
        fail("unsupported object");
      if (target == "/" && !S_ISDIR(st.st_mode)) fail("non-directory root");
      if (target != "/") {
        const size_t slash = target.find_last_of('/');
        ensure_dir_exists(slash == 0 ? "/" : target.substr(0, slash));
      }
      VFSNode node{};
#ifdef FSSTATE_DETAILED_METADATA
      if (S_ISREG(st.st_mode)) {
        node.metadata.st_nlink = 1;
      }
#endif
      node.metadata.st_mtim = st.st_mtim;
      node.metadata.st_mode = st.st_mode;
      node.metadata.st_size = 0;
      if (S_ISLNK(st.st_mode)) {
        std::vector<char> bytes(4096);
        const ssize_t length = readlink(host.c_str(), bytes.data(), bytes.size());
        if (length < 0 || static_cast<size_t>(length) == bytes.size()) fail("symlink");
        bytes.resize(length);
        node.content = bytes;
        node.metadata.st_size = length;
      } else {
        const int raw = open(host.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
            (S_ISDIR(st.st_mode) ? O_DIRECTORY : O_NONBLOCK));
        if (raw < 0) fail("open");
        struct Descriptor { int fd; ~Descriptor() { if (fd >= 0) close(fd); } } descriptor{raw};
        struct stat opened{};
        if (fstat(raw, &opened) < 0 || opened.st_dev != st.st_dev ||
            opened.st_ino != st.st_ino || opened.st_mode != st.st_mode ||
            opened.st_size != st.st_size) fail("changed object");
        if (S_ISDIR(st.st_mode)) {
          DIR* directory = fdopendir(raw);
          if (!directory) fail("directory");
          descriptor.fd = -1;
          struct Directory { DIR* value; ~Directory() { closedir(value); } } owner{directory};
          while (true) {
            errno = 0;
            const auto* entry = readdir(directory);
            if (!entry) {
              if (errno) fail("directory entries");
              break;
            }
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            pending.emplace_back(join_paths(host, name), join_paths(target, name));
          }
        } else {
          if (st.st_size < 0 || static_cast<uintmax_t>(st.st_size) >
              std::numeric_limits<size_t>::max()) fail("file size");
          std::vector<char> bytes(static_cast<size_t>(st.st_size));
          size_t offset = 0;
          while (offset < bytes.size()) {
            const ssize_t count = read(raw, bytes.data() + offset,
                std::min(bytes.size() - offset, static_cast<size_t>(SSIZE_MAX)));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) fail("file bytes");
            offset += count;
          }
          char extra;
          ssize_t count;
          do { count = read(raw, &extra, 1); } while (count < 0 && errno == EINTR);
          if (count != 0) fail("file extent");
          node.content = bytes;
          node.metadata.st_size = bytes.size();
        }
      }
      // A later mapping may replace the object at exactly the same path. Its
      // descendants retain the established ordered overlay behavior.
      filesystem[target] = std::move(node);
    }
  }
}

void FileSystemState::set_cwd(const std::string &path)
{
  if (path.empty())
    cwd = "/";
  else if (path[0] == '/')
    cwd = path;
  else
    cwd = join_paths(cwd, path);
}

const std::string &FileSystemState::get_cwd() const { return cwd; }

std::string FileSystemState::resolve_path(int dirfd, const std::string &path)
{
  std::string base;
  if (!path.empty() && path[0] == '/') base = "";
  else if (dirfd == AT_FDCWD) base = cwd.empty() ? "/" : cwd;
  else base = open_files.at(dirfd).path;
  // normalize
  std::string combined = join_paths(base, path);
  // collapse /. and /.. (simple implementation)
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < combined.size())
  {
    while (i < combined.size() && combined[i] == '/')
      i++;
    size_t j = i;
    while (j < combined.size() && combined[j] != '/')
      j++;
    if (j > i)
    {
      std::string part = combined.substr(i, j - i);
      if (part == ".")
      {
        // skip
      }
      else if (part == "..")
      {
        if (!parts.empty())
          parts.pop_back();
      }
      else
      {
        parts.push_back(part);
      }
    }
    i = j + 1;
  }
  std::string out = "/";
  for (size_t k = 0; k < parts.size(); k++)
  {
    out += parts[k];
    if (k + 1 != parts.size())
      out += '/';
  }
  if (out.empty())
    out = "/";
  return out;
}

int FileSystemState::resolve_path_checked(int dirfd, const std::string& path, std::string& result, bool follow_final, bool check_search, bool creating)
{
  if (path.empty()) return -ENOENT;
  if (path[0]!='/' && dirfd!=AT_FDCWD) {
    if (!fd_manager_ || !fd_manager_->is_allocated(dirfd)) return -EBADF;
    const auto open=open_files.find(dirfd);
    if (open==open_files.end()) return -ENOTDIR;
    const auto node=filesystem.find(open->second.path);
    if (node==filesystem.end()) throw std::runtime_error("Directory descriptor has no payload");
    if (!S_ISDIR(node->second.metadata.st_mode)) return -ENOTDIR;
  }
  // Resolve one component at a time. In particular, link/.. walks the target
  // before interpreting ..; normalizing the input first changes Linux behavior.
  std::string current = path[0]=='/' ? "/" :
      (dirfd==AT_FDCWD ? (cwd.empty() ? "/" : cwd) : open_files.at(dirfd).path);
  std::deque<std::string> pending;
  auto prepend = [&](const std::string& value) {
    std::deque<std::string> parts;
    size_t position = 0;
    while (position < value.size()) {
      const size_t end = value.find('/', position);
      if (end != position) parts.push_back(value.substr(position, end-position));
      if (end == std::string::npos) break;
      position = end + 1;
    }
    // A slash after the final component still requires a directory and follows
    // a final symlink, even for lstat/readlink/O_NOFOLLOW.
    if (!value.empty() && value.back() == '/' && pending.empty()) parts.push_back("");
    pending.insert(pending.begin(), parts.begin(), parts.end());
  };
  prepend(path);
  unsigned links = 0;
  while (!pending.empty()) {
    const auto parent = filesystem.find(current);
    if (parent == filesystem.end()) return -ENOENT;
    if (!S_ISDIR(parent->second.metadata.st_mode)) return -ENOTDIR;
    if (check_search && (parent->second.metadata.st_mode & 0111) != 0111)
      throw std::runtime_error("VFS search permission requires captured credentials: " + current);
    // On open(O_CREAT), a final name followed by / fails after searching
    // its parent, even if that name is a missing file or a looping link.
    if (creating && pending.size() == 2 && pending.back().empty()) return -EISDIR;
    const std::string part = pending.front();
    pending.pop_front();
    // The virtual Linux namespace uses the fixed Linux NAME_MAX.
    // Check after reaching the parent, including components from link bytes.
    if (part.size() > NAME_MAX) return -ENAMETOOLONG;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      const size_t slash = current.find_last_of('/');
      current = slash == 0 ? "/" : current.substr(0, slash);
      continue;
    }
    const std::string next = join_paths(current, part);
    const auto node = filesystem.find(next);
    if (node != filesystem.end() && S_ISLNK(node->second.metadata.st_mode) &&
        (!pending.empty() || follow_final)) {
      if (++links > 40) return -ELOOP;
      const auto& bytes = node->second.content;
      const std::string target(bytes.begin(), bytes.end());
      if (target.empty()) return -ENOENT;
      if (target.find('\0') != std::string::npos)
        throw std::runtime_error("Captured symlink contains a null byte: " + next);
      if (target[0] == '/') current = "/";
      prepend(target);
      continue;
    }
    current = next;
  }
  result = current;
  return 0;
}

int FileSystemState::do_open(const std::string &path, int flags, mode_t mode)
{
  return do_openat(AT_FDCWD, path, flags, mode);
}

int FileSystemState::do_openat(int dirfd, const std::string &input, int flags, mode_t mode)
{
  std::string path = input;
  if (path.empty())
    return -ENOENT;
  if (path.find('\0') != std::string::npos)
    return -EINVAL;
  if ((flags & O_ACCMODE) == O_ACCMODE ||
      ((flags & O_CREAT) && (flags & O_DIRECTORY)))
    return -EINVAL;
  if ((flags & (O_ASYNC | O_DIRECT | O_NOATIME | O_PATH)) ||
      (flags & O_TMPFILE) == O_TMPFILE)
    throw std::runtime_error("Unsupported virtual open flags");
  if (!fd_manager_)
    return -EMFILE;
  std::string checked;
  const bool exclusive = (flags & O_CREAT) && (flags & O_EXCL);
  const int path_error = resolve_path_checked(dirfd, path, checked,
                                              !exclusive && !(flags & O_NOFOLLOW), false, flags & O_CREAT);
  if (path_error) return path_error;
  path = checked;
  if (path == "/proc/self" && (flags & O_NOFOLLOW))
    throw std::runtime_error("Virtual proc self nofollow open is not modeled");
  auto it = filesystem.find(path);
  if (it != filesystem.end() && S_ISLNK(it->second.metadata.st_mode)) {
    if (exclusive) return -EEXIST;
    return (flags & O_DIRECTORY) ? -ENOTDIR : -ELOOP;
  }
  bool exists = (it != filesystem.end());

  if (exists)
  {
    if (exclusive) return -EEXIST;
    if ((flags & O_DIRECTORY) && !S_ISDIR(it->second.metadata.st_mode))
      return -ENOTDIR;
    if (S_ISDIR(it->second.metadata.st_mode) &&
        ((flags & O_CREAT) || (flags & O_ACCMODE) != O_RDONLY))
      return -EISDIR;
  }
  else
  {
    if (flags & O_DIRECTORY)
      return -ENOENT;
    // File does not exist.
    if (flags & O_CREAT)
    {
      // Create a new file node
      VFSNode newNode{};
#ifdef FSSTATE_DETAILED_METADATA
      memset(&newNode.metadata, 0, sizeof(struct stat));
      newNode.metadata.st_nlink = 1;
      newNode.metadata.st_uid = 0;
      newNode.metadata.st_gid = 0;
      newNode.metadata.st_atime = newNode.metadata.st_ctime = modeled_fs_time().tv_sec;
#endif
      newNode.metadata.st_mtim = modeled_fs_time();
      newNode.metadata.st_mode = S_IFREG | ((mode & 07777) & ~creation_mask_);
      newNode.metadata.st_size = 0;
      filesystem[path] = newNode;
      const size_t slash = path.find_last_of('/');
      filesystem.at(slash == 0 ? "/" : path.substr(0, slash)).metadata.st_mtim = modeled_fs_time();
    }
    else
    {
      return -ENOENT;
    }
  }

  const auto executable_link = filesystem.find("/proc/self/exe");
  if (executable_link != filesystem.end() && S_ISLNK(executable_link->second.metadata.st_mode)) {
    const auto& bytes = executable_link->second.content;
    const std::string executable_path(bytes.begin(), bytes.end());
    if (path == executable_path && ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)))
      return -ETXTBSY;
  }

  // If we got here, the file is accessible or was created.
  // Now, create an open file description for it.
  int fd = allocate_fd(FdType::FILE,
                       flags & ~(O_CLOEXEC | O_CREAT | O_EXCL | O_TRUNC),
                       (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
  if (fd < 0)
    return -EMFILE;
  if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY)
  {
    filesystem.at(path).content.clear();
    filesystem.at(path).metadata.st_size = 0;
    filesystem.at(path).metadata.st_mtim = modeled_fs_time();
  }
  OpenFileDescription ofd;
  ofd.path = path;
  ofd.offset = 0;

  open_files[fd] = ofd;

  return fd;
}

ssize_t FileSystemState::do_read(int fd, void *buf, size_t count)
{
  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return -EBADF;
  }

  if (!fd_manager_ || !fd_manager_->is_allocated(fd))
    return -EBADF;

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if (S_ISDIR(node.metadata.st_mode))
    return -EISDIR;
  if ((fd_manager_->get_status_flags(fd) & O_ACCMODE) == O_WRONLY)
  {
    return -EBADF;
  }

  if (static_cast<uint64_t>(ofd.offset) >= node.content.size())
  {
    return 0; // EOF
  }

  size_t bytes_to_read = std::min(count, node.content.size() - static_cast<size_t>(ofd.offset));
  if (bytes_to_read == 0)
    return 0;
  memcpy(buf, node.content.data() + ofd.offset, bytes_to_read);
  ofd.offset += bytes_to_read;

  return bytes_to_read;
}

ssize_t FileSystemState::do_write(int fd, const void *buf, size_t count)
{
  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return -EBADF;
  }

  if (!fd_manager_ || !fd_manager_->is_allocated(fd))
    return -EBADF;

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if ((fd_manager_->get_status_flags(fd) & O_ACCMODE) == O_RDONLY)
  {
    return -EBADF;
  }

  if (count == 0)
    return 0;
  off_t write_offset = ofd.offset;
  // Output streams retain branch-owned bytes; they are always append-only.
  if ((fd_manager_->get_status_flags(fd) & O_APPEND) ||
      fd_manager_->get_fd_type(fd) == FdType::STDIO)
  {
    write_offset = node.content.size();
  }

  if (count > static_cast<size_t>(std::numeric_limits<off_t>::max() - write_offset))
    return -EFBIG;
  node.content.write(static_cast<size_t>(write_offset), buf, count);
  ofd.offset = write_offset + count;
  node.metadata.st_size = node.content.size();
  node.metadata.st_mtim = modeled_fs_time();

  return count;
}

int FileSystemState::do_close(int fd)
{
  if (!fd_manager_ || !fd_manager_->is_allocated(fd) ||
      open_files.erase(fd) == 0)
  {
    return -EBADF;
  }
  release_fd(fd);
  return 0;
}

off_t FileSystemState::do_lseek(int fd, off_t offset, int whence)
{
  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return -EBADF;
  }

  if (!fd_manager_ || !fd_manager_->is_allocated(fd))
    return -EBADF;

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);
  if (fd_manager_->get_fd_type(fd) == FdType::STDIO)
    return -ESPIPE;
  off_t new_offset;

  off_t base = 0;
  switch (whence)
  {
    case SEEK_SET: break;
    case SEEK_CUR: base = ofd.offset; break;
    case SEEK_END:
      if (S_ISDIR(node.metadata.st_mode)) return -EINVAL;
      base = node.content.size(); break;
    default: return -EINVAL;
  }
  if (__builtin_add_overflow(base, offset, &new_offset))
    return -EOVERFLOW;

  if (new_offset < 0)
  {
    return -EINVAL;
  }

  ofd.offset = new_offset;
  return new_offset;
}

// Captured paths are the supported object identity: hard links, rename and
// unlink are not modeled. Recompute against the complete saved namespace so
// creation never renumbers old objects and collisions cannot depend on history.
static std::map<std::string, uint64_t> vfs_inodes(const std::map<std::string, VFSNode>& filesystem)
{
  std::map<std::string, uint64_t> result;
  std::map<uint64_t, std::string> owners;
  for (const auto& [path, node] : filesystem) {
    uint64_t inode = XXHash64::hash(path.data(), path.size());
    if (!inode) inode = 1;
    if (!owners.emplace(inode, path).second)
      throw std::runtime_error("Captured VFS path inode collision");
    result.emplace(path, inode);
  }
  return result;
}

static void copy_vfs_stat(const VFSNode& node, uint64_t inode, struct stat* statbuf)
{
#ifdef FSSTATE_DETAILED_METADATA
  memcpy(statbuf, &node.metadata, sizeof(struct stat));
#else
  // Minimal mode retains size, mode and precise semantic modification time.
  memset(statbuf, 0, sizeof(struct stat));
  statbuf->st_size = node.metadata.st_size;
  statbuf->st_mode = node.metadata.st_mode;
  statbuf->st_mtim = node.metadata.st_mtim;
#endif
  statbuf->st_ino = inode;
}

int FileSystemState::do_stat(const std::string &path, struct stat *statbuf, bool follow)
{
  std::string checked;
  const int path_error = resolve_path_checked(AT_FDCWD, path, checked, follow);
  if (path_error) return path_error;
  if (!follow && checked == "/proc/self")
    throw std::runtime_error("Virtual proc self link metadata is not modeled");
  auto it = filesystem.find(checked);
  if (it == filesystem.end())
  {
    return -ENOENT;
  }

  copy_vfs_stat(it->second, vfs_inodes(filesystem).at(checked), statbuf);
  return 0;
}

int FileSystemState::do_fstat(int fd, struct stat *statbuf)
{
  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return -EBADF;
  }

  if (!fd_manager_ || !fd_manager_->is_allocated(fd))
    return -EBADF;
  // Descriptions already name the opened object, including unnameable stdio
  // payloads. fstat must not perform a pathname walk or follow a later alias.
  const auto node = filesystem.find(it->second.path);
  if (node == filesystem.end())
    throw std::runtime_error("Open descriptor has no captured payload");
  copy_vfs_stat(node->second, vfs_inodes(filesystem).at(node->first), statbuf);
  return 0;
}

long FileSystemState::do_getdents64(int fd, void* buffer, unsigned int count, pid_t pid)
{
  if (!fd_manager_ || !fd_manager_->is_allocated(fd)) return -EBADF;
  const auto opened = open_files.find(fd);
  if (opened == open_files.end()) return -ENOTDIR;
  auto& description = opened->second;
  const auto node = filesystem.find(description.path);
  if (node == filesystem.end()) throw std::runtime_error("Directory descriptor has no captured payload");
  if (!S_ISDIR(node->second.metadata.st_mode)) return -ENOTDIR;
  if (description.offset < 0) throw std::runtime_error("Negative captured directory cursor");
  const auto inodes = vfs_inodes(filesystem);
  struct Entry { std::string name, path; };
  const auto slash = description.path.find_last_of('/');
  const std::string parent = slash == 0 ? "/" : description.path.substr(0, slash);
  std::vector<Entry> entries{{".", description.path}, {"..", parent}};
  const std::string prefix = description.path == "/" ? "/" : description.path + "/";
  for (auto it = filesystem.lower_bound(prefix); it != filesystem.end() && it->first.starts_with(prefix); ++it) {
    const auto name = it->first.substr(prefix.size());
    if (!name.empty() && name.find('/') == std::string::npos) {
      // Validate the complete stream before writing even its dot entries.
      // Captured mapping targets can bypass pathname lookup's NAME_MAX check;
      // narrowing an oversized record first would wrap its length/count bound.
      if (name.size() > NAME_MAX || name.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid captured directory entry name");
      entries.push_back({name, it->first});
    }
  }
  if (uint64_t(description.offset) >= entries.size()) return 0;
  const uintptr_t start = reinterpret_cast<uintptr_t>(buffer);
  // The reserved control page must never be overwritten, even by a later
  // partial record. Count is the raw unsigned x86-64 syscall argument.
  if (count && start < scratch_page + 4096 &&
      (count > UINTPTR_MAX - start || start + count > scratch_page))
    throw std::runtime_error("getdents64 output overlaps reserved scratch page");
  std::vector<maps_item> maps;
  auto scalar = [&](uintptr_t address, const auto& value) {
    constexpr size_t size = sizeof(value);
    if (size > UINTPTR_MAX - address) return false;
    if (address % 4096 + size > 4096) {
      if (maps.empty()) {
        auto file = fileutils::open_cfile("/proc/" + std::to_string(pid) + "/maps", "r");
        if (!file) throw std::runtime_error("Cannot inspect directory scalar mappings");
        get_maps_item(maps, file.get());
        if (ferror(file.get()) || maps.empty()) throw std::runtime_error("Cannot read directory scalar mappings");
      }
      auto cursor = address;
      for (const auto& map : maps) {
        if (map.end <= cursor) continue;
        if (map.start > cursor || map.flags[1] != 'w') break;
        cursor = std::min<uint64_t>(map.end, address + size);
        if (cursor == address + size) break;
      }
      if (cursor != address + size) return false;
    }
    return guest_write_exact(pid, reinterpret_cast<void*>(address), &value, size);
  };
  int remaining = static_cast<int32_t>(count);
  size_t written = 0, previous = 0;
  long error = 0;
  while (uint64_t(description.offset) < entries.size()) {
    const auto& entry = entries[description.offset];
    const uint16_t length = (19 + entry.name.size() + 1 + 7) & ~size_t(7);
    if (length > remaining) { error = -EINVAL; break; }
    const uintptr_t address = start + written;
    // Match access_ok's range check without requiring padding to be mapped.
    if (address < start || address >= (uint64_t(1) << 47) || length > (uint64_t(1) << 47) - address) {
      error = -EFAULT; break;
    }
    const uint64_t cookie = description.offset;
    const uint64_t inode = inodes.at(entry.path);
    const auto mode = filesystem.at(entry.path).metadata.st_mode;
    const uint8_t type = S_ISDIR(mode) ? DT_DIR : S_ISREG(mode) ? DT_REG : S_ISLNK(mode) ? DT_LNK : DT_UNKNOWN;
    const uint8_t zero = 0;
    // Linux 6.12 filldir64: previous d_off, inode, reclen, type,
    // name terminator, name; final d_off is stored on syscall completion.
    if (!scalar(address - previous + 8, cookie) || !scalar(address, inode) ||
        !scalar(address + 16, length) || !scalar(address + 18, type) ||
        !scalar(address + 19 + entry.name.size(), zero) ||
        !guest_write_exact(pid, reinterpret_cast<void*>(address + 19), entry.name.data(), entry.name.size())) {
      error = -EFAULT; break;
    }
    ++description.offset;
    previous = length;
    written += length;
    remaining -= length;
  }
  if (previous) {
    const uint64_t cookie = description.offset;
    if (!scalar(start + written - previous + 8, cookie)) return -EFAULT;
    return written;
  }
  return error;
}

long emu_vfs_getdents64(int fd, void* buffer, unsigned int count)
{
  return ptmc_state.fs_states[ptmc_state.cursor].do_getdents64(fd, buffer, count, ptmc_state.pids[ptmc_state.cursor]);
}

int FileSystemState::do_chdir(const std::string &path)
{
  if (path.empty()) return -ENOENT;
  std::string resolved;
  const int error = resolve_path_checked(AT_FDCWD, path, resolved);
  if (error) return error;

  auto it = filesystem.find(resolved);
  if (it == filesystem.end())
  {
    return -ENOENT;
  }

  if (!S_ISDIR(it->second.metadata.st_mode))
  {
    return -ENOTDIR;
  }

  cwd = resolved;
  return 0;
}

/* ======================================================================
 * Section: VFS-based Syscall Handlers
 * These functions bridge between guest system calls and VFS operations.
 * ====================================================================== */

#include "monitor.h"

// Path arguments are copied exactly; an invalid guest address cannot become
// an empty or partially read pathname.
static long read_guest_string(const char *guest_addr, std::string &result)
{
  if (!guest_addr)
    return -EFAULT;
  result.clear();
  for (size_t i = 0; i < 4096; ++i)
  {
    char ch;
    if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor],
                          guest_addr + i, &ch, 1))
      return -EFAULT;
    if (ch == '\0')
      return 0;
    result += ch;
  }
  return -ENAMETOOLONG;
}

long emu_vfs_openat(int dirfd, const char *path_ptr, int flags, mode_t mode)
{
  std::string path;
  long copied = read_guest_string(path_ptr, path);
  if (copied < 0)
    return copied;
  auto& fs=ptmc_state.fs_states[ptmc_state.cursor];
  return fs.do_openat(dirfd,path,flags,mode);
}

long emu_vfs_lseek(int fd, off_t offset, int whence)
{
  return ptmc_state.fs_states[ptmc_state.cursor].do_lseek(fd, offset, whence);
}

long emu_vfs_stat(const char* path,struct stat* destination)
{
  return emu_vfs_statat(AT_FDCWD,path,destination,0);
}

long emu_chdir(const char *path_ptr)
{
  std::string path;
  long copied = read_guest_string(path_ptr, path);
  if (copied < 0)
    return copied;
  return ptmc_state.fs_states[ptmc_state.cursor].do_chdir(path);
}

template void
VFSNode::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void
VFSNode::serialize<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive &);

template void OpenFileDescription::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void OpenFileDescription::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

template void FileSystemState::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void FileSystemState::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

int FileSystemState::do_pipe(int pipefd[2], int flags)
{
  if (flags & ~(O_NONBLOCK | O_CLOEXEC))
    return -EINVAL;
  int pipe_id = 1;
  while (pipes_.contains(pipe_id))
    ++pipe_id;

  // Create the pipe
  pipes_[pipe_id] = Pipe();

  // Allocate two fds: read end and write end
  int read_fd = allocate_fd(FdType::PIPE, O_RDONLY | (flags & O_NONBLOCK),
                            (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
  int write_fd = allocate_fd(FdType::PIPE, O_WRONLY | (flags & O_NONBLOCK),
                             (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);

  if (read_fd < 0 || write_fd < 0)
  {
    if (read_fd >= 0)
      release_fd(read_fd);
    if (write_fd >= 0)
      release_fd(write_fd);
    pipes_.erase(pipe_id);
    return -EMFILE;
  }

  // Record pipe fds: positive pipe_id for read end, negative for write end
  pipe_fds_[read_fd] = pipe_id;
  pipe_fds_[write_fd] = -pipe_id;

  pipefd[0] = read_fd;
  pipefd[1] = write_fd;

  LOG_TRACE("pipe() = [%d, %d] (pipe_id=%d)", read_fd, write_fd, pipe_id);
  return 0;
}

bool FileSystemState::is_pipe(int fd) const
{
  return pipe_fds_.find(fd) != pipe_fds_.end();
}

ssize_t FileSystemState::do_pipe_read(int fd, void *buf, size_t count)
{
  auto it = pipe_fds_.find(fd);
  if (!fd_manager_ || fd_manager_->get_fd_type(fd) != FdType::PIPE ||
      it == pipe_fds_.end())
  {
    return -EBADF;
  }

  int pipe_id = it->second;
  if (pipe_id < 0)
  {
    return -EBADF; // Write end cannot read
  }

  auto pipe_it = pipes_.find(pipe_id);
  if (pipe_it == pipes_.end())
  {
    return -EBADF;
  }

  Pipe &pipe = pipe_it->second;
  if (count == 0)
    return 0;
  if (pipe.buffer.empty())
  {
    if (pipe.write_closed)
    {
      return 0; // EOF
    }
    return -EAGAIN; // No data available
  }

  size_t to_read = std::min(count, pipe.buffer.size());
  std::vector<char> data(pipe.buffer.begin(), pipe.buffer.begin() + to_read);
  if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], buf, data.data(), to_read))
    return -EFAULT;
  pipe.buffer.erase(pipe.buffer.begin(), pipe.buffer.begin() + to_read);

  LOG_TRACE("pipe_read(fd=%d, count=%zu) = %zu", fd, count, to_read);
  return to_read;
}

ssize_t FileSystemState::do_pipe_write(int fd, const void *buf, size_t count)
{
  auto it = pipe_fds_.find(fd);
  if (!fd_manager_ || fd_manager_->get_fd_type(fd) != FdType::PIPE ||
      it == pipe_fds_.end())
  {
    return -EBADF;
  }

  int pipe_id = it->second;
  if (pipe_id > 0)
  {
    return -EBADF; // Read end cannot write
  }
  pipe_id = -pipe_id; // Make positive

  auto pipe_it = pipes_.find(pipe_id);
  if (pipe_it == pipes_.end())
  {
    return -EBADF;
  }

  Pipe &pipe = pipe_it->second;
  if (count == 0)
    return 0;
  if (pipe.read_closed)
  {
    return -EPIPE; // Reader closed
  }

  // Read data from guest
  if (count > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
    return -EINVAL;
  const size_t available = pipe_capacity - pipe.buffer.size();
  if (count > pipe_atomic_limit && count > available &&
      !(fd_manager_->get_status_flags(fd) & O_NONBLOCK))
    throw std::runtime_error("Blocking pipe writes requiring partial progress are unsupported");
  if (!available || (count <= pipe_atomic_limit && count > available)) return -EAGAIN;
  count = std::min(count, available);
  std::vector<char> data(count);
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], buf, data.data(), count))
    return -EFAULT;

  // Append to pipe buffer
  pipe.buffer.insert(pipe.buffer.end(), data.begin(), data.end());

  LOG_TRACE("pipe_write(fd=%d, count=%zu) = %zu", fd, count, count);
  return count;
}

int FileSystemState::do_pipe_close(int fd)
{
  auto it = pipe_fds_.find(fd);
  if (!fd_manager_ || fd_manager_->get_fd_type(fd) != FdType::PIPE ||
      it == pipe_fds_.end())
  {
    return -EBADF;
  }

  int pipe_id = it->second;
  bool is_read_end = (pipe_id > 0);
  if (!is_read_end)
  {
    pipe_id = -pipe_id;
  }

  auto pipe_it = pipes_.find(pipe_id);
  if (pipe_it != pipes_.end())
  {
    if (is_read_end)
    {
      pipe_it->second.read_closed = true;
    }
    else
    {
      pipe_it->second.write_closed = true;
    }

    // Clean up if both ends are closed
    if (pipe_it->second.read_closed && pipe_it->second.write_closed)
    {
      pipes_.erase(pipe_it);
    }
  }

  pipe_fds_.erase(it);
  release_fd(fd);

  LOG_TRACE("pipe_close(fd=%d)", fd);
  return 0;
}

long emu_pipe(int pipefd[2], int flags)
{
  auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
  return fs.do_pipe(pipefd, flags);
}

long emu_pipe2(int pipefd[2], int flags) { return emu_pipe(pipefd, flags); }
template <class Archive>
void FileSystemState::Pipe::serialize(Archive &ar)
{
  ar(buffer, read_closed, write_closed);
}

template void FileSystemState::Pipe::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void FileSystemState::Pipe::serialize<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive &);

long emu_vfs_statat(int dirfd,const char* path_ptr,struct stat* destination,int flags)
{
  if (flags & ~(AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT)) return -EINVAL;
  std::string path;
  const long copied=read_guest_string(path_ptr,path);
  if (copied) return copied;
  auto& fs=ptmc_state.fs_states[ptmc_state.cursor];
  if (path.empty() && (flags&AT_EMPTY_PATH)) {
    if (dirfd!=AT_FDCWD) return emu_fstat(dirfd,destination);
    path=fs.get_cwd().empty()?"/":fs.get_cwd();
  }
  std::string resolved;
  const int error=fs.resolve_path_checked(dirfd,path,resolved,!(flags&AT_SYMLINK_NOFOLLOW));
  if (error) return error;
  struct stat value{};
  const int result=fs.do_stat(resolved,&value,!(flags&AT_SYMLINK_NOFOLLOW));
  if (result) return result;
  return guest_write_exact(ptmc_state.pids[ptmc_state.cursor],destination,&value,sizeof(value))?0:-EFAULT;
}
long FileSystemState::do_access(int dirfd, const std::string& input, int mode, int flags)
{
  if ((mode & ~(R_OK | W_OK | X_OK)) ||
      (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))) return -EINVAL;
  if (mode != F_OK)
    throw std::runtime_error("VFS access permissions require captured credentials");
  std::string path = input;
  if (path.empty() && (flags & AT_EMPTY_PATH)) {
    if (dirfd == AT_FDCWD) return filesystem.contains(cwd.empty() ? "/" : cwd) ? 0 : -ENOENT;
    if (!fd_manager_ || !fd_manager_->is_allocated(dirfd)) return -EBADF;
    const auto opened = open_files.find(dirfd);
    if (opened == open_files.end())
      throw std::runtime_error("VFS empty access on non-file descriptor is not modeled");
    return filesystem.contains(opened->second.path) ? 0 : -ENOENT;
  }
  std::string resolved;
  const int error = resolve_path_checked(dirfd, path, resolved,
      !(flags & AT_SYMLINK_NOFOLLOW), true);
  if (error) return error;
  return filesystem.contains(resolved) ? 0 : -ENOENT;
}

long emu_vfs_accessat(int dirfd, const char* path_ptr, int mode, int flags)
{
  if ((mode & ~(R_OK | W_OK | X_OK)) ||
      (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))) return -EINVAL;
  std::string path;
  const long copied = read_guest_string(path_ptr, path);
  if (copied) return copied;
  return ptmc_state.fs_states[ptmc_state.cursor].do_access(dirfd, path, mode, flags);
}

long emu_getcwd(char* destination,size_t size)
{
  if (!size) return -EINVAL;
  const auto& stored=ptmc_state.fs_states[ptmc_state.cursor].get_cwd();
  const std::string path=stored.empty()?"/":stored;
  if (size<path.size()+1) return -ERANGE;
  return guest_write_exact(ptmc_state.pids[ptmc_state.cursor],destination,path.c_str(),path.size()+1)?long(path.size()+1):-EFAULT;
}

long FileSystemState::do_readlink(const std::string& path, std::string& target)
{
  std::string resolved;
  const int error = resolve_path_checked(AT_FDCWD, path, resolved, false);
  if (error) return error;
  if (resolved == "/proc/self")
    throw std::runtime_error("Virtual proc self link target is not modeled");
  const auto node = filesystem.find(resolved);
  if (node == filesystem.end()) return -ENOENT;
  if (!S_ISLNK(node->second.metadata.st_mode)) return -EINVAL;
  target.assign(node->second.content.begin(), node->second.content.end());
  return 0;
}

long emu_vfs_readlinkat(int dirfd, const char* path_ptr, char* buffer, size_t size)
{
  // Linux's ABI takes an int, even though libc exposes size_t.
  const int count = static_cast<int>(size);
  if (count <= 0) return -EINVAL;
  std::string path;
  const long copied = read_guest_string(path_ptr, path);
  if (copied) return copied;
  auto& fs = ptmc_state.fs_states[ptmc_state.cursor];
  std::string resolved;
  const int error = fs.resolve_path_checked(dirfd, path, resolved, false);
  if (error) return error;
  std::string target;
  const long result = fs.do_readlink(resolved, target);
  if (result) return result;
  const size_t length = std::min(target.size(), static_cast<size_t>(count));
  return guest_write_exact(ptmc_state.pids[ptmc_state.cursor], buffer, target.data(), length)
      ? static_cast<long>(length) : -EFAULT;
}

void FileSystemState::capture_executable(const std::string& executable)
{
  namespace fs = std::filesystem;
  const fs::path absolute = fs::absolute(executable);
  const std::string canonical = fs::canonical(absolute).string();
  std::string path = absolute.lexically_normal().string();
  if (fs::canonical(path).string() != canonical)
    throw std::runtime_error("Executable link/dot-dot traversal is not modeled");
  for (unsigned depth = 0; ; ++depth) {
    if (depth == 40) throw std::runtime_error("Executable symlink chain exceeds model limit");
    struct stat metadata{};
    if (lstat(path.c_str(), &metadata))
      throw std::runtime_error("Cannot capture executable path: " + path);
    VFSNode node{};
    node.metadata.st_mode = metadata.st_mode;
    node.metadata.st_mtim = metadata.st_mtim;
    if (S_ISLNK(metadata.st_mode)) {
      char target[4096];
      const ssize_t length = readlink(path.c_str(), target, sizeof(target));
      if (length < 0 || static_cast<size_t>(length) == sizeof(target))
        throw std::runtime_error("Cannot capture executable symlink: " + path);
      node.content.assign(target, target + length);
    } else {
      if (!S_ISREG(metadata.st_mode)) throw std::runtime_error("Executable is not a regular file");
      std::ifstream input(path, std::ios::binary);
      node.content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
      if (!input || node.content.size() != static_cast<size_t>(metadata.st_size))
        throw std::runtime_error("Cannot capture executable content: " + path);
    }
    node.metadata.st_size = node.content.size();
    const bool link = S_ISLNK(metadata.st_mode);
    const std::string target(node.content.begin(), link ? node.content.end() : node.content.begin());
    // Capture ancestors, without flattening host directory symlinks into fake
    // directories. Modeled traversal uses only these captured link bytes.
    fs::path prefix;
    for (const auto& component : fs::path(path).parent_path()) {
      prefix /= component;
      const std::string name = prefix.string();
      struct stat ancestor{};
      if (lstat(name.c_str(), &ancestor)) throw std::runtime_error("Cannot capture executable ancestor: " + name);
      VFSNode directory{};
      directory.metadata.st_mode = ancestor.st_mode;
      directory.metadata.st_mtim = ancestor.st_mtim;
      if (S_ISLNK(ancestor.st_mode)) {
        char target[4096];
        const ssize_t length = readlink(name.c_str(), target, sizeof(target));
        if (length < 0 || static_cast<size_t>(length) == sizeof(target))
          throw std::runtime_error("Cannot capture executable ancestor link: " + name);
        directory.content.assign(target, target + length);
      }
      directory.metadata.st_size = directory.content.size();
      const auto existing = filesystem.find(name);
      if (existing != filesystem.end() &&
          ((existing->second.metadata.st_mode & S_IFMT) != (directory.metadata.st_mode & S_IFMT) ||
           (S_ISLNK(directory.metadata.st_mode) && existing->second.content != directory.content)))
        throw std::runtime_error("Executable ancestor conflicts with configured virtual file: " + name);
      filesystem.emplace(name, std::move(directory));
    }
    const auto prior = filesystem.find(path);
    if (prior != filesystem.end() &&
        (prior->second.metadata.st_mode != node.metadata.st_mode || prior->second.content != node.content))
      throw std::runtime_error("Executable conflicts with configured virtual file: " + path);
    filesystem[path] = std::move(node);
    if (!link) {
      if (path != canonical) { path = canonical; continue; }
      break;
    }
    const fs::path next = fs::path(target).is_absolute() ? fs::path(target) : fs::path(path).parent_path()/target;
    path = next.lexically_normal().string();
    if (fs::canonical(next).string() != fs::canonical(path).string())
      throw std::runtime_error("Executable link/dot-dot traversal is not modeled");
  }
  VFSNode proc_link{};
  proc_link.metadata.st_mode = S_IFLNK | 0777;
  proc_link.content.assign(canonical.begin(), canonical.end());
  proc_link.metadata.st_size = proc_link.content.size();
  if (filesystem.contains("/proc/self/exe"))
    throw std::runtime_error("Executable link conflicts with configured virtual file: /proc/self/exe");
  ensure_dir_exists("/");
  ensure_dir_exists("/proc");
  ensure_dir_exists("/proc/self");
  if (!S_ISDIR(filesystem.at("/proc").metadata.st_mode) ||
      !S_ISDIR(filesystem.at("/proc/self").metadata.st_mode))
    throw std::runtime_error("Executable proc projection conflicts with configured virtual filesystem");
  filesystem["/proc/self/exe"] = std::move(proc_link);
}
