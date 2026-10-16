#include "pending_io.h"
#include "monitor.h"
#include "guest.h"
#include "io.h"
#include <cerrno>
#include <algorithm>
#include <vector>
#include <fcntl.h>
#include <sys/syscall.h>
#include <stdexcept>
#include <time.h>

namespace {
bool epoll_call(int nr) {
  return nr == SYS_epoll_wait || nr == SYS_epoll_pwait || nr == SYS_epoll_pwait2;
}
bool data_call(int nr) {
  return nr == SYS_read || nr == SYS_write || nr == SYS_recvfrom ||
         nr == SYS_accept || nr == SYS_accept4;
}
}

bool supports_pending_io(int nr) { return epoll_call(nr) || data_call(nr); }

bool io_should_suspend(int node, const syscall_info& call, long result) {
  if (epoll_call(call.nr)) {
    if (result != 0) return false;
    if (call.nr != SYS_epoll_pwait2) {
      const int timeout = static_cast<int>(call.args[3]);
      return timeout != 0;
    }
    if (!call.args[3]) return true;
    timespec timeout{};
    if (!guest_read_exact(ptmc_state.pids[node], reinterpret_cast<void*>(call.args[3]),
                          &timeout, sizeof(timeout)))
      throw std::runtime_error("Validated epoll timeout became unreadable");
    return timeout.tv_sec || timeout.tv_nsec;
  }
  if (!data_call(call.nr) || result != -EAGAIN) return false;
  const int fd = static_cast<int>(call.args[0]);
  const auto& manager = ptmc_state.fd_managers[node];
  if (!manager || !manager->is_allocated(fd)) return false;
  if (manager->get_status_flags(fd) & O_NONBLOCK) return false;
  return call.nr != SYS_recvfrom || !(call.args[3] & MSG_DONTWAIT);
}

void configure_pending_io(int node, pid_t tid, const syscall_info& call) {
  if (!epoll_call(call.nr)) return;
  int64_t seconds = 0;
  int32_t nanoseconds = 0;
  if (call.nr == SYS_epoll_pwait2) {
    if (!call.args[3]) return;
    timespec timeout{};
    if (!guest_read_exact(ptmc_state.pids[node], reinterpret_cast<void*>(call.args[3]),
                          &timeout, sizeof(timeout)))
      throw std::runtime_error("Validated epoll timeout became unreadable");
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
      throw std::runtime_error("Invalid captured epoll timeout");
    seconds = timeout.tv_sec;
    nanoseconds = timeout.tv_nsec;
  } else {
    const int milliseconds = static_cast<int>(call.args[3]);
    if (milliseconds < 0) return;
    seconds = milliseconds / 1000;
    nanoseconds = (milliseconds % 1000) * 1000000;
  }
  const auto deadline = make_pending_deadline(node, CLOCK_MONOTONIC, seconds,
                                              nanoseconds, true, 0);
  for (const auto& thread : ptmc_state.running_state.child[node].threads) {
    if (thread.alive && thread.physical_tid == tid) {
      set_pending_deadline(node, thread.tid, deadline);
      return;
    }
  }
  throw std::runtime_error("Pending epoll deadline has no live bound actor");
}

