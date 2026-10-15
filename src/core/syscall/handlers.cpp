/*
 * handlers.cpp - Individual syscall handler implementations
 */

#include "handlers.h"
#include "guest.h"
#include "io.h"
#include "log_wrapper.h"
#include "monitor.h"
#include "proc_status.h"
#include "state/state.h"
#include "thread_manager.h"
#include "engine/thread_pool.h"
#include "engine/pending_syscall.h"
#include "pending_io.h"
#include "sync/futexstate.h"
#include "fs/fsstate.h"
#include "net/sockstate.h"
#include "net/emu.h"
#include <sys/syscall.h>
#include <linux/sched.h>
#include <algorithm>
#include <array>
#include <limits>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <stdexcept>

static int finish_io_call(pid_t pid, syscall_info& info, long result) {
    if (io_should_suspend(ptmc_state.cursor, info, result)) {
        suspend_syscall(ptmc_state.cursor,pid,info,PendingReason::Io);
        configure_pending_io(ptmc_state.cursor,pid,info);
    } else {
        tracee_set_rax(pid,result);
        info.rval=result;
    }
    return CKPT_YES;
}

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
        case SYS_getsockname: return handle_getsockname(pid, info);
        case SYS_getpeername: return handle_getpeername(pid, info);
        case SYS_poll: return handle_poll(pid, info);
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
    return finish_io_call(pid,info,ret);
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
    long ret = info.nr==SYS_accept4
        ? emu_accept4(info.args[0],(struct sockaddr*)info.args[1],(socklen_t*)info.args[2],info.args[3])
        : emu_accept(info.args[0],(struct sockaddr*)info.args[1],(socklen_t*)info.args[2]);
    return finish_io_call(pid,info,ret);
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

int handle_poll(pid_t pid, syscall_info &info) {
    info.rval = emu_poll(reinterpret_cast<pollfd*>(info.args[0]),
                         static_cast<uint32_t>(info.args[1]), static_cast<int>(info.args[2]));
    tracee_set_rax(pid, info.rval);
    return CKPT_YES;
}

int handle_epoll_create(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_create(info.args[0]);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_YES;
}

int handle_getsockname(pid_t pid, syscall_info& info) {
    const long result=emu_getsockname(info.args[0],(sockaddr*)info.args[1],(socklen_t*)info.args[2]);
    tracee_set_rax(pid,result);info.rval=result;return CKPT_NO;
}
int handle_getpeername(pid_t pid, syscall_info& info) {
    const long result=emu_getpeername(info.args[0],(sockaddr*)info.args[1],(socklen_t*)info.args[2]);
    tracee_set_rax(pid,result);info.rval=result;return CKPT_NO;
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
    return finish_io_call(pid,info,ret);
}

int handle_epoll_pwait(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_pwait(info.args[0], (struct epoll_event *)info.args[1],
                               info.args[2], info.args[3],
                               (const sigset_t *)info.args[4], info.args[5]);
    return finish_io_call(pid,info,ret);
}

int handle_epoll_pwait2(pid_t pid, syscall_info &info) {
    long ret = emu_epoll_pwait2(info.args[0], (struct epoll_event *)info.args[1],
                                info.args[2], (const struct timespec *)info.args[3],
                                (const sigset_t *)info.args[4], info.args[5]);
    return finish_io_call(pid,info,ret);
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
        case SYS_ioctl:     return handle_ioctl(pid, info);
        case SYS_newfstatat:return handle_statat(pid,info);
        case SYS_getcwd:    return handle_getcwd(pid,info);
        case SYS_lstat:     return handle_stat(pid,info);
        case SYS_readlink:
        case SYS_readlinkat: {
            const bool at = info.nr == SYS_readlinkat;
            const long result = emu_vfs_readlinkat(at ? static_cast<int>(info.args[0]) : AT_FDCWD,
                (const char*)info.args[at ? 1 : 0], (char*)info.args[at ? 2 : 1], info.args[at ? 3 : 2]);
            tracee_set_rax(pid,result); info.rval=result; return CKPT_YES;
        }
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
    long ret = emu_read(info.args[0], (void *)info.args[1], info.args[2]);
    return finish_io_call(pid,info,ret);
}

