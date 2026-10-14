#include "fsstate.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <fcntl.h>
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