static bool io_retry_ready(const FdManager& manager, const FileSystemState& fs,
                           const SockState& network, const syscall_info& call) {
  const int fd = static_cast<int>(call.args[0]);
  if (!manager.is_allocated(fd)) return true;
  if (epoll_call(call.nr)) return network.epoll_ready(fd, fs);
  if (!data_call(call.nr)) throw std::runtime_error("Unknown pending I/O syscall");
  if (manager.get_fd_type(fd) == FdType::PIPE) {
    const auto descriptor = fs.pipe_fds_.find(fd);
    if (descriptor == fs.pipe_fds_.end()) return true;
    const auto pipe = fs.pipes_.find(std::abs(descriptor->second));
    if (pipe == fs.pipes_.end()) return true;
    if (call.nr == SYS_write) {
      if (descriptor->second > 0 || pipe->second.read_closed) return true;
      const size_t available = FileSystemState::pipe_capacity - pipe->second.buffer.size();
      return call.args[2] <= FileSystemState::pipe_atomic_limit
                 ? available >= call.args[2] : available > 0;
    }
    return descriptor->second < 0 || !pipe->second.buffer.empty() || pipe->second.write_closed;
  }
  if (manager.get_fd_type(fd) != FdType::SOCKET) return true;
  return network.ready_events(fd, fs) & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

bool io_retry_ready(int node, const syscall_info& call) {
  const auto& manager = ptmc_state.fd_managers[node];
  return !manager || io_retry_ready(*manager, ptmc_state.fs_states[node], ptmc_state.sock_states[node], call);
}
bool io_retry_ready(const tracee_state& source, const syscall_info& call) {
  return io_retry_ready(source.fd_manager_state, source.fs_state, source.sock_state, call);
}

bool retry_pending_io(int node, pid_t tid, syscall_info& call) {
  // The execution layer selects the actor before retry. Never resume a carrier.
  if (ptmc_state.cursor != node || ptmc_state.get_current_tid(node) != tid)
    throw std::runtime_error("Pending I/O retry actor mismatch");
  const auto& a = call.args;
  long result;
  switch (call.nr) {
    case SYS_read: result = emu_read(a[0], reinterpret_cast<void*>(a[1]), a[2]); break;
    case SYS_write: result = emu_write(a[0], reinterpret_cast<void*>(a[1]), a[2]); break;
    case SYS_recvfrom:
      result = ptmc_state.sock_states[node].do_recvfrom(a[0], reinterpret_cast<void*>(a[1]),
          a[2], a[3], reinterpret_cast<sockaddr*>(a[4]), reinterpret_cast<socklen_t*>(a[5]));
      break;
    case SYS_accept: result = emu_accept(a[0], reinterpret_cast<sockaddr*>(a[1]), reinterpret_cast<socklen_t*>(a[2])); break;
    case SYS_accept4: result = emu_accept4(a[0], reinterpret_cast<sockaddr*>(a[1]), reinterpret_cast<socklen_t*>(a[2]), a[3]); break;
    case SYS_epoll_wait:
    case SYS_epoll_pwait:
    case SYS_epoll_pwait2:
      result = emu_epoll_wait(a[0], reinterpret_cast<epoll_event*>(a[1]), a[2], 0);
      if (!result) return false;
      call.rval = result;
      return true;
    default: throw std::runtime_error("Unsupported pending I/O retry");
  }
  if (io_should_suspend(node, call, result)) return false;
  call.rval = result;
  return true;
}

void require_no_pending_fd(int node, int fd) {
  for (const auto& thread : ptmc_state.running_state.child[node].threads) {
    if (!thread.alive || !thread.pending_syscall) continue;
    const auto& pending = *thread.pending_syscall;
    if (pending.reason == PendingReason::Io && pending.phase != PendingPhase::Ready &&
        static_cast<int>(pending.args[0]) == fd)
      throw std::runtime_error("Mutating a descriptor retained by pending I/O is unsupported");
  }
}

void preflight_process_io_exit(int node) {
  if (node < 0 || node >= NP) throw std::runtime_error("I/O exit node out of range");
  const auto& network = ptmc_state.sock_states[node];
  const auto& manager = ptmc_state.fd_managers[node];
  if (!manager) {
    const auto& fs = ptmc_state.fs_states[node];
    if (!network.sockets().empty() || !network.epoll_instances().empty() ||
        !fs.pipe_fds_.empty() || !fs.pipes_.empty() ||
        std::any_of(fs.open_files.begin(), fs.open_files.end(),
                    [](const auto& entry) { return entry.first < 0 || entry.first > 2; }))
      throw std::runtime_error("Process I/O exit has resources without a descriptor manager");
    return; // Uninitialized component fixtures have only default stdio payloads.
  }
  for (const auto& [fd, socket] : network.sockets()) {
    if (socket.type != SOCK_STREAM) continue;
    if (socket.listening && (!socket.pending_connections.empty() ||
                             !socket.established_connections.empty()))
      throw std::runtime_error("Process exit with unaccepted TCP connections requires reset semantics");
    if (!socket.connected) continue;
    const auto connection = network.tcp_connections().find(fd);
    if (connection == network.tcp_connections().end())
      throw std::runtime_error("Connected TCP socket has no connection payload at process exit");
    if (std::any_of(connection->second.recv_buffer.begin(), connection->second.recv_buffer.end(),
                    [](const auto& message) { return !message.empty(); }))
      throw std::runtime_error("Process exit with unread TCP data requires reset semantics");
  }
}

void close_process_io(int node) {
  if (node < 0 || node >= NP) throw std::runtime_error("Process I/O close node out of range");
  for (const auto& thread : ptmc_state.running_state.child[node].threads)
    if (thread.alive) throw std::runtime_error("Cannot close process I/O with live logical tasks");
  preflight_process_io_exit(node);
  const auto& manager = ptmc_state.fd_managers[node];
  if (!manager) return;
  struct CursorGuard {
    int previous = ptmc_state.cursor;
    ~CursorGuard() { ptmc_state.cursor = previous; }
  } guard;
  ptmc_state.cursor = node;
  const auto owned = manager->get_allocated_fds();
  std::vector<int> descriptors(owned.begin(), owned.end());
  std::sort(descriptors.begin(), descriptors.end());
  for (const int fd : descriptors)
    if (emu_close(fd) != 0) throw std::runtime_error("Virtual descriptor close failed during process exit");
}

void start_pselect_timer(int node, pid_t tid, syscall_info& call) {
  // pselect6 imports its optional mask wrapper before inspecting the timeout.
  // Temporary signal masks and descriptor multiplexing have no model yet.
  if (call.args[5]) throw std::runtime_error("Unsupported pselect6 signal mask");
  if (call.args[4] < scratch_page + 4096 && call.args[4] > scratch_page - sizeof(timespec))
    throw std::runtime_error("pselect6 timeout overlaps reserved control memory");
  timespec timeout{};
  long result = 0;
  if (call.args[4] && !guest_read_exact(tid, reinterpret_cast<void*>(call.args[4]),
                                       &timeout, sizeof(timeout))) result = -EFAULT;
  else if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
    result = -EINVAL;
  else if (static_cast<int>(call.args[0]) < 0) result = -EINVAL;
  else if (static_cast<int>(call.args[0]) != 0)
    throw std::runtime_error("Unsupported pselect6 descriptor multiplexing");
  if (result || (call.args[4] && !timeout.tv_sec && !timeout.tv_nsec)) {
    call.rval = result;
    tracee_set_rax(tid, call.rval);
    return;
  }
  // nfds=0 ignores all three descriptor pointers, including invalid addresses.
  suspend_syscall(node, tid, call, PendingReason::Timer);
  if (!call.args[4]) return; // Infinite sleep; only explicit external grant can finish it.
  const auto deadline = make_pending_deadline(node, CLOCK_MONOTONIC,
      timeout.tv_sec, timeout.tv_nsec, true, 0);
  for (const auto& thread : ptmc_state.running_state.child[node].threads) {
    if (thread.alive && thread.physical_tid == tid) {
      set_pending_deadline(node, thread.tid, deadline);
      return;
    }
  }
  throw std::runtime_error("Pending pselect6 has no live bound actor");
}
