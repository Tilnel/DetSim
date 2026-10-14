#include "subsys/fs/fd_manager.h"
#include "subsys/sync/futexstate.h"

#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/vector.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sstream>

// Unlike assert(), checks remain active with -DNDEBUG.
#define CHECK(condition)                                                       \
  do                                                                           \
  {                                                                            \
    if (!(condition))                                                          \
    {                                                                          \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition " failed\n"; \
      std::exit(EXIT_FAILURE);                                                 \
    }                                                                          \
  } while (false)

static void fd_snapshot_and_copy()
{
  FdManager original;
  const int file = original.allocate_fd(FdType::FILE);
  const int socket = original.allocate_fd(FdType::SOCKET);
  CHECK(file >= 3);
  CHECK(socket != file);
  CHECK(original.is_allocated(file));
  CHECK(original.is_allocated(socket));
  CHECK(original.get_fd_type(file) == FdType::FILE);
  CHECK(original.get_fd_type(socket) == FdType::SOCKET);
  original.release_fd(file);
  CHECK(!original.is_allocated(file));

  std::stringstream bytes;
  {
    cereal::BinaryOutputArchive archive(bytes);
    archive(original);
  }
  FdManager restored;
  {
    cereal::BinaryInputArchive archive(bytes);
    archive(restored);
  }
  CHECK(restored == original);
  CHECK(!restored.is_allocated(file));
  CHECK(restored.is_allocated(socket));
  const int next = restored.allocate_fd(FdType::PIPE);
  CHECK(next == file);
  CHECK(next == original.get_next_fd());
  CHECK(!original.is_allocated(next));

  FdManager copy(original);
  copy.release_fd(socket);
  CHECK(original.is_allocated(socket));
  CHECK(!copy.is_allocated(socket));
  FdManager assigned;
  assigned.allocate_fd(FdType::EPOLL);
  assigned = original;
  CHECK(assigned == original);
  assigned.release_fd(socket);
  CHECK(original.is_allocated(socket));
  CHECK(!assigned.is_allocated(socket));
}

