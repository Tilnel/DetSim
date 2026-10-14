/*
 * handlers.cpp - Individual syscall handler implementations
 */

#include "handlers.h"
#include "guest.h"
#include "log_wrapper.h"
#include "monitor.h"
#include "proc_status.h"
#include "state/state.h"
#include "thread_manager.h"
#include "sync/futexstate.h"
#include "fs/fsstate.h"
#include "net/sockstate.h"
#include "net/emu.h"
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <fcntl.h>

/* ======================================================================
 * Network Handlers
 * ====================================================================== */

namespace handlers {
namespace network {

int handle(pid_t pid, syscall_info &info) {
    switch (info.nr) {
        case SYS_sendto:      return handle_sendto(pid, info);
        case SYS_recvfrom:    return handle_recvfrom(pid, info);
        case SYS_socket:      return handle_socket(pid, info);
        case SYS_connect:     return handle_connect(pid, info);
        case SYS_bind:        return handle_bind(pid, info);
        case SYS_listen:      return handle_listen(pid, info);
        case SYS_accept:
        case SYS_accept4:     return handle_accept(pid, info);
        case SYS_setsockopt:  return handle_setsockopt(pid, info);
        case SYS_getsockopt:  return handle_getsockopt(pid, info);
        case SYS_epoll_create: return handle_epoll_create(pid, info);
        case SYS_epoll_create1: return handle_epoll_create1(pid, info);
        case SYS_epoll_ctl:   return handle_epoll_ctl(pid, info);
        case SYS_epoll_wait:  return handle_epoll_wait(pid, info);
        case SYS_epoll_pwait: return handle_epoll_pwait(pid, info);
        case SYS_epoll_pwait2: return handle_epoll_pwait2(pid, info);
        default:              return CKPT_NO;
    }
}

int handle_sendto(pid_t pid, syscall_info &info) {
    long ret = emu_sendto(info.args[0], (void *)info.args[1], info.args[2],
                          info.args[3], (struct sockaddr *)info.args[4],
                          (socklen_t)info.args[5]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_recvfrom(pid_t pid, syscall_info &info) {
    long ret = emu_recvfrom(info.args[0], (void *)info.args[1], info.args[2],
                            info.args[3], (struct sockaddr *)info.args[4],
                            (socklen_t *)info.args[5]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    if (ret < 0 && ptmc_state.n_choose < 2) {
        return CKPT_NO;
    }
    return CKPT_YES;
}

int handle_socket(pid_t pid, syscall_info &info) {
    long ret = emu_socket(info.args[0], info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_connect(pid_t pid, syscall_info &info) {
    long ret = emu_connect(info.args[0], (const struct sockaddr *)info.args[1],
                           info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_bind(pid_t pid, syscall_info &info) {
    long ret = emu_bind(info.args[0], (const struct sockaddr *)info.args[1],
                        info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_listen(pid_t pid, syscall_info &info) {
    long ret = emu_listen(info.args[0], info.args[1]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_accept(pid_t pid, syscall_info &info) {
    long ret = emu_accept(info.args[0], (struct sockaddr *)info.args[1],
                          (socklen_t *)info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_setsockopt(pid_t pid, syscall_info &info) {
    long ret = emu_setsockopt(info.args[0], info.args[1], info.args[2],
                              (void *)info.args[3], info.args[4]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_getsockopt(pid_t pid, syscall_info &info) {
    long ret = emu_getsockopt(info.args[0], info.args[1], info.args[2],
                              (void *)info.args[3], (socklen_t *)info.args[4]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_create(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_create(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_create1(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_create1(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_ctl(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_ctl(info.args[0], info.args[1], info.args[2],
                             (struct epoll_event *)info.args[3]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_wait(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_wait(info.args[0], (struct epoll_event *)info.args[1],
                              info.args[2], info.args[3]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_pwait(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_pwait(info.args[0], (struct epoll_event *)info.args[1],
                               info.args[2], info.args[3],
                               (const sigset_t *)info.args[4], info.args[5]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_epoll_pwait2(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_pwait2(info.args[0], (struct epoll_event *)info.args[1],
                                info.args[2], (const struct timespec *)info.args[3],
                                (const sigset_t *)info.args[4], info.args[5]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

} // namespace network
} // namespace handlers

/* ======================================================================
 * Filesystem Handlers
 * ====================================================================== */

namespace handlers {
namespace fs {

int handle(pid_t pid, syscall_info &info) {
    switch (info.nr) {
        case SYS_open:      return handle_open(pid, info);
        case SYS_openat:    return handle_openat(pid, info);
        case SYS_read:      return handle_read(pid, info);
        case SYS_write:     return handle_write(pid, info);
        case SYS_close:     return handle_close(pid, info);
        case SYS_lseek:     return handle_lseek(pid, info);
        case SYS_stat:      return handle_stat(pid, info);
        case SYS_fstat:     return handle_fstat(pid, info);
        case SYS_chdir:     return handle_chdir(pid, info);
        case SYS_pipe:
        case SYS_pipe2:     return handle_pipe(pid, info);
        case SYS_mmap:      return handle_mmap(pid, info);
        case SYS_munmap:    return handle_munmap(pid, info);
        case SYS_madvise:   return handle_madvise(pid, info);
        case SYS_fsync:     return handle_fsync(pid, info);
        case SYS_fdatasync: return handle_fdatasync(pid, info);
        case SYS_fcntl:     return handle_fcntl(pid, info);
        default:            return CKPT_NO;
    }
}

int handle_open(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_openat(AT_FDCWD, (const char *)info.args[0], info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_openat(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_openat(info.args[0], (const char *)info.args[1], info.args[2], info.args[3]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_read(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_read(info.args[0], (void *)info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_write(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_write(info.args[0], (const void *)info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_close(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_close(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_lseek(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_lseek(info.args[0], info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_stat(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_stat((const char *)info.args[0], (struct stat *)info.args[1]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_fstat(pid_t pid, syscall_info &info) {
    long ret = emu_vfs_fstat(info.args[0], (struct stat *)info.args[1]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_chdir(pid_t pid, syscall_info &info) {
    long ret = emu_chdir((const char *)info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_pipe(pid_t pid, syscall_info &info) {
    int pipefd[2];
    long ret = emu_pipe(pipefd, info.args[1]);
    if (ret == 0) {
        memcpy_host2guest((void *)info.args[0], pipefd, sizeof(pipefd));
    }
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_mmap(pid_t pid, syscall_info &info) {
    // VFS mmap is handled in dispatcher
    return CKPT_NO;
}

int handle_munmap(pid_t pid, syscall_info &info) {
    long ret = emu_munmap((void *)info.args[0], info.args[1]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_madvise(pid_t pid, syscall_info &info) {
    void *addr = (void *)info.args[0];
    size_t length = info.args[1];
    int advice = (int)info.args[2];

    // For MADV_DONTNEED, we need to wake any futex waiters on this memory range
    // because the kernel would unmap these pages, causing futex waits to return
    if (advice == MADV_DONTNEED) {
        int tracee_idx = ptmc_state.cursor;
        auto& child_state = ptmc_state.running_state.child[tracee_idx];

        if (child_state.futex_state) {
            uint64_t start = (uint64_t)addr;
            uint64_t end = start + length;
            int woken = child_state.futex_state->wake_waiters_in_range(start, end);
            if (woken > 0) {
                LOG_INFO("madvise(MADV_DONTNEED): woke %d futex waiters in range [%p, %p)",
                         woken, addr, (void*)end);

                // Mark the woken threads as unblocked in ptmc_state
                for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
                    if (ptmc_state.thread_blocked[tracee_idx][i]) {
                        // Check if this thread is still waiting on a futex
                        int virtual_tid = i + 1;
                        if (!child_state.futex_state->is_thread_waiting(virtual_tid)) {
                            ptmc_state.thread_blocked[tracee_idx][i] = false;
                            LOG_DEBUG("Thread %d unblocked after madvise", i);
                        }
                    }
                }
            }
        }
    }

    // Pass through to kernel
    long ret = syscall(SYS_madvise, addr, length, advice);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_fsync(pid_t pid, syscall_info &info) {
    long ret = emu_fsync(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_fdatasync(pid_t pid, syscall_info &info) {
    long ret = emu_fdatasync(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_fcntl(pid_t pid, syscall_info &info) {
    long ret = emu_fcntl(info.args[0], info.args[1], info.args[2]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

} // namespace fs
} // namespace handlers

/* ======================================================================
 * Thread Handlers
 * ====================================================================== */

namespace handlers {
namespace thread {

int handle(pid_t pid, syscall_info &info) {
    switch (info.nr) {
        case SYS_clone:      return handle_clone(pid, info);
        case SYS_clone3:     return handle_clone3(pid, info);
        case SYS_gettid:     return handle_gettid(pid, info);
        case SYS_exit:       return handle_exit(pid, info);
        case SYS_exit_group: return handle_exit_group(pid, info);
        default:             return CKPT_NO;
    }
}

int handle_clone(pid_t pid, syscall_info &info) {
    // Thread creation is recorded in extract_one_syscall
    // Just checkpoint here
    if (info.rval > 0) {
        pid_t new_tid = (pid_t)info.rval;
        LOG_INFO("Clone completed: new thread TID %d", new_tid);

        // Wait for and continue the new thread
        int wstatus;
        int retries = 0;
        pid_t waited = 0;

        while (retries < 100 && waited == 0) {
            waited = waitpid(new_tid, &wstatus, WNOHANG);
            if (waited == 0) {
                usleep(1000);
                retries++;
            }
        }

        if (waited == new_tid && WIFSTOPPED(wstatus)) {
            LOG_INFO("New thread %d stopped with signal %d, continuing",
                     new_tid, WSTOPSIG(wstatus));
            ptrace(PTRACE_SYSCALL, new_tid, NULL, NULL);
        } else {
            LOG_WARN("New thread %d not in expected stopped state (waited=%d)",
                     new_tid, waited);
        }
    }
    return CKPT_YES;
}

int handle_clone3(pid_t pid, syscall_info &info) {
    // Similar to clone
    if (info.rval > 0) {
        pid_t new_tid = (pid_t)info.rval;
        LOG_INFO("Clone3 completed: new thread TID %d", new_tid);

        // Wait for and continue the new thread
        // The new thread is created in a stopped state (CLONE_PTRACE),
        // so we need to wait for it and continue it for it to make progress
        int wstatus;
        int retries = 0;
        pid_t waited = 0;

        while (retries < 100 && waited == 0) {
            waited = waitpid(new_tid, &wstatus, WNOHANG);
            if (waited == 0) {
                usleep(1000);
                retries++;
            }
        }

        if (waited == new_tid && WIFSTOPPED(wstatus)) {
            LOG_INFO("New thread %d stopped with signal %d, continuing",
                     new_tid, WSTOPSIG(wstatus));
            // Continue the new thread to its first syscall entry
            ptrace(PTRACE_SYSCALL, new_tid, NULL, NULL);
        } else {
            LOG_WARN("New thread %d not in expected stopped state (waited=%d)",
                     new_tid, waited);
        }
    }
    return CKPT_YES;
}

int handle_gettid(pid_t pid, syscall_info &info) {
    // Return virtual TID (thread index within the tracee)
    int tracee_idx = ptmc_state.cursor;
    int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
    int virtual_tid = thread_idx + 1;  // +1 to avoid 0
    tracee_set_rax(pid, virtual_tid);
    info.rval = virtual_tid;
    LOG_DEBUG("gettid: returning virtual tid %d for thread %d in process %d",
              virtual_tid, thread_idx, tracee_idx);
    return CKPT_NO;
}

int handle_exit(pid_t pid, syscall_info &info) {
    // Thread exit is handled in do_one_syscall via handle_thread_exit
    return CKPT_EXIT;
}

int handle_exit_group(pid_t pid, syscall_info &info) {
    // Mark process as exited
    ptmc_state.status[ptmc_state.cursor] = dstatus_exit(info.args[0]);
    return CKPT_EXIT;
}

} // namespace thread
} // namespace handlers

/* ======================================================================
 * Futex Handlers
 * ====================================================================== */

namespace handlers {
namespace futex {

int handle(pid_t pid, syscall_info &info) {
    int tracee_idx = ptmc_state.cursor;
    auto& child_state = ptmc_state.running_state.child[tracee_idx];

    // Initialize FutexState if needed
    if (!child_state.futex_state) {
        child_state.futex_state = new FutexState();
    }

    uint64_t uaddr = info.args[0];
    int futex_op = (int)info.args[1];
    int val = (int)info.args[2];
    uint64_t timeout = info.args[3];
    uint64_t uaddr2 = info.args[4];
    int val3 = (int)info.args[5];

    // For FUTEX_WAIT, we need to read the current value at uaddr
    uint64_t stackval = 0;
    if ((futex_op & 0x7F) == FUTEX_WAIT || (futex_op & 0x7F) == FUTEX_WAIT_BITSET) {
        tracee_read_mem(pid, (void*)uaddr, &stackval, sizeof(int));
    }

    // Use virtual TID (thread index + 1) instead of physical PID
    int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
    int virtual_tid = thread_idx + 1;

    int result = child_state.futex_state->handle_futex(
        virtual_tid, uaddr, futex_op, val, timeout, uaddr2, val3,
        (futex_op & 0x7F) == FUTEX_WAIT || (futex_op & 0x7F) == FUTEX_WAIT_BITSET ? &stackval : nullptr);

    // Check if current thread is now waiting on futex (must check BEFORE setting result)
    int op_type = futex_op & 0x7F;
    if (op_type == FUTEX_WAIT || op_type == FUTEX_WAIT_BITSET) {
        if (child_state.futex_state->is_thread_waiting(virtual_tid)) {
            LOG_INFO("Thread %d (vTID %d) is now waiting on futex - DISCARDING", pid, virtual_tid);
            ptmc_state.thread_blocked[tracee_idx][thread_idx] = true;
            // Return -EAGAIN so the tracee knows the wait didn't complete
            tracee_set_rax(pid, (uint64_t)-EAGAIN);
            info.rval = (uint64_t)-EAGAIN;
            return CKPT_DISCARD;
        }
    }

    if (result < 0) {
        tracee_set_rax(pid, (uint64_t)result);
        info.rval = (uint64_t)result;
    } else {
        tracee_set_rax(pid, result);
        info.rval = result;
    }

    if (op_type == FUTEX_WAKE || op_type == FUTEX_WAKE_BITSET) {
        if (result > 0) {
            LOG_DEBUG("FUTEX_WAKE waked %d threads, unblocking them", result);
            auto waiters = child_state.futex_state->get_waiters(uaddr);
            for (pid_t waiter_vtid : waiters) {
                int waiter_idx = waiter_vtid - 1;
                if (waiter_idx >= 0 && waiter_idx < 64) {
                    ptmc_state.thread_blocked[tracee_idx][waiter_idx] = false;
                }
            }
        }
    }

    return CKPT_YES;
}

} // namespace futex
} // namespace handlers
/* ======================================================================
 * Time Handlers
 * ====================================================================== */

namespace handlers {
namespace time {

int handle(pid_t pid, syscall_info &info) {
    switch (info.nr) {
        case SYS_gettimeofday:    return handle_gettimeofday(pid, info);
        case SYS_nanosleep:       return handle_nanosleep(pid, info);
        case SYS_clock_nanosleep: return handle_clock_nanosleep(pid, info);
        default:                  return CKPT_NO;
    }
}

int handle_gettimeofday(pid_t pid, syscall_info &info) {
    long ret = emu_gettimeofday((struct timeval *)info.args[0],
                                (struct timezone *)info.args[1]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_nanosleep(pid_t pid, syscall_info &info) {
    // Return immediately for deterministic execution
    tracee_set_rax(pid, 0);
    info.rval = 0;
    return CKPT_NO;
}

int handle_clock_nanosleep(pid_t pid, syscall_info &info) {
    // Return immediately for deterministic execution
    tracee_set_rax(pid, 0);
    info.rval = 0;
    return CKPT_NO;
}

} // namespace time
} // namespace handlers