int handle_write(pid_t pid, syscall_info &info) {
    long ret = emu_write(info.args[0], (const void *)info.args[1], info.args[2]);
    return finish_io_call(pid,info,ret);
}

int handle_close(pid_t pid, syscall_info &info) {
    long ret = emu_close(info.args[0]);
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
    long ret = emu_vfs_statat(AT_FDCWD, (const char *)info.args[0], (struct stat *)info.args[1],
        info.nr == SYS_lstat ? AT_SYMLINK_NOFOLLOW : 0);
    tracee_set_rax(pid, ret);
    info.rval = ret;
    return CKPT_NO;
}

int handle_fstat(pid_t pid, syscall_info &info) {
    long ret = emu_fstat(info.args[0], (struct stat *)info.args[1]);
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
    long ret = emu_pipe(pipefd, info.nr == SYS_pipe2 ? info.args[1] : 0);
    if (ret == 0) {
        if (!guest_write_exact(pid, (void*)info.args[0], pipefd, sizeof(pipefd))) {
            emu_close(pipefd[0]); emu_close(pipefd[1]); ret = -EFAULT;
        }
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
    (void)pid; (void)info;
    // The kernel already performed this operation in the guest address space.
    return CKPT_NO;
}

int handle_madvise(pid_t pid, syscall_info &info) {
    (void)pid;
    (void)info;
    // madvise has already run in the tracee. Discarding private pages does
    // not wake futex waiters, and guest addresses must not touch tracer VMAs.
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

int handle_ioctl(pid_t pid, syscall_info& info) {
    const long result=emu_ioctl(info.args[0],info.args[1],(void*)info.args[2]);
    tracee_set_rax(pid,result);info.rval=result;return CKPT_YES;
}

int handle_statat(pid_t pid, syscall_info& info) {
    const long result=emu_vfs_statat(info.args[0],(const char*)info.args[1],(struct stat*)info.args[2],info.args[3]);
    tracee_set_rax(pid,result);info.rval=result;return CKPT_NO;
}
int handle_getcwd(pid_t pid, syscall_info& info) {
    const long result=emu_getcwd((char*)info.args[0],info.args[1]);
    tracee_set_rax(pid,result);info.rval=result;return CKPT_NO;
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
        case SYS_set_robust_list:
            info.rval=emulate_set_robust_list(ptmc_state.cursor,pid,info.args[0],info.args[1]);
            tracee_set_rax(pid,info.rval);return CKPT_YES;
        case SYS_get_robust_list:
            info.rval=emulate_get_robust_list(ptmc_state.cursor,pid,static_cast<int32_t>(info.args[0]),info.args[1],info.args[2]);
            tracee_set_rax(pid,info.rval);return CKPT_YES;
        case SYS_rt_sigprocmask:
            info.rval=emulate_thread_sigprocmask(ptmc_state.cursor,pid,info.args[0],info.args[1],info.args[2],info.args[3]);
            tracee_set_rax(pid,info.rval);return CKPT_YES;
        case SYS_set_tid_address:
            info.rval=record_logical_clear_tid(ptmc_state.cursor,pid,info.args[0]);
            tracee_set_rax(pid,info.rval);return CKPT_YES;
        case SYS_exit:       return handle_exit(pid, info);
        case SYS_exit_group: return handle_exit_group(pid, info);
        default:             return CKPT_NO;
    }
}

static int finish_clone(pid_t pid, syscall_info& info, const CloneRequest& request) {
    const pid_t slot=create_logical_thread(ptmc_state.cursor,pid,request);
    auto& child_info=ptmc_state.running_state.child[ptmc_state.cursor].si[slot-1];
    child_info=info;child_info.rval=0;
    info.rval=logical_thread_id(ptmc_state.cursor,slot);
    tracee_set_rax(pid,info.rval);
    return CKPT_YES;
}

int handle_clone(pid_t pid, syscall_info &info) {
    return finish_clone(pid,info,{info.args[0],info.args[1],info.args[4],info.args[2],info.args[3]});
}

int handle_clone3(pid_t pid, syscall_info &info) {
    auto error=[&](long value) {info.rval=value;tracee_set_rax(pid,value);return CKPT_YES;};
    const auto size=info.args[1];
    if (size<CLONE_ARGS_SIZE_VER0) return error(-EINVAL);
    if (size>4096) return error(-E2BIG);
    clone_args args{};
    if (size>sizeof(args)) {
        std::array<unsigned char,4096-sizeof(args)> tail{};
        if (info.args[0]>UINT64_MAX-size || !guest_read_exact(pid,
            reinterpret_cast<const void*>(info.args[0]+sizeof(args)),tail.data(),size-sizeof(args)))
            return error(-EFAULT);
        if (std::any_of(tail.begin(),tail.begin()+size-sizeof(args),[](auto byte){return byte!=0;}))
            return error(-E2BIG);
    }
    // Linux checks an oversized ABI tail before copying the known header.
    if (!guest_read_exact(pid,reinterpret_cast<const void*>(info.args[0]),&args,
                          std::min<uint64_t>(size,sizeof(args)))) return error(-EFAULT);
    // Only shared-address-space threads are implemented. Ignored ABI fields
    // (e.g. pidfd without CLONE_PIDFD) are not treated as active requests.
    if (args.exit_signal>64 || args.set_tid_size>32 ||
        bool(args.set_tid)!=bool(args.set_tid_size)) return error(-EINVAL);
    validate_logical_clone(args.flags);
    if (args.exit_signal) return error(-EINVAL);
    if (args.set_tid_size) throw std::runtime_error("clone3 explicit TID selection is unsupported");
    if ((!args.stack)!=(!args.stack_size) || args.stack>UINT64_MAX-args.stack_size)
        return error(-EINVAL);
    if (args.stack >= (uint64_t{1}<<63) || args.stack_size >= (uint64_t{1}<<63))
        return error(-EINVAL);
    // The ordinary lower canonical range is common to four- and five-level
    // x86-64 kernels. Do not guess access_ok for extended address layouts.
    constexpr uint64_t ordinary_limit=(uint64_t{1}<<47)-4096;
    if (args.stack>=ordinary_limit || args.stack_size>ordinary_limit-args.stack)
        throw std::runtime_error("clone3 extended virtual-address stack ranges are unsupported");
    return finish_clone(pid,info,{args.flags,args.stack+args.stack_size,args.tls,args.parent_tid,args.child_tid});
}

int handle_gettid(pid_t pid, syscall_info &info) {
    const auto& threads=ptmc_state.running_state.child[ptmc_state.cursor].threads;
    const int slot=ptmc_state.current_thread_idx[ptmc_state.cursor];
    const pid_t tid=threads.empty()?1:threads.at(slot).tid;
    info.rval=logical_thread_id(ptmc_state.cursor,tid);
    tracee_set_rax(pid,info.rval);
    return CKPT_YES;
}

int handle_exit(pid_t pid, syscall_info &info) {
    const bool last=exit_logical_thread(ptmc_state.cursor,pid,info.args[0]);
    info.rval=0;tracee_set_rax(pid,0);
    return last?CKPT_EXIT:CKPT_YES;
}

int handle_exit_group(pid_t pid, syscall_info &info) {
    exit_logical_process(ptmc_state.cursor,pid,info.args[0]);
    info.rval=0;tracee_set_rax(pid,0);
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
    const int tracee_idx = ptmc_state.cursor;
    auto &child = ptmc_state.running_state.child[tracee_idx];
    if (!child.futex_state) child.futex_state.emplace();
    auto &futex = *child.futex_state;
    const uint64_t uaddr = info.args[0];
    const int futex_op = static_cast<int>(info.args[1]);
    const int op = futex_op & FUTEX_CMD_MASK;
    const int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
    const pid_t virtual_tid = child.threads.at(thread_idx).tid;
    const auto previous_waiters = futex.get_waiters(uaddr);

    std::optional<PendingDeadline> deadline;
    if ((op==FUTEX_WAIT || op==FUTEX_WAIT_BITSET) && info.args[3]) {
        timespec timeout{};
        long error=0;
        if (!guest_read_exact(pid,reinterpret_cast<void*>(info.args[3]),&timeout,sizeof(timeout))) error=-EFAULT;
        else if(timeout.tv_sec<0 || timeout.tv_nsec<0 || timeout.tv_nsec>=1000000000) error=-EINVAL;
        if(error) {tracee_set_rax(pid,error);info.rval=error;return CKPT_YES;}
        deadline=make_pending_deadline(tracee_idx,
            (futex_op&FUTEX_CLOCK_REALTIME)?CLOCK_REALTIME:CLOCK_MONOTONIC,
            timeout.tv_sec,timeout.tv_nsec,op==FUTEX_WAIT,-ETIMEDOUT);
    }

    // A futex word is exactly 32 bits. Never synthesize zero after a failed
    // guest read: that can turn EFAULT into a successful blocking wait.
    uint64_t value = 0;
    uint64_t *current_value = nullptr;
    if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET || op == FUTEX_CMP_REQUEUE) {
        uint32_t word = 0;
        iovec local{&word, sizeof(word)};
        iovec remote{reinterpret_cast<void *>(uaddr), sizeof(word)};
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) == sizeof(word)) {
            value = word;
            current_value = &value;
        }
    }
    const int result = futex.handle_futex(virtual_tid, uaddr, futex_op,
        static_cast<int>(info.args[2]), deadline?0:info.args[3], info.args[4],
        static_cast<int>(info.args[5]), current_value);
    if ((op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) && result == 0) {
        suspend_syscall(tracee_idx,pid,info,PendingReason::Futex);
        if(deadline) set_pending_deadline(tracee_idx,virtual_tid,*deadline);
    } else {
        tracee_set_rax(pid,static_cast<uint64_t>(result));
        info.rval=static_cast<uint64_t>(result);
    }

    // Requeued waiters remain blocked. Only waiters removed by this wake
    // become runnable; the surviving queue is not the set that was woken.
    for (pid_t tid : previous_waiters) {
        if (futex.is_thread_waiting(tid)) continue;
        for (size_t i = 0; i < child.threads.size(); ++i)
            if (child.threads[i].tid == tid)
                ptmc_state.thread_blocked[tracee_idx][i] = false;
    }
    refresh_futex_pending(tracee_idx);
    // Blocking registration and syscall completion are separate transitions.
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

