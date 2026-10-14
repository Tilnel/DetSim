/*
 * dispatcher.cpp - System call dispatch and emulation
 */

#include "dispatcher.h"
#include "handlers.h"
#include "guest.h"
#include "log_wrapper.h"
#include "monitor.h"
#include "thread_manager.h"
#include "sync/futexstate.h"
#include <cassert>
#include <cstring>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/mman.h>

// External functions from state_transition.cpp
extern int analyze_choose(struct syscall_info &info);
extern int is_auto_mode();

// Forward declarations for choice handling (defined in state_transition.cpp)
void handle_user_choice();
void handle_auto_choice();

// Checkpoint return codes
#define CKPT_YES 1
#define CKPT_NO 0
#define CKPT_EXIT 2
#define CKPT_STOP 3
#define CKPT_DISCARD 4

/* ======================================================================
 * SyscallDispatcher Implementation
 * ====================================================================== */

SyscallDispatcher::SyscallDispatcher()
    : vfs_mmap_state_{false, -1, 0, 0}
{
}

SyscallRoute SyscallDispatcher::get_route(int syscall_nr) const
{
    switch (syscall_nr) {
        // Network syscalls - emulated
        case SYS_sendto:
        case SYS_recvfrom:
        case SYS_socket:
        case SYS_connect:
        case SYS_bind:
        case SYS_listen:
        case SYS_accept:
        case SYS_accept4:
        case SYS_setsockopt:
        case SYS_getsockopt:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "Network syscall - emulated");

        // Epoll syscalls - emulated
        case SYS_epoll_create:
        case SYS_epoll_create1:
        case SYS_epoll_ctl:
        case SYS_epoll_wait:
        case SYS_epoll_pwait:
        case SYS_epoll_pwait2:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "Epoll syscall - emulated");

        // Time syscalls - emulated
        case SYS_gettimeofday:
        case SYS_nanosleep:
        case SYS_clock_nanosleep:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "Time syscall - emulated");

        // VFS syscalls - emulated
        case SYS_open:
        case SYS_openat:
        case SYS_read:
        case SYS_write:
        case SYS_close:
        case SYS_lseek:
        case SYS_stat:
        case SYS_fstat:
        case SYS_chdir:
        case SYS_pipe:
        case SYS_pipe2:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "VFS syscall - emulated");

        // Clone syscalls - modified
        case SYS_clone:
        case SYS_clone3:
            return SyscallRoute(SyscallCategory::MODIFIED,
                               SyscallAction::MODIFY,
                               "Clone - add CLONE_PTRACE");

        // Blocked syscalls
        case SYS_exit_group:
            return SyscallRoute(SyscallCategory::BLOCKED,
                               SyscallAction::BLOCK,
                               "exit_group - blocked");

        // Special handling
        case SYS_exit:
        case SYS_futex:
        case SYS_mmap:
        case SYS_gettid:
            return SyscallRoute(SyscallCategory::SPECIAL,
                               SyscallAction::DEFERRED,
                               "Special handling required");

        // Everything else - passthrough
        default:
            return SyscallRoute(SyscallCategory::PASSTHROUGH,
                               SyscallAction::EXECUTE,
                               "Passthrough");
    }
}

bool SyscallDispatcher::is_emulated(int syscall_nr) const
{
    SyscallRoute route = get_route(syscall_nr);
    return route.category == SyscallCategory::EMULATED;
}

bool SyscallDispatcher::needs_checkpoint(int syscall_nr) const
{
    // Most emulated syscalls need checkpoint
    // Network, FS, and thread-related syscalls
    switch (syscall_nr) {
        case SYS_sendto:
        case SYS_recvfrom:
        case SYS_socket:
        case SYS_connect:
        case SYS_bind:
        case SYS_listen:
        case SYS_accept:
        case SYS_accept4:
        case SYS_open:
        case SYS_openat:
        case SYS_clone:
        case SYS_clone3:
        case SYS_futex:
        case SYS_epoll_wait:
        case SYS_epoll_pwait:
        case SYS_epoll_pwait2:
            return true;
        default:
            return false;
    }
}