static void fd_flags_and_stdio()
{
  FdManager manager;
  CHECK(manager.get_next_fd() == 3);
  CHECK(manager.get_fd_type(0) == FdType::STDIO);
  CHECK(manager.get_status_flags(0) == O_RDONLY);
  CHECK(manager.get_status_flags(1) == O_WRONLY);
  manager.release_fd(0);
  const int fd = manager.allocate_fd(FdType::SOCKET, O_RDWR | O_NONBLOCK,
                                     FD_CLOEXEC);
  CHECK(fd == 0);
  CHECK(manager.do_fcntl(fd, F_GETFD, 0) == FD_CLOEXEC);
  CHECK(manager.do_fcntl(fd, F_SETFL, O_RDONLY | O_APPEND | O_CREAT) == 0);
  CHECK(manager.get_status_flags(fd) == (O_RDWR | O_APPEND));
  CHECK(manager.do_fcntl(fd, F_SETFD, 0x100) == 0);
  CHECK(manager.get_descriptor_flags(fd) == 0);
  CHECK(manager.do_fcntl(100, F_SETFL, O_APPEND) == -EBADF);
  CHECK(manager.get_status_flags(100) == -EBADF);
  bool rejected = false;
  try { manager.do_fcntl(fd, F_SETFL, O_ASYNC); }
  catch (const std::runtime_error &) { rejected = true; }
  CHECK(rejected);
  CHECK(manager.get_status_flags(fd) == (O_RDWR | O_APPEND));
  rejected = false;
  try { manager.do_fcntl(fd, F_DUPFD, 10); }
  catch (const std::runtime_error &) { rejected = true; }
  CHECK(rejected);

  FdManager copy = manager;
  CHECK(copy.do_fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
  CHECK(copy != manager);
  CHECK(manager.get_status_flags(fd) == (O_RDWR | O_APPEND));
  CHECK(copy.do_fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
  std::stringstream bytes;
  { cereal::BinaryOutputArchive ar(bytes); ar(copy); }
  FdManager restored;
  { cereal::BinaryInputArchive ar(bytes); ar(restored); }
  CHECK(restored == copy);
  CHECK(restored.get_fd_type(fd) == FdType::SOCKET);
  CHECK(restored.get_status_flags(fd) == (O_RDWR | O_NONBLOCK));
  CHECK(restored.get_descriptor_flags(fd) == FD_CLOEXEC);
}

static void fd_archive_validation()
{
  for (int variant = 0; variant < 6; ++variant)
  {
    std::stringstream bytes;
    {
      cereal::BinaryOutputArchive ar(bytes);
      uint64_t count = variant == 4 ? (1U << 20) + 1 :
                       variant == 5 ? 2 : 1;
      ar(count);
      if (variant != 4)
      {
        int fd = variant == 0 ? -1 : 3;
        FdType kind = variant == 1 ? FdType::UNKNOWN : FdType::FILE;
        int status = variant == 2 ? O_ASYNC : O_RDONLY;
        int descriptor = variant == 3 ? 0x100 : 0;
        ar(fd, kind, status, descriptor);
        if (variant == 5) ar(fd, kind, status, descriptor);
      }
    }
    FdManager restored;
    const FdManager before = restored;
    bool rejected = false;
    try { cereal::BinaryInputArchive ar(bytes); ar(restored); }
    catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected);
    CHECK(restored == before);
  }
}

static void wait(FutexState &state, pid_t tid, uint64_t address,
                 int bitset = -1)
{
  uint64_t value = 7;
  CHECK(state.handle_futex(tid, address, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                           7, 0, 0, bitset, &value) == 0);
  CHECK(state.is_thread_waiting(tid));
  CHECK(state.get_wait_address(tid) == address);
}

static void futex_wait_and_wake()
{
  FutexState state;
  constexpr uint64_t address = 0x1000;
  uint64_t mismatch = 8;
  CHECK(state.handle_futex(1, address, FUTEX_WAIT, 7, 0, 0, 0, &mismatch) ==
        -EAGAIN);
  CHECK(!state.is_thread_waiting(1));
  CHECK(state.get_waiters(address).empty());
  CHECK(state.handle_futex(1, address, FUTEX_WAIT_BITSET, 7, 0, 0, 0) ==
        -EINVAL);

  wait(state, 1, address, 1);
  wait(state, 2, address, 2);
  wait(state, 3, address, 1);
  CHECK((state.get_waiters(address) == std::vector<pid_t>{1, 2, 3}));
  CHECK(state.futex_wake(address, 1, 2) == 1);
  CHECK(!state.is_thread_waiting(2));
  CHECK((state.get_waiters(address) == std::vector<pid_t>{1, 3}));
  CHECK(state.futex_wake(address, 1) == 1);
  CHECK(!state.is_thread_waiting(1));
  CHECK(state.is_thread_waiting(3));
  CHECK(state.wake_thread(3));
  CHECK(!state.wake_thread(3));
  CHECK(state.get_waiters(address).empty());
  CHECK(state.futex_wake(address, 10) == 0);
}

static void futex_snapshot_and_copy()
{
  FutexState original;
  wait(original, 1, 0x1000, 1);
  wait(original, 2, 0x1000, 2);
  wait(original, 3, 0x2000);
  FutexState copy(original);
  CHECK(copy.wake_thread(1));
  CHECK(original.is_thread_waiting(1));
  CHECK(!copy.is_thread_waiting(1));

  std::stringstream bytes;
  {
    cereal::BinaryOutputArchive archive(bytes);
    archive(original);
  }
  FutexState restored;
  wait(restored, 99, 0x3000);
  {
    cereal::BinaryInputArchive archive(bytes);
    archive(restored);
  }
  CHECK(!restored.is_thread_waiting(99));
  CHECK(restored.get_waiters(0x3000).empty());
  CHECK((restored.get_waiters(0x1000) == std::vector<pid_t>{1, 2}));
  CHECK((restored.get_waiters(0x2000) == std::vector<pid_t>{3}));
  CHECK(restored.get_wait_address(1) == 0x1000);
  CHECK(restored.get_wait_address(3) == 0x2000);
  CHECK(restored.futex_wake(0x1000, 2, 2) == 1);
  CHECK(restored.is_thread_waiting(1));
  CHECK(!restored.is_thread_waiting(2));
  CHECK(original.is_thread_waiting(2));
  restored.clear();
  CHECK(!restored.is_thread_waiting(1));
  CHECK(!restored.is_thread_waiting(3));
  CHECK(restored.get_waiters(0x1000).empty());
  CHECK(restored.get_waiters(0x2000).empty());
}

static void futex_requeue_and_range()
{
  FutexState state;
  wait(state, 1, 0x1000);
  wait(state, 2, 0x1000);
  wait(state, 3, 0x1000);
  // Exercise the dispatcher path; the direct helper is covered separately.
  CHECK(state.handle_futex(9, 0x1000, FUTEX_REQUEUE, 1, 1, 0x2000, 99) == 2);
  CHECK(!state.is_thread_waiting(1));
  CHECK(state.get_wait_address(2) == 0x2000);
  CHECK((state.get_waiters(0x1000) == std::vector<pid_t>{3}));
  CHECK((state.get_waiters(0x2000) == std::vector<pid_t>{2}));
  CHECK(state.wake_waiters_in_range(0x1000, 0x2000) == 1);
  CHECK(!state.is_thread_waiting(3));
  CHECK(state.is_thread_waiting(2));
  CHECK(state.wake_waiters_in_range(0x2000, 0x2001) == 1);
  CHECK(!state.is_thread_waiting(2));
  CHECK(state.get_waiters(0x1000).empty());
  CHECK(state.get_waiters(0x2000).empty());
}

static void futex_requeue_index_and_archive()
{
  // Both requeue entry points must preserve the queue/index invariant.
  FutexState direct;
  FutexState dispatched;
  for (pid_t tid : {1, 2, 3})
  {
    wait(direct, tid, 0x1000);
    wait(dispatched, tid, 0x1000);
  }
  CHECK(direct.futex_requeue(0x1000, 0x2000, 1, 1) == 2);
  CHECK(dispatched.handle_futex(9, 0x1000, FUTEX_REQUEUE, 1, 1, 0x2000, 99) ==
        2);
  CHECK(direct.get_wait_address(2) == 0x2000);
  CHECK(direct.get_wait_address(2) == dispatched.get_wait_address(2));
  CHECK(direct.get_waiters(0x1000) == dispatched.get_waiters(0x1000));
  CHECK(direct.get_waiters(0x2000) == dispatched.get_waiters(0x2000));
  std::stringstream bytes;
  {
    cereal::BinaryOutputArchive archive(bytes);
    archive(direct);
  }
  FutexState restored;
  {
    cereal::BinaryInputArchive archive(bytes);
    archive(restored);
  }
  CHECK(restored.get_wait_address(2) == 0x2000);
  CHECK(restored.wake_thread(2));
  CHECK(!restored.is_thread_waiting(2));
  CHECK(restored.get_waiters(0x2000).empty());
  CHECK(restored.is_thread_waiting(3));
  CHECK(direct.is_thread_waiting(2));
}

static void futex_requeue_validation()
{
  FutexState state;
  for (pid_t tid : {1, 2, 3}) wait(state, tid, 0x1000);
  uint64_t value = 7;
  CHECK(state.handle_futex(9, 0x1000, FUTEX_CMP_REQUEUE, 1, 1, 0x2000, 8, &value) == -EAGAIN);
  CHECK(state.get_waiters(0x1000).size() == 3);
  CHECK(state.futex_requeue(0x1000, 0x2000, -1, 1) == -EINVAL);
  CHECK(state.futex_requeue(0x1000, 0x2001, 1, 1) == -EINVAL);
  CHECK(state.futex_requeue(0x1000, 0x2000, 1, -1) == -EINVAL);
  CHECK(state.get_waiters(0x1000).size() == 3);
  CHECK(state.futex_requeue(0x1000, 0x1000, 1, 2) == 3);
  CHECK((state.get_waiters(0x1000) == std::vector<pid_t>{2, 3}));
  CHECK(state.get_wait_address(2) == 0x1000);
  CHECK(state.handle_futex(9, 0x1000, FUTEX_CMP_REQUEUE, 0, 1, 0x2000, 7, &value) == 1);
  CHECK(state.get_wait_address(2) == 0x2000);
  CHECK(state.get_wait_address(3) == 0x1000);
  value = 0xffffffff;
  CHECK(state.handle_futex(4, 0x3000, FUTEX_WAIT, -1, 0, 0, 0, &value) == 0);
  CHECK(state.handle_futex(5, 0x3001, FUTEX_WAIT, -1, 0, 0, 0, &value) == -EINVAL);
}

int main()
{
  futex_requeue_validation();
  fd_snapshot_and_copy();
  fd_flags_and_stdio();
  fd_archive_validation();
  futex_wait_and_wake();
  futex_snapshot_and_copy();
  futex_requeue_and_range();
  futex_requeue_index_and_archive();
  std::cout << "PASS: 8 state component cases (allocation, flags, copy, archive, "
               "wait/wake/requeue/range)\n";
}
