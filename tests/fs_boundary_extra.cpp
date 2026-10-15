#include "fsstate.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
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
    FileSystemState imported(std::make_shared<FdManager>());
    imported.init_from_mappings({{root.string(), "/fixture"}});
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/link").metadata.st_mode));
    CHECK(imported.filesystem.at("/fixture/link").content == std::vector<char>({'r','e','g','u','l','a','r'}));
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/dangling").metadata.st_mode));
    CHECK(S_ISLNK(imported.filesystem.at("/fixture/loop").metadata.st_mode));
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
    link_restore.filesystem.at("/fixture/link").content[0] = 'X';
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
