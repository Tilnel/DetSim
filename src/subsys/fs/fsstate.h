#ifndef __FSSTATE_H
#define __FSSTATE_H

#include "common.h"
#include "fd_manager.h"
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

/* --- Forward declarations for guest memory operations --- */
void *memcpy_host2guest(void *dest, const void *src, size_t n);
void *memcpy_guest2host(void *dest, const void *src, size_t n);

/* --- VFS-based Syscall Handlers ---
 * These functions implement system calls using the virtual file system.
 * They follow the naming convention of other emulated syscalls (emu_*).
 */
long emu_vfs_openat(int dirfd, const char *path, int flags, mode_t mode);
long emu_vfs_lseek(int fd, off_t offset, int whence);
long emu_vfs_stat(const char *path, struct stat *statbuf);
long emu_vfs_statat(int dirfd, const char* path, struct stat* statbuf, int flags);
long emu_getcwd(char* destination, size_t size);
long emu_chdir(const char *path);
long emu_pipe(int pipefd[2], int flags);
long emu_pipe2(int pipefd[2], int flags);

// Represents a node in the virtual file system. Can be a file or a directory.
struct VFSNode
{
  // Complete file content. For directories, this is typically empty.
  std::vector<char> content;

  // File metadata.
  // When FSSTATE_DETAILED_METADATA is defined, full struct stat is stored.
  // Otherwise, only minimal metadata (size, mode) is kept to reduce state
  // space.
#ifdef FSSTATE_DETAILED_METADATA
  struct stat metadata;
#else
  // Minimal metadata: only size and file type/mode
  struct
  {
    off_t st_size;
    mode_t st_mode;
  } metadata;
#endif

  template <class Archive>
  void serialize(Archive &ar);
};

// Represents an entry in a process's file descriptor table.
struct OpenFileDescription
{
  // The absolute path of the file in the VFS.
  std::string path;

  // The current read/write offset into the file's content.
  off_t offset;

  template <class Archive>
  void serialize(Archive &ar);
};

// Manages the complete file system state for a single tracee.
class FileSystemState
{
  public:
  // The entire virtual file system, mapping absolute paths to VFSNodes.
  std::map<std::string, VFSNode> filesystem;

  // The table of open files for the process, mapping a file descriptor (int)
  // to its description.
  std::map<int, OpenFileDescription> open_files;

  // Virtual file mappings use anonymous host VMAs. Preserve their semantic
  // provenance independently of /proc maps: canonical page-aligned [start,end).
  std::map<uint64_t, uint64_t> virtual_file_mappings;
  void add_virtual_file_mapping(uint64_t start, uint64_t end);
  void remove_virtual_file_mapping(uint64_t start, uint64_t end);
  bool overlaps_virtual_file_mapping(uint64_t start, uint64_t end) const;

  // Current working directory for the tracee (in VFS namespace)
  std::string cwd;

  // Mappings from host directories into the VFS. Each pair is (host_base,
  // target_base)
  std::vector<std::pair<std::string, std::string>> mappings;

  private:
  // FdManager for unified fd allocation (shared with SockState)
  FdManagerPtr fd_manager_;
  mode_t creation_mask_ = 0022;

  public:
  // Default constructor - ensures all members are properly initialized
  FileSystemState();

  // Copy control
  FileSystemState(const FileSystemState &other) = default;
  FileSystemState &operator=(const FileSystemState &other) = default;

  // Move control
  FileSystemState(FileSystemState &&other) noexcept = default;
  FileSystemState &operator=(FileSystemState &&other) noexcept = default;

  explicit FileSystemState(FdManagerPtr fd_mgr);

  // Attach the captured descriptor namespace without creating any descriptors.
  void set_fd_manager(FdManagerPtr fd_mgr) { fd_manager_ = fd_mgr; }
  mode_t creation_mask() const { return creation_mask_; }
  void set_creation_mask(mode_t mask) { creation_mask_ = mask & 0777; }

  template <class Archive>
  void serialize(Archive &ar);

  // --- Syscall Implementations ---
  // These will be implemented in fsstate.cpp
  int do_open(const std::string &path, int flags, mode_t mode);
  ssize_t do_read(int fd, void *buf, size_t count);
  ssize_t do_write(int fd, const void *buf, size_t count);
  int do_close(int fd);
  off_t do_lseek(int fd, off_t offset, int whence);
  int do_stat(const std::string &path, struct stat *statbuf);
  int do_fstat(int fd, struct stat *statbuf);

  // Initialize VFS from host mappings (may load files into memory)
  void init_from_mappings(
      const std::vector<std::pair<std::string, std::string>> &maps);

  // Resolve a pathname according to dirfd and cwd. Returns an absolute VFS
  // path.
  std::string resolve_path(int dirfd, const std::string &path);
  int resolve_path_checked(int dirfd, const std::string& path, std::string& result);

  // Set/get cwd
  void set_cwd(const std::string &path);
  const std::string &get_cwd() const;

  // Helper to ensure a directory exists in the VFS
  void ensure_dir_exists(const std::string &path);

  // Pipe support
  struct Pipe
  {
    std::deque<char> buffer;
    bool read_closed;
    bool write_closed;

    Pipe() : read_closed(false), write_closed(false) {}

    template <class Archive>
    void serialize(Archive &ar);
  };

  std::map<int, Pipe> pipes_;
  std::map<int, int> pipe_fds_;

  int do_pipe(int pipefd[2], int flags);
  bool is_pipe(int fd) const;
  ssize_t do_pipe_read(int fd, void *buf, size_t count);
  ssize_t do_pipe_write(int fd, const void *buf, size_t count);
  int do_pipe_close(int fd);
  int do_chdir(const std::string &path);
  int allocate_fd(FdType type, int status_flags = 0, int descriptor_flags = 0);
  void release_fd(int fd);
};

#endif /* __FSSTATE_H */