bool SyscallDispatcher::on_enter(pid_t pid, int syscall_nr, syscall_info& info)
{
    SyscallRoute route = get_route(syscall_nr);

    LOG_TRACE("Syscall %d enter: %s", syscall_nr, route.description);

    switch (route.action) {
        case SyscallAction::EMULATE:
            // Block the syscall - will be emulated on exit
            tracee_set_orig_rax(pid, -1);
            tracee_set_rax(pid, -1);
            return true;

        case SyscallAction::MODIFY:
            if (syscall_nr == SYS_clone) {
                handle_clone_enter(pid, info);
            } else if (syscall_nr == SYS_clone3) {
                handle_clone3_enter(pid, info);
            }
            return true;

        case SyscallAction::BLOCK:
            tracee_set_orig_rax(pid, -1);
            tracee_set_rax(pid, -1);
            return true;

        case SyscallAction::DEFERRED:
            if (syscall_nr == SYS_mmap) {
                handle_mmap_enter(pid, info);
            } else if (syscall_nr == SYS_futex) {
                // For futex, we need to handle it specially on enter
                // to prevent blocking operations from blocking the tracee
                handle_futex_enter(pid, info);
            }
            return true;

        case SyscallAction::EXECUTE:
        default:
            return true;
    }
}

int SyscallDispatcher::on_exit(pid_t pid, syscall_info& info)
{
    // Handle choice for syscalls with multiple outcomes
    ptmc_state.n_choose = analyze_choose(info);
    if (ptmc_state.n_choose != 0) {
        if (is_auto_mode()) {
            handle_auto_choice();
        } else {
            handle_user_choice();
        }
    }

    // Route to appropriate handler
    switch (info.nr) {
        // Network syscalls
        case SYS_sendto:
        case SYS_recvfrom:
        case SYS_socket:
        case SYS_connect:
        case SYS_bind:
        case SYS_listen:
        case SYS_accept:
        case SYS_accept4:
        case SYS_setsockopt:
        case SYS_getsockopt:
        case SYS_epoll_create:
        case SYS_epoll_create1:
        case SYS_epoll_ctl:
        case SYS_epoll_wait:
        case SYS_epoll_pwait:
        case SYS_epoll_pwait2:
            return handle_network_exit(pid, info);

        // VFS syscalls
        case SYS_open:
        case SYS_openat:
        case SYS_read:
        case SYS_write:
        case SYS_close:
        case SYS_lseek:
        case SYS_stat:
        case SYS_fstat:
        case SYS_chdir:
        case SYS_pipe:
        case SYS_pipe2:
        case SYS_mmap:
        case SYS_munmap:
        case SYS_fsync:
        case SYS_fdatasync:
        case SYS_fcntl:
            return handle_fs_exit(pid, info);

        // Thread syscalls
        case SYS_clone:
        case SYS_clone3:
        case SYS_gettid:
        case SYS_exit:
        case SYS_exit_group:
            return handle_thread_exit(pid, info);

        // Futex
        case SYS_futex:
            return handle_futex_exit(pid, info);

        // Time syscalls
        case SYS_gettimeofday:
        case SYS_nanosleep:
        case SYS_clock_nanosleep:
            return handle_time_exit(pid, info);

        // Default - no checkpoint needed
        default:
            return CKPT_NO;
    }
}

void SyscallDispatcher::handle_mmap_enter(pid_t pid, syscall_info& info)
{
    // Check if this is a VFS file mmap
    int fd = ptrace(PTRACE_PEEKUSER, pid,
                    offsetof(struct user_regs_struct, r8), NULL);
    auto &fs = ptmc_state.fs_states[ptmc_state.cursor];

    if (fd >= 0 && fs.open_files.find(fd) != fs.open_files.end()) {
        // Save VFS mmap state
        vfs_mmap_state_.active = true;
        vfs_mmap_state_.fd = fd;
        vfs_mmap_state_.offset = ptrace(PTRACE_PEEKUSER, pid,
                                        offsetof(struct user_regs_struct, r9), NULL);
        vfs_mmap_state_.length = ptrace(PTRACE_PEEKUSER, pid,
                                        offsetof(struct user_regs_struct, rsi), NULL);
        int prot = ptrace(PTRACE_PEEKUSER, pid,
                          offsetof(struct user_regs_struct, rdx), NULL);
        int flags = ptrace(PTRACE_PEEKUSER, pid,
                           offsetof(struct user_regs_struct, r10), NULL);

        // Change to anonymous mmap (fd = -1)
        ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, rdx),
               prot | PROT_WRITE);
        ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, r10),
               flags | MAP_ANONYMOUS);
        ptrace(PTRACE_POKEUSER, pid, offsetof(struct user_regs_struct, r8), -1);

        LOG_DEBUG("VFS mmap: fd=%d, offset=%ld, len=%zu", fd,
                  vfs_mmap_state_.offset, vfs_mmap_state_.length);
    } else {
        vfs_mmap_state_.active = false;
    }
}

