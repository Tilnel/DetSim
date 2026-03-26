#include "fsstate.h"
#include "cereal/archives/binary.hpp"
#include "cereal/cereal.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/utility.hpp"
#include "cereal/types/vector.hpp"
#include "debug.h"
#include "monitor.h"
#include "sockstate.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <libgen.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>

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
  ar(CEREAL_NVP(path), CEREAL_NVP(offset), CEREAL_NVP(flags));
}

template <class Archive>
void FileSystemState::serialize(Archive &ar)
{
  /* Note: fd_manager_ is shared and not serialized per instance */
  ar(filesystem, open_files, cwd, mappings);
}

/* --- FileSystemState Method Implementations --- */

int FileSystemState::get_new_fd()
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(FdType::FILE);
}

int FileSystemState::allocate_fd(FdType type)
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(type);
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

  VFSNode dir;
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
        if (stat(host_p.c_str(), &st) < 0)
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
  if (path.empty())
    return cwd.empty() ? std::string("/") : cwd;
  if (path[0] == '/')
  {
    return path;
  }
  // relative path
  std::string base;
  if (dirfd == AT_FDCWD)
  {
    base = cwd.empty() ? "/" : cwd;
  }
  else
  {
    auto it = open_files.find(dirfd);
    if (it != open_files.end())
    {
      std::string p = it->second.path;
      // if it's a file, use its directory
      size_t pos = p.find_last_of('/');
      if (pos == std::string::npos)
        base = "/";
      else if (pos == 0)
        base = "/";
      else
        base = p.substr(0, pos);
    }
    else
    {
      base = cwd.empty() ? "/" : cwd;
    }
  }
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

int FileSystemState::do_open(const std::string &path, int flags, mode_t mode)
{
  // Simplified path handling for now. Assumes absolute paths.
  auto it = filesystem.find(path);
  bool exists = (it != filesystem.end());

  if (exists)
  {
    // File exists. Check permissions, etc. (to be implemented)
    if ((flags & O_CREAT) && (flags & O_EXCL))
    {
      return -EEXIST;
    }
  }
  else
  {
    // File does not exist.
    if (flags & O_CREAT)
    {
      // Create a new file node
      VFSNode newNode;
#ifdef FSSTATE_DETAILED_METADATA
      memset(&newNode.metadata, 0, sizeof(struct stat));
      newNode.metadata.st_nlink = 1;
      newNode.metadata.st_uid = getuid();
      newNode.metadata.st_gid = getgid();
      newNode.metadata.st_atime = newNode.metadata.st_mtime =
          newNode.metadata.st_ctime = time(NULL);
#endif
      newNode.metadata.st_mode = mode;
      newNode.metadata.st_size = 0;
      filesystem[path] = newNode;
    }
    else
    {
      return -ENOENT;
    }
  }

  // If we got here, the file is accessible or was created.
  // Now, create an open file description for it.
  int fd = get_new_fd();
  OpenFileDescription ofd;
  ofd.path = path;
  ofd.flags = flags;
  ofd.offset = 0; // TODO: handle O_APPEND

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

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if ((ofd.flags & O_ACCMODE) == O_WRONLY)
  {
    return -EBADF;
  }

  ssize_t readable_bytes = node.content.size() - ofd.offset;
  if (readable_bytes <= 0)
  {
    return 0; // EOF
  }

  ssize_t bytes_to_read = std::min((ssize_t)count, readable_bytes);
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

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if ((ofd.flags & O_ACCMODE) == O_RDONLY)
  {
    return -EBADF;
  }

  // Handle append mode
  if (ofd.flags & O_APPEND)
  {
    ofd.offset = node.content.size();
  }

  size_t new_size = ofd.offset + count;
  if (new_size > node.content.size())
  {
    node.content.resize(new_size);
  }

  memcpy(node.content.data() + ofd.offset, buf, count);
  ofd.offset += count;
  node.metadata.st_size = node.content.size();
#ifdef FSSTATE_DETAILED_METADATA
  node.metadata.st_mtime = time(NULL);
#endif

  return count;
}

int FileSystemState::do_close(int fd)
{
  if (open_files.erase(fd) == 0)
  {
    return -EBADF;
  }
  return 0;
}

