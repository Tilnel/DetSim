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
#include <libgen.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef FSSTATE_DETAILED_METADATA
static time_t modeled_fs_time()
{
  const int node=ptmc_state.cursor;
  return node>=0 && node<NP ? ptmc_state.time[node].tv_sec : 0;
}
#endif

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
     CEREAL_NVP(metadata.st_mtime), CEREAL_NVP(metadata.st_ctime));
#else
  ar(CEREAL_NVP(content), CEREAL_NVP(metadata.st_size),
     CEREAL_NVP(metadata.st_mode));
#endif
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
  if (filesystem.find(path) != filesystem.end())
    return;

  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0)
    ensure_dir_exists(path.substr(0, slash));
  else if (path != "/")
    ensure_dir_exists("/");
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

  for (const auto &m : mappings)
  {
    const std::string &host_base = m.first;
    const std::string &target_base = m.second;

    ensure_dir_exists(target_base);

    DIR *d = opendir(host_base.c_str());
    if (!d)
      continue;
    struct dirent *de;
    std::vector<std::string> stack;
    stack.push_back(host_base);
    while (!stack.empty())
    {
      std::string cur = stack.back();
      stack.pop_back();
      DIR *cd = opendir(cur.c_str());
      if (!cd)
        continue;
      while ((de = readdir(cd)) != NULL)
      {
        if (de->d_name[0] == '.')
          continue;
        std::string host_p = cur + "/" + de->d_name;
        struct stat st;
        if (lstat(host_p.c_str(), &st) < 0)
          continue;

        std::string rel = host_p.substr(host_base.size());
        if (!rel.empty() && rel[0] == '/')
          rel = rel.substr(1);
        std::string target_p = target_base;
        if (target_p.back() != '/')
          target_p += '/';
        target_p += rel;

        if (S_ISDIR(st.st_mode))
        {
          ensure_dir_exists(target_p);
          stack.push_back(host_p);
          continue;
        }

        // Create parent directories for the file
        size_t last_slash = target_p.find_last_of('/');
        if (last_slash != std::string::npos && last_slash > 0)
        {
          std::string parent = target_p.substr(0, last_slash);
          ensure_dir_exists(parent);
        }

        if (S_ISLNK(st.st_mode)) {
          std::vector<char> target(4096);
          const ssize_t length = readlink(host_p.c_str(), target.data(), target.size());
          if (length < 0 || static_cast<size_t>(length) == target.size())
            throw std::runtime_error("Cannot capture VFS symlink: " + host_p);
          target.resize(length);
          VFSNode node{};
          node.content = std::move(target);
          node.metadata.st_mode = st.st_mode;
          node.metadata.st_size = length;
          filesystem[target_p] = std::move(node);
          continue;
        }
        std::ifstream ifs(host_p, std::ios::binary);
        std::vector<char> content;
        if (ifs)
        {
          ifs.seekg(0, std::ios::end);
          std::streamsize sz = ifs.tellg();
          ifs.seekg(0, std::ios::beg);
          if (sz > 0)
          {
            content.resize((size_t)sz);
            ifs.read(content.data(), sz);
          }
        }

        VFSNode node;
        node.content = std::move(content);
#ifdef FSSTATE_DETAILED_METADATA
        memset(&node.metadata, 0, sizeof(struct stat));
        node.metadata.st_nlink = 1;
        node.metadata.st_mtime = st.st_mtime;
#endif
        node.metadata.st_mode = st.st_mode;
        node.metadata.st_size = node.content.size();
        filesystem[target_p] = std::move(node);
      }
      closedir(cd);
    }
    closedir(d);
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

int FileSystemState::resolve_path_checked(int dirfd, const std::string& path, std::string& result, bool follow_final)
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
  // Inspect components before lexical normalization: link/.. still traverses a
  // link in Linux. Unsupported traversal must not accidentally resolve elsewhere.
  std::string current = path[0]=='/' ? "/" :
      (dirfd==AT_FDCWD ? cwd : open_files.at(dirfd).path);
  size_t position = 0;
  while (position < path.size()) {
    while (position < path.size() && path[position]=='/') ++position;
    const size_t end = path.find('/', position);
    const std::string part = path.substr(position, end-position);
    if (part.empty()) break;
    current = resolve_path(AT_FDCWD, join_paths(current, part));
    const auto node = filesystem.find(current);
    const bool final = end == std::string::npos;
    if (!final && node == filesystem.end()) return -ENOENT;
    if (node != filesystem.end() && S_ISLNK(node->second.metadata.st_mode) &&
        (!final || follow_final))
      throw std::runtime_error("Virtual symlink traversal is not modeled: " + current);
    if (!final && !S_ISDIR(node->second.metadata.st_mode)) return -ENOTDIR;
    if (end == std::string::npos) break;
    position = end + 1;
  }
  result=resolve_path(dirfd,path);
  return 0;
}

