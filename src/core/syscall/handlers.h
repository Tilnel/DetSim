/*
 * handlers.h - Individual syscall handler namespaces
 *
 * Each namespace contains handlers for a category of syscalls.
 * All handlers return checkpoint decision codes.
 */

#ifndef __SYSCALL_HANDLERS_H
#define __SYSCALL_HANDLERS_H

#include <sys/types.h>

// Forward declarations
struct syscall_info;

// Checkpoint return codes
#define CKPT_YES 1
#define CKPT_NO 0
#define CKPT_EXIT 2
#define CKPT_STOP 3
#define CKPT_DISCARD 4

/* ======================================================================
 * Network Handlers
 * ====================================================================== */

namespace handlers {
namespace network {

// Main handler for network syscalls
int handle(pid_t pid, syscall_info &info);

// Individual handlers
int handle_sendto(pid_t pid, syscall_info &info);
int handle_recvfrom(pid_t pid, syscall_info &info);
int handle_socket(pid_t pid, syscall_info &info);
int handle_connect(pid_t pid, syscall_info &info);
int handle_bind(pid_t pid, syscall_info &info);
int handle_listen(pid_t pid, syscall_info &info);
int handle_accept(pid_t pid, syscall_info &info);
int handle_setsockopt(pid_t pid, syscall_info &info);
int handle_getsockopt(pid_t pid, syscall_info &info);
int handle_getsockname(pid_t pid, syscall_info &info);
int handle_getpeername(pid_t pid, syscall_info &info);
int handle_poll(pid_t pid, syscall_info &info);
int handle_epoll_create(pid_t pid, syscall_info &info);
int handle_epoll_create1(pid_t pid, syscall_info &info);
int handle_epoll_ctl(pid_t pid, syscall_info &info);
int handle_epoll_wait(pid_t pid, syscall_info &info);
int handle_epoll_pwait(pid_t pid, syscall_info &info);
int handle_epoll_pwait2(pid_t pid, syscall_info &info);

} // namespace network
} // namespace handlers

/* ======================================================================
 * Filesystem Handlers
 * ====================================================================== */

namespace handlers {
namespace fs {

// Main handler for filesystem syscalls
int handle(pid_t pid, syscall_info &info);

// Individual handlers
int handle_open(pid_t pid, syscall_info &info);
int handle_openat(pid_t pid, syscall_info &info);
int handle_read(pid_t pid, syscall_info &info);
int handle_write(pid_t pid, syscall_info &info);
int handle_writev(pid_t pid, syscall_info &info);
int handle_close(pid_t pid, syscall_info &info);
int handle_lseek(pid_t pid, syscall_info &info);
int handle_stat(pid_t pid, syscall_info &info);
int handle_fstat(pid_t pid, syscall_info &info);
int handle_chdir(pid_t pid, syscall_info &info);
int handle_pipe(pid_t pid, syscall_info &info);
int handle_mmap(pid_t pid, syscall_info &info);
int handle_munmap(pid_t pid, syscall_info &info);
int handle_madvise(pid_t pid, syscall_info &info);
int handle_fsync(pid_t pid, syscall_info &info);
int handle_fdatasync(pid_t pid, syscall_info &info);
int handle_fcntl(pid_t pid, syscall_info &info);
int handle_ioctl(pid_t pid, syscall_info &info);
int handle_statat(pid_t pid, syscall_info &info);
int handle_getcwd(pid_t pid, syscall_info &info);

} // namespace fs
} // namespace handlers

/* ======================================================================
 * Thread Handlers
 * ====================================================================== */

namespace handlers {
namespace thread {

// Main handler for thread-related syscalls
int handle(pid_t pid, syscall_info &info);

// Individual handlers
int handle_clone(pid_t pid, syscall_info &info);
int handle_clone3(pid_t pid, syscall_info &info);
int handle_gettid(pid_t pid, syscall_info &info);
int handle_exit(pid_t pid, syscall_info &info);
int handle_exit_group(pid_t pid, syscall_info &info);

} // namespace thread
} // namespace handlers

/* ======================================================================
 * Futex Handlers
 * ====================================================================== */

namespace handlers {
namespace futex {

// Main handler for futex syscalls
int handle(pid_t pid, syscall_info &info);

} // namespace futex
} // namespace handlers

/* ======================================================================
 * Time Handlers
 * ====================================================================== */

namespace handlers {
namespace time {

// Main handler for time syscalls
int handle(pid_t pid, syscall_info &info);

// Individual handlers
int handle_gettimeofday(pid_t pid, syscall_info &info);
int handle_nanosleep(pid_t pid, syscall_info &info);
int handle_clock_nanosleep(pid_t pid, syscall_info &info);

} // namespace time
} // namespace handlers

#endif /* __SYSCALL_HANDLERS_H */