off_t FileSystemState::do_lseek(int fd, off_t offset, int whence)
{
  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return -EBADF;
  }

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);
  off_t new_offset;

  switch (whence)
  {
    case SEEK_SET:
      new_offset = offset;
      break;
    case SEEK_CUR:
      new_offset = ofd.offset + offset;
      break;
    case SEEK_END:
      new_offset = node.content.size() + offset;
      break;
    default:
      return -EINVAL;
  }

  if (new_offset < 0)
  {
    return -EINVAL;
  }

  ofd.offset = new_offset;
  return new_offset;
}

int FileSystemState::do_stat(const std::string &path, struct stat *statbuf)
{
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

  return do_stat(it->second.path, statbuf);
}

int FileSystemState::do_chdir(const std::string &path)
{
  std::string resolved = resolve_path(AT_FDCWD, path);

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

// Helper to read a null-terminated string from guest memory.
// Returns empty string if guest_addr is 0 or string exceeds max length.
static std::string read_guest_string(uintptr_t guest_addr)
{
  if (guest_addr == 0)
    return "";

  std::string str;
  str.reserve(256);
  char ch;
  size_t max_len = 4096; // Reasonable limit for path strings
  do
  {
    if (str.length() >= max_len)
      break;
    memcpy_guest2host(&ch, (void *)(guest_addr + str.length()), 1);
    if (ch != '\0')
    {
      str += ch;
    }
  } while (ch != '\0');
  return str;
}

long emu_vfs_openat(int dirfd, const char *path_ptr, int flags, mode_t mode)
{
  if (!path_ptr)
    return -EFAULT;
  std::string path = read_guest_string((uintptr_t)path_ptr);
  std::string resolved =
      ptmc_state.fs_states[ptmc_state.cursor].resolve_path(dirfd, path);
  return ptmc_state.fs_states[ptmc_state.cursor].do_open(resolved, flags, mode);
}

long emu_vfs_read(int fd, void *buf, size_t count)
{
  // Check if fd is a socket
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  if (sock_state.is_valid_socket(fd))
  {
    ssize_t ret = sock_state.do_recv(fd, buf, count, 0);
    return ret;
  }

  std::vector<char> host_buf(count);
  long bytes_read = ptmc_state.fs_states[ptmc_state.cursor].do_read(
      fd, host_buf.data(), count);

  if (bytes_read > 0)
  {
    memcpy_host2guest(buf, host_buf.data(), bytes_read);
  }
  return bytes_read;
}

long emu_vfs_write(int fd, const void *buf, size_t count)
{
  // Check if fd is a socket
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  if (sock_state.is_valid_socket(fd))
  {
    ssize_t ret = sock_state.do_send(fd, buf, count, 0);
    return ret;
  }

  std::vector<char> host_buf(count);
  memcpy_guest2host(host_buf.data(), buf, count);

  return ptmc_state.fs_states[ptmc_state.cursor].do_write(fd, host_buf.data(),
                                                          count);
}

long emu_vfs_close(int fd)
{
  auto &fs = ptmc_state.fs_states[ptmc_state.cursor];

  // Check if it's a socket
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  if (sock_state.get_socket(fd) != nullptr)
  {
    return sock_state.do_close(fd);
  }

  // Check if it's a pipe
  if (fs.is_pipe(fd))
  {
    return fs.do_pipe_close(fd);
  }

  // Regular file
  return fs.do_close(fd);
}

long emu_vfs_lseek(int fd, off_t offset, int whence)
{
  return ptmc_state.fs_states[ptmc_state.cursor].do_lseek(fd, offset, whence);
}

long emu_vfs_stat(const char *path_ptr, struct stat *statbuf)
{
  if (!path_ptr)
    return -EFAULT;
  std::string path = read_guest_string((uintptr_t)path_ptr);

  struct stat host_statbuf;
  long result =
      ptmc_state.fs_states[ptmc_state.cursor].do_stat(path, &host_statbuf);

  if (result == 0)
  {
    memcpy_host2guest(statbuf, &host_statbuf, sizeof(struct stat));
  }
  return result;
}

long emu_vfs_fstat(int fd, struct stat *statbuf)
{
  struct stat host_statbuf;
  long result =
      ptmc_state.fs_states[ptmc_state.cursor].do_fstat(fd, &host_statbuf);

  if (result == 0)
  {
    memcpy_host2guest(statbuf, &host_statbuf, sizeof(struct stat));
  }
  return result;
}

long emu_chdir(const char *path_ptr)
{
  if (!path_ptr)
    return -EFAULT;
  std::string path = read_guest_string((uintptr_t)path_ptr);
  return ptmc_state.fs_states[ptmc_state.cursor].do_chdir(path);
}

void *FileSystemState::do_mmap(void *addr, size_t length, int prot, int flags,
                               int fd, off_t offset)
{
  (void)flags;
  (void)addr;

  auto it = open_files.find(fd);
  if (it == open_files.end())
  {
    return MAP_FAILED;
  }

  std::string &path = it->second.path;
  auto node_it = filesystem.find(path);
  if (node_it == filesystem.end())
  {
    return MAP_FAILED;
  }

  VFSNode &node = node_it->second;

  // Check offset bounds
  if ((size_t)offset > node.content.size())
  {
    return MAP_FAILED;
  }

  // Allocate memory via host mmap
  size_t map_len = length;
  if (offset + (off_t)length > (off_t)node.content.size())
  {
    map_len = node.content.size() - offset;
  }

  void *mem = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
  {
    return MAP_FAILED;
  }

  // Copy file content to the allocated memory
  if (map_len > 0)
  {
    memcpy(mem, node.content.data() + offset, map_len);
  }

  // Zero the rest if length > file size
  if (length > map_len)
  {
    memset((char *)mem + map_len, 0, length - map_len);
  }

  // Track this mmap region
  MmapRegion region;
  region.fd = fd;
  region.offset = offset;
  region.length = length;
  region.prot = prot;
  mmap_regions[(uintptr_t)mem] = region;

  // Apply protection
  int host_prot = 0;
  if (prot & PROT_READ)
    host_prot |= PROT_READ;
  if (prot & PROT_WRITE)
    host_prot |= PROT_WRITE;
  if (prot & PROT_EXEC)
    host_prot |= PROT_EXEC;
  mprotect(mem, length, host_prot);

  return mem;
}

int FileSystemState::do_munmap(void *addr, size_t length)
{
  uintptr_t key = (uintptr_t)addr;
  auto it = mmap_regions.find(key);
  if (it != mmap_regions.end())
  {
    mmap_regions.erase(it);
  }
  return munmap(addr, length);
}

long emu_mmap(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset)
{
  // Check if fd is a VFS file
  auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
  if (fd >= 0 && fs.open_files.find(fd) != fs.open_files.end())
  {
    void *result = fs.do_mmap(addr, length, prot, flags, fd, offset);
    return (long)result;
  }

  // Not a VFS file, let the host handle it (this will likely fail or be NOP)
  // We still need to return something valid for anonymous mappings
  if (fd == -1)
  {
    // Anonymous mapping - let host handle it
    void *result = mmap(addr, length, prot, flags, fd, offset);
    return (long)result;
  }

  // For non-VFS files with fd >= 0, we can't really support this
  // Return MAP_FAILED
  return (long)MAP_FAILED;
}

long emu_munmap(void *addr, size_t length)
{
  auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
  return fs.do_munmap(addr, length);
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
  (void)flags;

  // Allocate pipe ID
  static int next_pipe_id = 1;
  int pipe_id = next_pipe_id++;

  // Create the pipe
  pipes_[pipe_id] = Pipe();

  // Allocate two fds: read end and write end
  int read_fd = allocate_fd(FdType::PIPE);
  int write_fd = allocate_fd(FdType::PIPE);

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
  if (it == pipe_fds_.end())
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
  memcpy_host2guest(buf, data.data(), to_read);
  pipe.buffer.erase(pipe.buffer.begin(), pipe.buffer.begin() + to_read);

  LOG_TRACE("pipe_read(fd=%d, count=%zu) = %zu", fd, count, to_read);
  return to_read;
}

ssize_t FileSystemState::do_pipe_write(int fd, const void *buf, size_t count)
{
  auto it = pipe_fds_.find(fd);
  if (it == pipe_fds_.end())
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
  if (pipe.read_closed)
  {
    return -EPIPE; // Reader closed
  }

  // Read data from guest
  std::vector<char> data(count);
  memcpy_guest2host(data.data(), buf, count);

  // Append to pipe buffer
  pipe.buffer.insert(pipe.buffer.end(), data.begin(), data.end());

  LOG_TRACE("pipe_write(fd=%d, count=%zu) = %zu", fd, count, count);
  return count;
}

int FileSystemState::do_pipe_close(int fd)
{
  auto it = pipe_fds_.find(fd);
  if (it == pipe_fds_.end())
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