int FileSystemState::do_open(const std::string &path, int flags, mode_t mode)
{
  if (path.empty())
    return -ENOENT;
  if (path.find('\0') != std::string::npos)
    return -EINVAL;
  if ((flags & O_ACCMODE) == O_ACCMODE)
    return -EINVAL;
  if ((flags & (O_ASYNC | O_DIRECT | O_NOATIME | O_PATH)) ||
      (flags & O_TMPFILE) == O_TMPFILE)
    throw std::runtime_error("Unsupported virtual open flags");
  if (!fd_manager_)
    return -EMFILE;
  std::string checked;
  const int path_error = resolve_path_checked(AT_FDCWD, path, checked, false);
  if (path_error) return path_error;
  if (checked == "/proc/self" && (flags & O_NOFOLLOW))
    throw std::runtime_error("Virtual proc self nofollow open is not modeled");
  auto it = filesystem.find(path);
  if (it != filesystem.end() && S_ISLNK(it->second.metadata.st_mode)) {
    if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (flags & O_NOFOLLOW) return -ELOOP;
    throw std::runtime_error("Virtual symlink traversal is not modeled: " + path);
  }
  bool exists = (it != filesystem.end());

  if (exists)
  {
    if ((flags & O_DIRECTORY) && !S_ISDIR(it->second.metadata.st_mode))
      return -ENOTDIR;
    if (S_ISDIR(it->second.metadata.st_mode) && (flags & O_ACCMODE) != O_RDONLY)
      return -EISDIR;
    if ((flags & O_CREAT) && (flags & O_EXCL))
    {
      return -EEXIST;
    }
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
      newNode.metadata.st_atime = newNode.metadata.st_mtime =
          newNode.metadata.st_ctime = modeled_fs_time();
#endif
      newNode.metadata.st_mode = S_IFREG | ((mode & 07777) & ~creation_mask_);
      newNode.metadata.st_size = 0;
      filesystem[path] = newNode;
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
  size_t new_size = static_cast<size_t>(write_offset) + count;
  if (new_size > node.content.size())
  {
    node.content.resize(new_size);
  }

  memcpy(node.content.data() + write_offset, buf, count);
  ofd.offset = write_offset + count;
  node.metadata.st_size = node.content.size();
#ifdef FSSTATE_DETAILED_METADATA
  node.metadata.st_mtime = modeled_fs_time();
#endif

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
    case SEEK_END: base = node.content.size(); break;
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

int FileSystemState::do_stat(const std::string &path, struct stat *statbuf, bool follow)
{
  std::string checked;
  const int path_error = resolve_path_checked(AT_FDCWD, path, checked, follow);
  if (path_error) return path_error;
  if (!follow && checked == "/proc/self")
    throw std::runtime_error("Virtual proc self link metadata is not modeled");
  auto it = filesystem.find(path);
  if (it == filesystem.end())
  {
    return -ENOENT;
  }

#ifdef FSSTATE_DETAILED_METADATA
  memcpy(statbuf, &it->second.metadata, sizeof(struct stat));
#else
  // Minimal mode: only fill size and mode, zero out the rest
  memset(statbuf, 0, sizeof(struct stat));
  statbuf->st_size = it->second.metadata.st_size;
  statbuf->st_mode = it->second.metadata.st_mode;
#endif
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
  return do_stat(it->second.path, statbuf);
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
  std::string resolved;
  const int error=fs.resolve_path_checked(dirfd,path,resolved,false);
  if (error) return error;
  return fs.do_open(resolved,flags,mode);
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
    if (S_ISLNK(metadata.st_mode)) {
      node.content.resize(4096);
      const ssize_t length = readlink(path.c_str(), node.content.data(), node.content.size());
      if (length < 0 || static_cast<size_t>(length) == node.content.size())
        throw std::runtime_error("Cannot capture executable symlink: " + path);
      node.content.resize(length);
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
    // directories. Traversing those links remains explicitly unsupported.
    fs::path prefix;
    for (const auto& component : fs::path(path).parent_path()) {
      prefix /= component;
      const std::string name = prefix.string();
      struct stat ancestor{};
      if (lstat(name.c_str(), &ancestor)) throw std::runtime_error("Cannot capture executable ancestor: " + name);
      VFSNode directory{};
      directory.metadata.st_mode = ancestor.st_mode;
      if (S_ISLNK(ancestor.st_mode)) {
        directory.content.resize(4096);
        const ssize_t length = readlink(name.c_str(), directory.content.data(), directory.content.size());
        if (length < 0 || static_cast<size_t>(length) == directory.content.size())
          throw std::runtime_error("Cannot capture executable ancestor link: " + name);
        directory.content.resize(length);
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