void SyscallDispatcher::handle_clone_enter(pid_t pid, syscall_info& info)
{
    // Add CLONE_PTRACE flag to auto-trace child threads
    uint64_t new_flags = info.args[0] | CLONE_PTRACE;
    tracee_set_rdi(pid, new_flags);
    LOG_INFO("Modified clone flags to add CLONE_PTRACE");
}

void SyscallDispatcher::handle_clone3_enter(pid_t pid, syscall_info& info)
{
    // clone3 takes a struct clone_args *args and size_t size
    uint64_t args_ptr = ptrace(PTRACE_PEEKUSER, pid,
                               offsetof(struct user_regs_struct, rdi), NULL);
    // Read current flags from clone_args
    uint64_t flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
    // Add CLONE_PTRACE flag
    flags |= CLONE_PTRACE;
    ptrace(PTRACE_POKEDATA, pid, args_ptr, flags);
    LOG_INFO("Modified clone3 flags at %p to add CLONE_PTRACE", (void*)args_ptr);
}

void SyscallDispatcher::handle_exit_group_enter(pid_t pid, syscall_info& info)
{
    // Block the syscall
    tracee_set_orig_rax(pid, -1);
    tracee_set_rax(pid, -1);
}

void SyscallDispatcher::handle_futex_enter(pid_t pid, syscall_info& info)
{
    (void)info;
    // Every futex operation is modeled or explicitly rejected on exit.
    // Unsupported PI operations must never block or mutate host kernel state.
    tracee_set_orig_rax(pid, -1);
}

int SyscallDispatcher::handle_network_exit(pid_t pid, syscall_info& info)
{
    // Delegate to network handlers
    return handlers::network::handle(pid, info);
}

int SyscallDispatcher::handle_fs_exit(pid_t pid, syscall_info& info)
{
    // Delegate to FS handlers
    int result = handlers::fs::handle(pid, info);

    // Handle VFS mmap special case
    if (info.nr == SYS_mmap && vfs_mmap_state_.active) {
        void *addr = (void *)info.rval;
        if (addr != MAP_FAILED) {
            auto &fs = ptmc_state.fs_states[ptmc_state.cursor];
            auto fd_it = fs.open_files.find(vfs_mmap_state_.fd);
            if (fd_it != fs.open_files.end()) {
                auto node_it = fs.filesystem.find(fd_it->second.path);
                if (node_it != fs.filesystem.end()) {
                    // Write file content to tracee memory
                    auto& node = node_it->second;
                    size_t offset = vfs_mmap_state_.offset;
                    size_t len = vfs_mmap_state_.length;

                    if (offset > node.content.size())
                        offset = node.content.size();
                    if (offset + len > node.content.size())
                        len = node.content.size() - offset;

                    memcpy_host2guest(addr, node.content.data() + offset, len);
                }
            }
        }
        vfs_mmap_state_.active = false;
    }

    return result;
}

int SyscallDispatcher::handle_thread_exit(pid_t pid, syscall_info& info)
{
    return handlers::thread::handle(pid, info);
}

int SyscallDispatcher::handle_futex_exit(pid_t pid, syscall_info& info)
{
    return handlers::futex::handle(pid, info);
}

int SyscallDispatcher::handle_time_exit(pid_t pid, syscall_info& info)
{
    return handlers::time::handle(pid, info);
}

/* ======================================================================
 * Global Dispatcher
 * ====================================================================== */

static SyscallDispatcher g_dispatcher;

namespace syscall_dispatch {

void init()
{
    LOG_INFO("Syscall dispatcher initialized");
}

SyscallDispatcher* get()
{
    return &g_dispatcher;
}

bool enter(pid_t pid, int syscall_nr, syscall_info& info)
{
    return g_dispatcher.on_enter(pid, syscall_nr, info);
}

int exit(pid_t pid, syscall_info& info)
{
    return g_dispatcher.on_exit(pid, info);
}

} // namespace syscall_dispatch
