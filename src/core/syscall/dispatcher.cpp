/*
 * dispatcher.cpp - System call dispatch and emulation
 */

#include "dispatcher.h"
#include "handlers.h"
#include "guest.h"
#include "utils.h"
#include "log_wrapper.h"
#include "monitor.h"
#include "thread_manager.h"
#include "engine/thread_pool.h"
#include "sync/futexstate.h"
#include "process/process_state.h"
#include <cassert>
#include <cstring>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <stdexcept>
#include <fcntl.h>
#include <algorithm>
#include <limits>

namespace {
void validate_guest_mapping_range(uint64_t address,uint64_t length) {
    constexpr uint64_t page_size=4096;
    if (!length) return;
    constexpr uint64_t maximum=std::numeric_limits<uint64_t>::max();
    if (length>maximum-(page_size-1)) throw std::runtime_error("Mapping length overflow");
    const uint64_t rounded=(length+page_size-1)&~(page_size-1);
    if (address>maximum-rounded) throw std::runtime_error("Mapping address overflow");
    if (address<scratch_page+page_size && address+rounded>scratch_page)
        throw std::runtime_error("Guest mapping operation overlaps reserved syscall scratch page");
}

void validate_anonymous_mapping_range(pid_t pid, uint64_t address, uint64_t length) {
    // Invalid alignment/empty extents retain the kernel's normal errno behavior.
    if (!length || address % 4096) return;
    const uint64_t end = address + ((length + 4095) & ~uint64_t(4095));
    if (ptmc_state.fs_states[ptmc_state.cursor].overlaps_virtual_file_mapping(address, end))
        throw std::runtime_error("Operation requires anonymous memory, not virtual file mappings");
    auto file = fileutils::open_cfile("/proc/" + std::to_string(pid) + "/maps", "r");
    if (!file) throw std::runtime_error("Cannot inspect private anonymous mapping backing");
    std::vector<maps_item> maps;
    get_maps_item(maps, file.get());
    if (ferror(file.get()) || maps.empty())
        throw std::runtime_error("Cannot read complete private anonymous mapping backing");
    for (const auto& map : maps) {
        if (map.end <= address || map.start >= end) continue;
        if (map.flags[3] != 'p' || map.inode || map.name[0])
            throw std::runtime_error("Operation requires unnamed private anonymous mappings");
    }
}

}

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
    : vfs_mmap_state_{}
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
        case SYS_getsockname:
        case SYS_getpeername:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "Network syscall - emulated");

        // Readiness probes and epoll syscalls - emulated
        case SYS_poll:
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
        case SYS_pselect6:
        case SYS_clock_nanosleep:
        case SYS_clock_gettime:
        case SYS_clock_getres:
        case SYS_time:
        case SYS_getrandom:
        case SYS_sysinfo:
        case SYS_sched_getaffinity:
        case SYS_rt_sigaction:
        case SYS_setitimer:
        case SYS_getitimer:
        case SYS_umask:
        case SYS_getrlimit: case SYS_prlimit64: case SYS_prctl:
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
        case SYS_fcntl:
        case SYS_fsync:
        case SYS_fdatasync:
        case SYS_ioctl:
        case SYS_newfstatat:
        case SYS_getcwd:
        case SYS_lstat:
        case SYS_readlink:
        case SYS_readlinkat:
        case SYS_getdents64:
        case SYS_access:
        case SYS_faccessat:
        case SYS_faccessat2:
            return SyscallRoute(SyscallCategory::EMULATED,
                               SyscallAction::EMULATE,
                               "VFS syscall - emulated");

        // These operations must not escape into the host descriptor namespace.
        case SYS_dup: case SYS_dup2: case SYS_dup3:
        case SYS_readv: case SYS_writev: case SYS_pread64: case SYS_pwrite64:
        case SYS_preadv: case SYS_pwritev: case SYS_preadv2: case SYS_pwritev2:
        case SYS_sendmsg: case SYS_recvmsg: case SYS_sendmmsg: case SYS_recvmmsg:
        case SYS_socketpair: case SYS_shutdown:
        case SYS_ppoll: case SYS_select:
        case SYS_sendfile: case SYS_splice: case SYS_tee: case SYS_vmsplice: case SYS_copy_file_range:
        case SYS_getdents: case SYS_ftruncate:
        case SYS_fchdir: case SYS_flock: case SYS_sync: case SYS_syncfs:
        case SYS_readahead: case SYS_fadvise64:
        case SYS_eventfd: case SYS_eventfd2:
        case SYS_timerfd_create: case SYS_timerfd_settime: case SYS_timerfd_gettime:
        case SYS_inotify_init: case SYS_inotify_init1: case SYS_inotify_add_watch: case SYS_inotify_rm_watch:
        case SYS_io_uring_setup: case SYS_io_uring_register: case SYS_io_uring_enter:
        case SYS_close_range:
        case SYS_memfd_create:
        case SYS_creat: case SYS_openat2: case SYS_truncate:
        case SYS_unlink: case SYS_unlinkat: case SYS_rename: case SYS_renameat: case SYS_renameat2:
        case SYS_mkdir: case SYS_mkdirat: case SYS_rmdir:
        case SYS_link: case SYS_linkat: case SYS_symlink: case SYS_symlinkat:
        case SYS_mknod: case SYS_mknodat:
        case SYS_chmod: case SYS_fchmod: case SYS_fchmodat:
        case SYS_chown: case SYS_lchown: case SYS_fchown: case SYS_fchownat:
        case SYS_utime: case SYS_utimes: case SYS_utimensat: case SYS_futimesat:
        case SYS_setxattr: case SYS_lsetxattr: case SYS_fsetxattr:
        case SYS_removexattr: case SYS_lremovexattr: case SYS_fremovexattr:
        case SYS_mount: case SYS_umount2: case SYS_pivot_root: case SYS_chroot:
        case SYS_statx:
        case SYS_getxattr: case SYS_lgetxattr: case SYS_fgetxattr:
        case SYS_listxattr: case SYS_llistxattr: case SYS_flistxattr:
        case SYS_execve: case SYS_execveat:
            return SyscallRoute(SyscallCategory::UNSUPPORTED,SyscallAction::REJECT,
                                "Unsupported virtual I/O or descriptor lifecycle");

        case SYS_rseq:
            return SyscallRoute(SyscallCategory::EMULATED, SyscallAction::EMULATE,
                                "rseq syscall unavailable in this execution environment");

        case SYS_set_robust_list: case SYS_get_robust_list:
        case SYS_clone: case SYS_clone3: case SYS_set_tid_address: case SYS_rt_sigprocmask: case SYS_sigaltstack:
        case SYS_exit: case SYS_getpid: case SYS_gettid:
            return SyscallRoute(SyscallCategory::EMULATED, SyscallAction::EMULATE,
                                "Modeled logical thread lifecycle or identity");

        // Blocked syscalls
        case SYS_exit_group:
            return SyscallRoute(SyscallCategory::BLOCKED,
                               SyscallAction::BLOCK,
                               "exit_group - blocked");

        // Special handling
        case SYS_futex:
        case SYS_mmap:
            return SyscallRoute(SyscallCategory::SPECIAL,
                               SyscallAction::DEFERRED,
                               "Special handling required");

        // Native queries are stable within the owned tracee. Private-memory
        // effects are included in capture; argument restrictions run on entry.
        case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
        case SYS_brk: case SYS_mprotect: case SYS_munmap: case SYS_madvise:
            return SyscallRoute(SyscallCategory::PASSTHROUGH,
                               SyscallAction::EXECUTE,
                               "Allowed owned-process query or private memory operation");
        default:
            return SyscallRoute();
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
        case SyscallAction::REJECT:
            throw std::runtime_error(std::string(route.description)+": syscall "+std::to_string(syscall_nr));
        case SyscallAction::EMULATE:
            if (syscall_nr == SYS_prlimit64 && (info.args[2] ||
                (static_cast<int32_t>(info.args[0]) != 0 &&
                 static_cast<int32_t>(info.args[0]) != logical_process_id(ptmc_state.cursor))))
                throw std::runtime_error("Only owned-process prlimit64 queries are supported");
            if (syscall_nr == SYS_clone) validate_logical_clone(info.args[0]);
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
            if (syscall_nr == SYS_mprotect || syscall_nr == SYS_munmap || syscall_nr == SYS_madvise)
                validate_guest_mapping_range(info.args[0],info.args[1]);
            if (syscall_nr == SYS_mprotect &&
                ((info.args[2] && !(info.args[2] & PROT_READ)) ||
                 (info.args[2] & ~uint64_t(PROT_READ|PROT_WRITE|PROT_EXEC))))
                throw std::runtime_error("Only readable or guard protections are supported");
            if (syscall_nr == SYS_mprotect && info.args[2] == PROT_NONE)
                validate_anonymous_mapping_range(pid, info.args[0], info.args[1]);
            if (syscall_nr == SYS_madvise) {
                const int32_t advice = static_cast<int32_t>(info.args[2]);
                if (advice != MADV_DONTNEED && advice != MADV_NOHUGEPAGE)
                    throw std::runtime_error("Only private anonymous MADV_DONTNEED/MADV_NOHUGEPAGE are supported");
                validate_anonymous_mapping_range(pid, info.args[0], info.args[1]);
            }
            return true;
        default:
            throw std::runtime_error("Invalid syscall action");
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
        case SYS_rseq:
            info.rval = -ENOSYS;
            tracee_set_rax(pid,info.rval);
            return CKPT_YES;
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
        case SYS_getsockname:
        case SYS_getpeername:
        case SYS_poll:
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
        case SYS_ioctl:
        case SYS_newfstatat:
        case SYS_getcwd:
        case SYS_lstat:
        case SYS_readlink:
        case SYS_readlinkat:
        case SYS_getdents64:
        case SYS_access:
        case SYS_faccessat:
        case SYS_faccessat2:
            return handle_fs_exit(pid, info);

        case SYS_getpid:
            info.rval=logical_process_id(ptmc_state.cursor);
            tracee_set_rax(pid,info.rval);return CKPT_YES;

        // Thread syscalls
        case SYS_clone:
        case SYS_clone3:
        case SYS_gettid:
        case SYS_set_tid_address:
        case SYS_set_robust_list:
        case SYS_get_robust_list:
        case SYS_rt_sigprocmask:
        case SYS_sigaltstack:
        case SYS_exit:
        case SYS_exit_group:
            return handle_thread_exit(pid, info);

        // Futex
        case SYS_futex:
            return handle_futex_exit(pid, info);

        // Time syscalls
        case SYS_gettimeofday:
        case SYS_nanosleep:
        case SYS_pselect6:
        case SYS_clock_nanosleep:
            return handle_time_exit(pid, info);

        case SYS_clock_gettime: case SYS_clock_getres: case SYS_time:
        case SYS_getrandom: case SYS_sysinfo: case SYS_sched_getaffinity: case SYS_rt_sigaction:
        case SYS_setitimer: case SYS_getitimer: case SYS_umask:
        case SYS_getrlimit: case SYS_prlimit64: case SYS_prctl: {
            const long result=emu_process_syscall(info);
            tracee_set_rax(pid,result);info.rval=result;
            return CKPT_YES;
        }

        case SYS_brk: {
            auto& state = ptmc_state.running_state.child[ptmc_state.cursor];
            // Linux returns the unchanged break on failure and the current
            // break for a query. Only a successful shrink removes mappings.
            if (info.args[0] && info.rval == info.args[0] && info.rval < state.brk) {
                const uint64_t start = (info.rval + 4095) & ~uint64_t(4095);
                const uint64_t end = (state.brk + 4095) & ~uint64_t(4095);
                ptmc_state.fs_states[ptmc_state.cursor].remove_virtual_file_mapping(start, end);
            }
            state.brk = info.rval;
            return CKPT_NO;
        }

        // Default - no checkpoint needed
        default:
            return CKPT_NO;
    }
}