static int sleep_call(pid_t pid, syscall_info& info, int clock, int flags, uint64_t request_address) {
    if (clock!=CLOCK_REALTIME && clock!=CLOCK_MONOTONIC)
        throw std::runtime_error("Unsupported sleep clock");
    if (flags&~TIMER_ABSTIME) {
        tracee_set_rax(pid,-EINVAL);info.rval=-EINVAL;return CKPT_YES;
    }
    timespec request{};
    long result=0;
    if (!guest_read_exact(pid,reinterpret_cast<void*>(request_address),&request,sizeof(request))) result=-EFAULT;
    else if (request.tv_sec<0 || request.tv_nsec<0 || request.tv_nsec>=1000000000) result=-EINVAL;
    if (result) {tracee_set_rax(pid,result);info.rval=result;return CKPT_YES;}
    const auto deadline=make_pending_deadline(ptmc_state.cursor,clock,request.tv_sec,request.tv_nsec,
                                            !(flags&TIMER_ABSTIME),0);
    const auto& now=ptmc_state.time[ptmc_state.cursor];
    if (now.tv_sec>deadline.sec || (now.tv_sec==deadline.sec && now.tv_usec*1000>=deadline.nsec)) {
        tracee_set_rax(pid,0);info.rval=0;return CKPT_YES;
    }
    suspend_syscall(ptmc_state.cursor,pid,info,PendingReason::Timer);
    set_pending_deadline(ptmc_state.cursor,
        ptmc_state.running_state.child[ptmc_state.cursor].threads.at(ptmc_state.current_thread_idx[ptmc_state.cursor]).tid,
        deadline);
    return CKPT_YES;
}

int handle_nanosleep(pid_t pid, syscall_info &info) {
    return sleep_call(pid,info,CLOCK_MONOTONIC,0,info.args[0]);
}

int handle_clock_nanosleep(pid_t pid, syscall_info &info) {
    return sleep_call(pid,info,static_cast<int>(info.args[0]),static_cast<int>(info.args[1]),info.args[2]);
}

} // namespace time
} // namespace handlers
