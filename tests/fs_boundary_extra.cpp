#include "fsstate.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

int main()
{
  try
  {
    char temporary[] = "/tmp/detsim-readlink-XXXXXX";
    CHECK(mkdtemp(temporary));
    CHECK(chmod(temporary, 0755) == 0);
    const std::filesystem::path root(temporary);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    std::ofstream(root / "regular") << "payload";
    CHECK(symlink("regular", (root / "link").c_str()) == 0);
    CHECK(symlink("missing", (root / "dangling").c_str()) == 0);
    CHECK(symlink("loop", (root / "loop").c_str()) == 0);
    std::filesystem::create_directories(root / "real/deep");
    std::ofstream(root / "real/regular") << "other executable";
    CHECK(symlink("real/deep", (root / "directory_alias").c_str()) == 0);
    bool dotdot_rejected = false;
    try {
      FileSystemState ambiguous;
      ambiguous.capture_executable((root / "directory_alias/../regular").string());
    } catch (const std::runtime_error&) { dotdot_rejected = true; }
    CHECK(dotdot_rejected);
    // A mapping root is an object, not an implicitly manufactured directory.
    std::ofstream(root / ".hidden", std::ios::binary) << std::string("a\0b", 3);
    CHECK(chmod((root / "real").c_str(), 0711) == 0);
    FileSystemState single(std::make_shared<FdManager>());
    single.init_from_mappings({{(root / ".hidden").string(), "/nested/file"},
                               {(root / "dangling").string(), "/nested/link"}});
    CHECK(S_ISREG(single.filesystem.at("/nested/file").metadata.st_mode));
    CHECK(single.filesystem.at("/nested/file").content == std::vector<char>({'a', '\0', 'b'}));
    CHECK(S_ISLNK(single.filesystem.at("/nested/link").metadata.st_mode));
    for (const auto& maps : std::vector<std::vector<std::pair<std::string, std::string>>>{
        {{(root / "missing").string(), "/missing"}},
        {{(root / "regular").string(), "/"}},
        {{(root / "dangling").string(), "/"}},
        {{(root / "real").string(), "/tree"}, {(root / "regular").string(), "/tree"},
         {(root / "regular").string(), "/tree/deep/new"}},
        {{(root / "regular").string(), "/parent"}, {(root / "regular").string(), "/parent/child"}}}) {
      bool rejected = false;
      try { FileSystemState invalid; invalid.init_from_mappings(maps); }
      catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
    }
    CHECK(mkfifo((root / "fifo").c_str(), 0600) == 0);
    bool fifo_rejected = false;
    try { FileSystemState invalid; invalid.init_from_mappings({{(root / "fifo").string(), "/fifo"}}); }
    catch (const std::runtime_error&) { fifo_rejected = true; }
    CHECK(fifo_rejected);
    CHECK(unlink((root / "fifo").c_str()) == 0);
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(socket_fd >= 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const std::string socket_path = (root / "socket").string();
    CHECK(socket_path.size() < sizeof(address.sun_path));
    strcpy(address.sun_path, socket_path.c_str());
    CHECK(bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    bool socket_rejected = false;
    try { FileSystemState invalid; invalid.init_from_mappings({{socket_path, "/socket"}}); }
    catch (const std::runtime_error&) { socket_rejected = true; }
    CHECK(socket_rejected);
    CHECK(close(socket_fd) == 0 && unlink(socket_path.c_str()) == 0);
    std::ofstream(root / "unreadable") << "must not become empty";
    CHECK(chmod((root / "unreadable").c_str(), 0) == 0);
    // Root can read mode000 files; exercise the error where host credentials deny it.
    if (geteuid() != 0) {
      bool unreadable_rejected = false;
      try { FileSystemState invalid; invalid.init_from_mappings({{(root / "unreadable").string(), "/unreadable"}}); }
      catch (const std::runtime_error&) { unreadable_rejected = true; }
      CHECK(unreadable_rejected);
    }
    CHECK(unlink((root / "unreadable").c_str()) == 0);
    FileSystemState overlay;
    overlay.init_from_mappings({{(root / "regular").string(), "/same"},
                                {(root / ".hidden").string(), "/same"}});
    CHECK(overlay.filesystem.at("/same").content == std::vector<char>({'a', '\0', 'b'}));
    FileSystemState imported(std::make_shared<FdManager>());
    imported.init_from_mappings({{root.string(), "/fixture"}});
    CHECK(imported.filesystem.at("/fixture/.hidden").content == std::vector<char>({'a', '\0', 'b'}));
    CHECK((imported.filesystem.at("/fixture/real").metadata.st_mode & 0777) == 0711);
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/link").metadata.st_mode));
    CHECK(imported.filesystem.at("/fixture/link").content == std::vector<char>({'r','e','g','u','l','a','r'}));
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/dangling").metadata.st_mode));
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/loop").metadata.st_mode));
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/directory_alias").metadata.st_mode));
    CHECK(!imported.filesystem.contains("/fixture/directory_alias/regular"));
    std::string target;
    CHECK(imported.do_readlink("/fixture/link", target) == 0 && target == "regular");
    CHECK(imported.do_readlink("/fixture/dangling", target) == 0 && target == "missing");
    CHECK(imported.do_readlink("/fixture/regular", target) == -EINVAL);
    CHECK(imported.do_readlink("/fixture/missing", target) == -ENOENT);
    struct stat link_stat{};
    CHECK(imported.do_stat("/fixture/link", &link_stat, false) == 0 && S_ISLNK(link_stat.st_mode));
    CHECK(imported.do_open("/fixture/link", O_RDONLY|O_NOFOLLOW, 0) == -ELOOP);
    CHECK(imported.do_open("/fixture/link", O_CREAT|O_EXCL|O_RDWR, 0) == -EEXIST);
    for (const auto& path : {"/fixture/link", "/fixture/link/..", "/fixture/link/"}) {
      bool rejected = false;
      try { imported.do_stat(path, &link_stat); } catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
    }
    CHECK(imported.do_readlink("/absent/../fixture/link", target) == -ENOENT);
    CHECK(imported.do_readlink("/fixture/regular/../link", target) == -ENOTDIR);
    CHECK(imported.do_open("/fixture/regular", O_RDONLY, 0) >= 0);
    FileSystemState executable(std::make_shared<FdManager>());
    executable.capture_executable("/bin/true");
    const std::string canonical = std::filesystem::canonical("/bin/true").string();
    CHECK(executable.do_readlink("/proc/self/exe", target) == 0 && target == canonical);
    CHECK(executable.filesystem.contains(canonical));
    CHECK(!executable.filesystem.at(canonical).content.empty());
    CHECK(executable.do_open(canonical, O_WRONLY, 0) == -ETXTBSY);
    CHECK(executable.do_open(canonical, O_RDONLY|O_TRUNC, 0) == -ETXTBSY);
    CHECK(executable.do_open(canonical, O_CREAT|O_EXCL|O_WRONLY, 0600) == -EEXIST);
    CHECK(executable.do_open(canonical, O_RDONLY, 0) >= 0);
    CHECK(open("/proc/self/exe", O_WRONLY) == -1 && errno == ETXTBSY);
    CHECK(open("/proc/self/exe", O_CREAT|O_EXCL|O_WRONLY, 0600) == -1 && errno == EEXIST);
    CHECK(S_ISLNK(executable.filesystem.at("/bin").metadata.st_mode));
    CHECK(executable.do_readlink("/bin", target) == 0);
    for (bool stat_operation : {false, true}) {
      bool rejected = false;
      try {
        if (stat_operation) executable.do_stat("/proc/./self", &link_stat, false);
        else executable.do_readlink("/proc/./self", target);
      } catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
    }
    CHECK(open("/proc/self", O_RDONLY|O_NOFOLLOW) == -1 && errno == ELOOP);
    bool proc_open_rejected = false;
    try { executable.do_open("/proc/self", O_RDONLY|O_NOFOLLOW, 0); }
    catch (const std::runtime_error&) { proc_open_rejected = true; }
    CHECK(proc_open_rejected);
    FileSystemState conflict(std::make_shared<FdManager>());
    VFSNode conflict_node{}; conflict_node.metadata.st_mode = S_IFREG | 0600;
    conflict.filesystem["/bin/true"] = conflict_node;
    bool conflict_rejected = false;
    try { conflict.capture_executable("/bin/true"); }
    catch (const std::runtime_error&) { conflict_rejected = true; }
    CHECK(conflict_rejected);
    std::stringstream link_bytes;
    { cereal::BinaryOutputArchive ar(link_bytes); ar(imported); }
    FileSystemState link_restore;
    { cereal::BinaryInputArchive ar(link_bytes); ar(link_restore); }
    CHECK(link_restore.do_readlink("/fixture/link", target) == 0 && target == "regular");
    link_restore.filesystem.at("/fixture/link").content.write(0, "X", 1);
    CHECK(imported.do_readlink("/fixture/link", target) == 0 && target == "regular");
    // Remove host inputs: all later reads must come from the archived namespace.
    CHECK(unlink((root / "link").c_str()) == 0);
    CHECK(imported.do_readlink("/fixture/link", target) == 0 && target == "regular");
    ptmc_state.cursor = 0;
    ptmc_state.pids[0] = getpid();
    ptmc_state.fs_states[0] = imported;
    char actual[16], native[16];
    for (size_t count : {size_t(1), size_t(7), size_t(16)}) {
      memset(actual, '#', sizeof(actual)); memset(native, '#', sizeof(native));
      const ssize_t expected = readlink((root / "dangling").c_str(), native, count);
      CHECK(expected == emu_vfs_readlinkat(AT_FDCWD, "/fixture/dangling", actual, count));
      CHECK(memcmp(actual, native, sizeof(actual)) == 0);
    }
    CHECK(emu_vfs_readlinkat(-7, "/fixture/link", actual, sizeof(actual)) == 7);
    CHECK(emu_vfs_readlinkat(-7, "link", actual, sizeof(actual)) == -EBADF);
    CHECK(emu_vfs_readlinkat(AT_FDCWD, "/fixture/link", actual, 0) == -EINVAL);
    CHECK(emu_vfs_readlinkat(AT_FDCWD, "/fixture/link", actual, size_t(1)<<31) == -EINVAL);
    CHECK(emu_vfs_readlinkat(AT_FDCWD, "/fixture/link", nullptr, 1) == -EFAULT);
    CHECK(emu_vfs_readlinkat(AT_FDCWD, nullptr, actual, 1) == -EFAULT);
    const int directory = ptmc_state.fs_states[0].do_open("/fixture", O_DIRECTORY|O_RDONLY, 0);
    CHECK(directory >= 0);
    CHECK(emu_vfs_readlinkat(directory, "link", actual, sizeof(actual)) == 7);
    CHECK(emu_vfs_readlinkat(directory, "", actual, sizeof(actual)) == -ENOENT);
    // Compare raw x86-64 access ABI results, including error ordering.
    auto native_access = [](int dirfd, const char* path, int mode, int flags) {
      errno = 0;
      const long value = syscall(SYS_faccessat2, dirfd, path, mode, flags);
      return value < 0 ? -long(errno) : value;
    };
    const int host_directory = open(root.c_str(), O_DIRECTORY | O_RDONLY);
    CHECK(host_directory >= 0);
    auto& access_fs = ptmc_state.fs_states[0];
    for (const auto& relative : {"regular", "missing", "regular/child", "missing/../regular", "real/../regular", ""}) {
      CHECK(native_access(host_directory, relative, F_OK, 0) ==
            emu_vfs_accessat(directory, relative, F_OK, 0));
    }
    CHECK(native_access(-7, (root / "regular").c_str(), F_OK, 0) ==
          emu_vfs_accessat(-7, "/fixture/regular", F_OK, 0));
    CHECK(native_access(-7, "regular", F_OK, 0) == emu_vfs_accessat(-7, "regular", F_OK, 0));
    CHECK(native_access(host_directory, "dangling", F_OK, AT_SYMLINK_NOFOLLOW) ==
          emu_vfs_accessat(directory, "dangling", F_OK, AT_SYMLINK_NOFOLLOW));
    for (int flags : {0, AT_EACCESS, AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH}) {
      CHECK(native_access(host_directory, "regular", F_OK, flags) ==
            emu_vfs_accessat(directory, "regular", F_OK, flags));
    }
    CHECK(native_access(host_directory, "", F_OK, AT_EMPTY_PATH) ==
          emu_vfs_accessat(directory, "", F_OK, AT_EMPTY_PATH));
    CHECK(native_access(-7, "", F_OK, AT_EMPTY_PATH) ==
          emu_vfs_accessat(-7, "", F_OK, AT_EMPTY_PATH));
    for (int mode : {-1, 8}) {
      CHECK(native_access(-7, nullptr, mode, 0) == emu_vfs_accessat(-7, nullptr, mode, 0));
    }
    CHECK(native_access(-7, nullptr, F_OK, 0x40000000) ==
          emu_vfs_accessat(-7, nullptr, F_OK, 0x40000000));
    CHECK(native_access(host_directory, nullptr, F_OK, 0) ==
          emu_vfs_accessat(directory, nullptr, F_OK, 0));
    for (const auto& path : {"/fixture/link", "/fixture/dangling", "/fixture/link/../regular"}) {
      bool rejected = false;
      try { emu_vfs_accessat(AT_FDCWD, path, F_OK, 0); }
      catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
    }
    for (int mode : {R_OK, W_OK, X_OK, R_OK | W_OK | X_OK}) {
      bool rejected = false;
      try { emu_vfs_accessat(AT_FDCWD, "/fixture/regular", mode, 0); }
      catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
    }
    access_fs.set_cwd("/fixture");
    CHECK(emu_vfs_accessat(AT_FDCWD, "regular", F_OK, 0) == 0);
    CHECK(emu_vfs_accessat(AT_FDCWD, "", F_OK, AT_EMPTY_PATH) == 0);
    access_fs.filesystem.at("/fixture/real").metadata.st_mode = S_IFDIR | 0700;
    bool search_rejected = false;
    try { emu_vfs_accessat(AT_FDCWD, "/fixture/real/../regular", F_OK, 0); }
    catch (const std::runtime_error&) { search_rejected = true; }
    CHECK(search_rejected);
    CHECK(emu_vfs_accessat(AT_FDCWD, "/fixture/real", F_OK, 0) == 0);
    const int host_regular = open((root / "regular").c_str(), O_RDONLY);
    const int guest_regular = access_fs.do_open("/fixture/regular", O_RDONLY, 0);
    CHECK(host_regular >= 0 && guest_regular >= 0);
    CHECK(native_access(host_regular, "child", F_OK, 0) ==
          emu_vfs_accessat(guest_regular, "child", F_OK, 0));
    CHECK(native_access(host_regular, "", F_OK, AT_EMPTY_PATH) ==
          emu_vfs_accessat(guest_regular, "", F_OK, AT_EMPTY_PATH));
    CHECK(syscall(SYS_access, (root / "regular").c_str(), F_OK) ==
          emu_vfs_accessat(AT_FDCWD, "/fixture/regular", F_OK, 0));
    CHECK(syscall(SYS_faccessat, host_directory, "regular", F_OK) ==
          emu_vfs_accessat(directory, "regular", F_OK, 0));
    CHECK(close(host_regular) == 0 && close(host_directory) == 0);
    // All queries and bytes remain independent of host deletion and branch edits.
    std::stringstream saved;
    { cereal::BinaryOutputArchive ar(saved); ar(single); }
    FileSystemState archived;
    { cereal::BinaryInputArchive ar(saved); ar(archived); }
    CHECK(unlink((root / ".hidden").c_str()) == 0);
    CHECK(unlink((root / "dangling").c_str()) == 0);
    CHECK(archived.do_access(AT_FDCWD, "/nested/file", F_OK, 0) == 0);
    CHECK(archived.filesystem.at("/nested/file").content == std::vector<char>({'a', '\0', 'b'}));
    FileSystemState branch = archived;
    branch.filesystem.at("/nested/file").content.write(0, "x", 1);
    branch.filesystem.erase("/nested/link");
    CHECK(archived.do_access(AT_FDCWD, "/nested/link", F_OK, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(branch.do_access(AT_FDCWD, "/nested/link", F_OK, AT_SYMLINK_NOFOLLOW) == -ENOENT);
    CHECK(archived.filesystem.at("/nested/file").content[0] == 'a');
    auto manager = std::make_shared<FdManager>();
    FileSystemState fs(manager);
    char buffer[8]{};
    CHECK(fs.do_read(0, buffer, sizeof(buffer)) == 0);
    CHECK(fs.do_write(0, "x", 1) == -EBADF);
    CHECK(fs.do_read(1, buffer, 1) == -EBADF);
    CHECK(fs.do_write(1, "ab", 2) == 2);
    CHECK(fs.do_write(1, "c", 1) == 1);
    CHECK(fs.filesystem.at(fs.open_files.at(1).path).content == std::vector<char>({'a','b','c'}));
    struct stat statbuf{};
    CHECK(fs.do_fstat(1, &statbuf) == 0 && S_ISCHR(statbuf.st_mode));
    CHECK(fs.do_lseek(1, 0, SEEK_SET) == -ESPIPE);

    const int fd = fs.do_open("/file", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd == 3);
    CHECK(manager->get_descriptor_flags(fd) == FD_CLOEXEC);
    CHECK(manager->get_status_flags(fd) == O_RDWR);
    CHECK(fs.do_write(fd, "ab", 2) == 2);
    CHECK(fs.do_lseek(fd, 0, SEEK_SET) == 0);
    CHECK(manager->do_fcntl(fd, F_SETFL, O_APPEND) == 0);
    CHECK(fs.do_write(fd, "c", 1) == 1);
    CHECK(fs.filesystem.at("/file").content == std::vector<char>({'a','b','c'}));
    CHECK(fs.do_lseek(fd, 100, SEEK_SET) == 100 && fs.do_read(fd, buffer, 1) == 0);
    CHECK(fs.do_lseek(fd, std::numeric_limits<off_t>::max(), SEEK_SET) == std::numeric_limits<off_t>::max());
    CHECK(fs.do_lseek(fd, 1, SEEK_CUR) == -EOVERFLOW);
    CHECK(fs.do_close(fd) == 0 && !manager->is_allocated(fd));
    CHECK(fs.do_open("/file", O_RDWR | O_TRUNC, 0) == fd);
    CHECK(fs.filesystem.at("/file").content.empty());

    int pipefd[2];
    CHECK(fs.do_pipe(pipefd, O_APPEND) == -EINVAL && fs.pipes_.empty());
    CHECK(fs.do_pipe(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    CHECK(manager->get_status_flags(pipefd[0]) == (O_RDONLY | O_NONBLOCK));
    CHECK(manager->get_status_flags(pipefd[1]) == (O_WRONLY | O_NONBLOCK));
    CHECK(manager->get_descriptor_flags(pipefd[0]) == FD_CLOEXEC);
    CHECK(fs.do_pipe_read(pipefd[0], nullptr, 0) == 0);
    CHECK(fs.do_pipe_write(pipefd[1], nullptr, 0) == 0);
    fs.pipes_.begin()->second.buffer.push_back('x');
    CHECK(fs.do_close(1) == 0);
    std::stringstream bytes;
    { cereal::BinaryOutputArchive ar(bytes); ar(fs, *manager); }
    FileSystemState restored;
    auto restored_manager = std::make_shared<FdManager>();
    { cereal::BinaryInputArchive ar(bytes); ar(restored, *restored_manager); }
    restored.set_fd_manager(restored_manager);
    CHECK(!restored.open_files.contains(1) && !restored_manager->is_allocated(1));
    CHECK(restored.do_write(1, "x", 1) == -EBADF);
    CHECK(restored.pipes_.begin()->second.buffer.front() == 'x');
    restored.pipes_.begin()->second.buffer.pop_front();
    CHECK(fs.pipes_.begin()->second.buffer.front() == 'x');
    int next_pipe[2];
    CHECK(restored.do_pipe(next_pipe, 0) == 0);
    CHECK(restored.pipes_.size() == 2 && restored.pipe_fds_.at(next_pipe[0]) == 2);
    CHECK(restored.do_pipe_close(pipefd[0]) == 0);
    CHECK(restored.do_pipe_close(pipefd[1]) == 0);
    CHECK(restored.do_pipe(next_pipe, 0) == 0 && restored.pipe_fds_.at(next_pipe[0]) == 1);
    std::cout << "PASS FS stdio, flags, offsets, pipe archive and branch isolation\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