void SyscallDispatcher::handle_mmap_enter(pid_t pid, syscall_info& info)
{
    vfs_mmap_state_ = {};
    // Anonymous zero-length probes cannot create a mapping. File mappings
    // must retain descriptor validation before EINVAL (a bad fd gives EBADF).
    // Huge-page setup can fail before the length check; keep it unsupported.
    // Do not execute a host syscall or touch the reserved injection mapping.
    if (!info.args[1] && (info.args[3] & MAP_ANONYMOUS) && !(info.args[3] & MAP_HUGETLB)) {
        vfs_mmap_state_.active = true;
        vfs_mmap_state_.error = -EINVAL;
        tracee_set_orig_rax(pid, -1);
        return;
    }
    const int flags=static_cast<int>(info.args[3]);
    // Linux ignores these historical loader flags; admitting them does not
    // introduce shared mappings or change the captured backing contract.
    constexpr int supported_flags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_FIXED_NOREPLACE|
        MAP_STACK|MAP_NORESERVE|MAP_DENYWRITE|MAP_EXECUTABLE;
    if (!(flags&MAP_PRIVATE) || (flags&~supported_flags) ||
        (info.args[2] && !(info.args[2]&PROT_READ)) ||
        (!info.args[2] && !(flags&MAP_ANONYMOUS)) ||
        (info.args[2]&~uint64_t(PROT_READ|PROT_WRITE|PROT_EXEC)))
        throw std::runtime_error("Only readable private mappings or anonymous guards are supported");
    if (flags&(MAP_FIXED|MAP_FIXED_NOREPLACE)) validate_guest_mapping_range(info.args[0],info.args[1]);
    if (flags & MAP_ANONYMOUS) return; // fd is ignored by the kernel.
    auto& fs=ptmc_state.fs_states[ptmc_state.cursor];
    auto manager=ptmc_state.fd_managers[ptmc_state.cursor];
    if (!manager) throw std::runtime_error("Missing virtual descriptor manager");
    const int fd=static_cast<int>(info.args[4]);
    vfs_mmap_state_.active=true;
    auto reject=[&](long error) {
        vfs_mmap_state_.error=error;
        tracee_set_orig_rax(pid,-1);
    };
    if (!manager->is_allocated(fd)) {reject(-EBADF);return;}
    if (manager->get_fd_type(fd)!=FdType::FILE) {reject(-ENODEV);return;}
    if (!(flags&MAP_PRIVATE) || (flags&MAP_SHARED))
        throw std::runtime_error("Shared virtual file mappings are unsupported");
    if (!info.args[1] || info.args[5]%4096 || info.args[5]>uint64_t(INT64_MAX)) {reject(-EINVAL);return;}
    if ((manager->get_status_flags(fd)&O_ACCMODE)==O_WRONLY) {reject(-EACCES);return;}
    const auto file=fs.open_files.find(fd);
    if (file==fs.open_files.end()) throw std::runtime_error("Mapped descriptor has no file payload");
    const auto& node=fs.filesystem.at(file->second.path);
    const size_t tail=node.content.size()%4096;
    const size_t backing=node.content.size()+(tail?4096-tail:0);
    if (info.args[5]>=backing || info.args[1]>backing-info.args[5])
        throw std::runtime_error("Virtual mapping with SIGBUS-backed pages is unsupported");
    vfs_mmap_state_.fd=fd;
    vfs_mmap_state_.offset=info.args[5];
    vfs_mmap_state_.length=info.args[1];
    vfs_mmap_state_.protection=info.args[2];
    vfs_mmap_state_.flags=flags;
    user_regs_struct registers;
    ptrace_right(PTRACE_GETREGS,pid,nullptr,&registers);
    registers.rdx=info.args[2]|PROT_WRITE;
    registers.r10=flags|MAP_ANONYMOUS;
    registers.r8=uint64_t(-1);
    registers.r9=0;
    ptrace_right(PTRACE_SETREGS,pid,nullptr,&registers);
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

    auto& fs = ptmc_state.fs_states[ptmc_state.cursor];
    if (info.nr == SYS_munmap && static_cast<int64_t>(info.rval) == 0) {
        validate_guest_mapping_range(info.args[0], info.args[1]);
        fs.remove_virtual_file_mapping(info.args[0], info.args[0] + ((info.args[1] + 4095) & ~uint64_t(4095)));
    }
    // Account for anonymous MAP_FIXED replacements as well as file mappings.
    // The returned extent is authoritative; failed syscalls never trim state.
    if (info.nr == SYS_mmap && static_cast<int64_t>(info.rval) >= 0 &&
        (!vfs_mmap_state_.active || !vfs_mmap_state_.error)) {
        validate_guest_mapping_range(info.rval, info.args[1]);
        fs.remove_virtual_file_mapping(info.rval, info.rval + ((info.args[1] + 4095) & ~uint64_t(4095)));
    }
    if (info.nr == SYS_mmap && vfs_mmap_state_.active) {
        const auto operation=vfs_mmap_state_;
        vfs_mmap_state_={};
        if (operation.error) {
            info.rval=operation.error;tracee_set_rax(pid,info.rval);
        } else {
            user_regs_struct registers;
            ptrace_right(PTRACE_GETREGS,pid,nullptr,&registers);
            registers.rdx=operation.protection;registers.r10=operation.flags;
            registers.r8=operation.fd;registers.r9=operation.offset;
            ptrace_right(PTRACE_SETREGS,pid,nullptr,&registers);
            if (static_cast<int64_t>(info.rval)>=0) {
                const auto& description=fs.open_files.at(operation.fd);
                const auto& content=fs.filesystem.at(description.path).content;
                const size_t offset=operation.offset;
                const size_t length=offset<content.size()?std::min(operation.length,content.size()-offset):0;
                if (length && !guest_write_exact(pid,reinterpret_cast<void*>(info.rval),content.data()+offset,length))
                    throw std::runtime_error("Virtual mapping copyout failed");
                if (!(operation.protection&PROT_WRITE) &&
                    tracee_do_syscall(pid,SYS_mprotect,info.rval,operation.length,operation.protection,0,0,0)!=0)
                    throw std::runtime_error("Virtual mapping protection restore failed");
                fs.add_virtual_file_mapping(info.rval, info.rval + ((operation.length + 4095) & ~uint64_t(4095)));
            }
        }
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
