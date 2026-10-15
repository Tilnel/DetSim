/*
 * exec_engine.cpp - Integrated execution engine
 */

#include "exec_engine.h"
#include "thread_manager.h"
#include "thread_pool.h"
#include "signal_handler.h"
#include "syscall/dispatcher.h"
#include "guest.h"
#include "log_wrapper.h"
#include "monitor.h"
#include "state/state.h"
#include "proc_status.h"
#include <linux/audit.h>
#include <climits>
#include <stdexcept>
#include <sys/ptrace.h>
#include <sys/wait.h>

#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)
#define CKPT_YES 1
#define CKPT_NO 0
#define CKPT_EXIT 2
#define CKPT_STOP 3
#define CKPT_DISCARD 4

ExecutionEngine::ExecutionEngine()
{
}

void ExecutionEngine::init_tracee(int tracee_idx)
{
    // Thread managers are initialized globally
    LOG_DEBUG("ExecutionEngine initialized for tracee %d", tracee_idx);
}

bool ExecutionEngine::handle_signal(pid_t pid, int wstatus)
{
    SignalResult result = sig::process(pid, wstatus);

    if (sig::should_stop(result)) {
        LOG_ERROR("Fatal signal received, stopping tracee %d", pid);
        ptmc_state.status[ptmc_state.cursor] = dstatus_crash(result.exit_status);
        return false;
    }

    if (result.action == SignalAction::SPECIAL && result.should_retry) {
        // Retry the syscall (e.g., after dismissing SIGURG)
        ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
        waitpid(pid, &wstatus, 0);
        return handle_signal(pid, wstatus);
    }

    return true;
}

int ExecutionEngine::do_single_syscall(pid_t pid, syscall_info& info)
{
    int wstatus = 0;
    struct ptrace_syscall_info psi;

    // User instructions may change memory before the next syscall entry, and
    // an unsupported handler can throw after that advance. Invalidate the
    // old snapshot image before the first resume, not after handler success.
    if (ptmc_state.cursor >= 0 && ptmc_state.cursor < NP)
        ptmc_state.tracee_image_dirty[ptmc_state.cursor] = true;
    complete_thread_start(ptmc_state.cursor, pid);
    // Entry
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
    waitpid(pid, &wstatus, 0);

    // Handle signals
    if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) != PTRACE_TRAP_SIG) {
        if (!handle_signal(pid, wstatus)) {
            return CKPT_STOP;
        }
    }

    // Get syscall info
    ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void*)sizeof(psi), &psi);
    if (psi.op != PTRACE_SYSCALL_INFO_ENTRY) {
        LOG_ERROR("Expected syscall entry, got op=%d", psi.op);
        return CKPT_STOP;
    }

    user_regs_struct entry_regs{};
    ptrace_right(PTRACE_GETREGS, pid, nullptr, &entry_regs);
    // The kernel may truncate the reported syscall number to 32 bits; reject
    // noncanonical register values before applying the model route.
    if (entry_regs.orig_rax != psi.entry.nr ||
        psi.arch != AUDIT_ARCH_X86_64 || psi.entry.nr > INT_MAX ||
        (psi.entry.nr & 0x40000000ULL))
        throw std::runtime_error("Unsupported syscall ABI or syscall number");
    info.nr = static_cast<int>(psi.entry.nr);
    for (int i = 0; i < 6; i++) {
        info.args[i] = psi.entry.args[i];
    }

    // Entry handlers (notably clone/futex) must see this syscall, not the
    // previous syscall or an uninitialized argument buffer.
    syscall_dispatch::enter(pid, info.nr, info);

    // Execute syscall
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);

    // Normal exit handling
    waitpid(pid, &wstatus, 0);
    if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) != PTRACE_TRAP_SIG) {
        if (!handle_signal(pid, wstatus)) {
            return CKPT_STOP;
        }
    }

    ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void*)sizeof(psi), &psi);
    if (psi.op != PTRACE_SYSCALL_INFO_EXIT) {
        LOG_ERROR("Expected syscall exit, got op=%d", psi.op);
        return CKPT_STOP;
    }

    info.rval = psi.exit.rval;
    return syscall_dispatch::exit(pid, info);
}

int ExecutionEngine::execute_syscall(pid_t pid, syscall_info& info)
{
    return do_single_syscall(pid, info);
}

// Forward declare auto_mode from config
extern int auto_mode;

ExecResult ExecutionEngine::execute_step(const sys_state& state, syscall_info& info)
{
    int tracee_idx = ptmc_state.cursor;

    // Get thread manager for this tracee
    ThreadManager* tm = thread::get_manager(tracee_idx);
    if (!tm) {
        LOG_ERROR("No ThreadManager for tracee %d", tracee_idx);
        return ExecResult(ExecStatus::ERROR, CKPT_STOP, "No thread manager");
    }

    // Sync thread manager from saved state
    tm->sync_from_tracee_state(ptmc_state.running_state.child[tracee_idx]);

    // Update thread blocked status from ptmc_state.thread_blocked
    for (int i = 0; i < static_cast<int>(tm->thread_count()); i++) {
        if (ptmc_state.thread_blocked[tracee_idx][i]) {
            auto* tinfo = tm->get_thread_info(i);
            if (tinfo && tinfo->state == ThreadState::RUNNING) {
                tm->mark_blocked(i);
                LOG_DEBUG("Thread %d marked as BLOCKED from ptmc_state", i);
            }
        }
    }

    // In manual mode, only execute the currently selected thread
    if (!auto_mode) {
        int thread_idx = ptmc_state.current_thread_idx[tracee_idx];
        
        // Validate thread index
        if (thread_idx < 0 || thread_idx >= static_cast<int>(tm->thread_count())) {
            LOG_WARN("Invalid thread index %d, using thread 0", thread_idx);
            thread_idx = 0;
            ptmc_state.current_thread_idx[tracee_idx] = 0;
        }

        const ThreadSchedInfo* tinfo = tm->get_thread_info(thread_idx);
        if (!tinfo || tinfo->state != ThreadState::RUNNING) {
            LOG_INFO("Thread %d is not runnable (state=%d)", thread_idx, 
                     tinfo ? static_cast<int>(tinfo->state) : -1);
            return ExecResult(ExecStatus::DISCARD, CKPT_DISCARD, "Selected thread not runnable");
        }

        pid_t pid = tinfo->physical_tid;
        if (pid <= 0) {
            LOG_WARN("Invalid physical_tid %d for thread %d", pid, thread_idx);
            return ExecResult(ExecStatus::ERROR, CKPT_STOP, "Invalid physical TID");
        }

        // Execute syscall on this specific thread
        int result = do_single_syscall(pid, info);

        switch (result) {
            case CKPT_NO:
                return ExecResult(ExecStatus::SUCCESS, CKPT_YES, "Syscall completed");
            case CKPT_DISCARD:
                return ExecResult(ExecStatus::DISCARD, CKPT_DISCARD, "Thread blocked");
            case CKPT_EXIT:
                return ExecResult(ExecStatus::EXIT, CKPT_EXIT, "Thread exited");
            case CKPT_STOP:
                return ExecResult(ExecStatus::CRASH, CKPT_STOP, "Process crashed");
            case CKPT_YES:
            default:
                return ExecResult(ExecStatus::SUCCESS, CKPT_YES, "Checkpoint saved");
        }
    }

    // Reference atom policy: one completed syscall per step, as in manual
    // mode. CKPT_NO is a handler hint, not permission to swallow scheduling
    // points indefinitely (e.g. receive retries or getpid busy loops).
    // This does not expose shared-memory interleavings inside user code.
    // Try each thread until a runnable one is found.
    int num_threads = tm->thread_count();
    if (num_threads == 0) {
        num_threads = 1;
    }

    for (int attempt = 0; attempt < num_threads; attempt++) {
        int thread_idx = tm->current_thread();

        const ThreadSchedInfo* tinfo = tm->get_thread_info(thread_idx);
        if (!tinfo || tinfo->state != ThreadState::RUNNING) {
            if (tm->next_runnable_thread() < 0) {
                break;
            }
            continue;
        }

        pid_t pid = tinfo->physical_tid;
        if (pid <= 0) {
            LOG_WARN("Invalid physical_tid %d for thread %d", pid, thread_idx);
            tm->next_runnable_thread();
            continue;
        }

        // Handlers address per-thread state through this selected actor.
        // Round-robin may have skipped the saved current thread.
        ptmc_state.current_thread_idx[tracee_idx] = thread_idx;
        ptmc_state.running_state.child[tracee_idx].current_thread_idx = thread_idx;

        int result = do_single_syscall(pid, info);

        switch (result) {
            case CKPT_NO:
                return ExecResult(ExecStatus::SUCCESS, CKPT_YES, "Syscall completed");
            case CKPT_DISCARD:
                return ExecResult(ExecStatus::DISCARD, CKPT_DISCARD, "Thread blocked");
            case CKPT_EXIT:
                return ExecResult(ExecStatus::EXIT, CKPT_EXIT, "Process exited");
            case CKPT_STOP:
                return ExecResult(ExecStatus::CRASH, CKPT_STOP, "Process crashed");
            case CKPT_YES:
            default:
                return ExecResult(ExecStatus::SUCCESS, CKPT_YES, "Checkpoint saved");
        }
    }

    if (tm->all_threads_exited()) {
        ptmc_state.status[tracee_idx] = dstatus_exit(0);
        return ExecResult(ExecStatus::EXIT, CKPT_EXIT, "All threads exited");
    }

    LOG_WARN("All threads blocked, possible deadlock");
    return ExecResult(ExecStatus::SUCCESS, CKPT_YES, "Possible deadlock");
}
void ExecutionEngine::on_thread_created(pid_t parent_pid, pid_t new_tid,
                                        uint64_t clone_flags, uint64_t stack_addr)
{
    int tracee_idx = ptmc_state.cursor;
    ThreadManager* tm = thread::get_manager(tracee_idx);

    if (tm) {
        int idx = tm->add_thread(new_tid, clone_flags, stack_addr, parent_pid, false);
        LOG_INFO("Added thread %d (TID %d) to tracee %d", idx, new_tid, tracee_idx);
    }

    // Also update tracee_state
    auto& child_state = ptmc_state.running_state.child[tracee_idx];
    child_state.add_thread(new_tid, clone_flags, stack_addr, parent_pid, false);
}

bool ExecutionEngine::on_thread_exit(pid_t pid, pid_t wait_result, int wstatus,
                                     syscall_info& si)
{
    if (!(wait_result == pid && (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)))) {
        return false;
    }

    int tracee_idx = ptmc_state.cursor;
    ThreadManager* tm = thread::get_manager(tracee_idx);

    if (tm) {
        tm->mark_exited(pid);
    }

    // Also update tracee_state
    auto& child_state = ptmc_state.running_state.child[tracee_idx];

    // Find and remove thread
    for (size_t i = 0; i < child_state.threads.size(); i++) {
        if (child_state.threads[i].physical_tid == pid) {
            int virtual_tid = i + 1;
            child_state.remove_thread(virtual_tid);
            ptmc_state.thread_exited[tracee_idx][i] = true;
            LOG_INFO("Thread %d (PID %d) exited", virtual_tid, pid);
            break;
        }
    }

    si.rval = 0;
    return true;
}

pid_t ExecutionEngine::get_current_tid(int tracee_idx) const
{
    ThreadManager* tm = thread::get_manager(tracee_idx);
    return tm ? tm->get_current_physical_tid() : -1;
}

int ExecutionEngine::get_current_thread_idx(int tracee_idx) const
{
    ThreadManager* tm = thread::get_manager(tracee_idx);
    return tm ? tm->current_thread() : 0;
}

bool ExecutionEngine::has_runnable_threads(int tracee_idx) const
{
    ThreadManager* tm = thread::get_manager(tracee_idx);
    return tm ? tm->has_runnable_threads() : false;
}

bool ExecutionEngine::all_threads_exited(int tracee_idx) const
{
    ThreadManager* tm = thread::get_manager(tracee_idx);
    return tm ? tm->all_threads_exited() : false;
}

bool ExecutionEngine::advance_to_next_thread(int tracee_idx)
{
    ThreadManager* tm = thread::get_manager(tracee_idx);
    if (!tm) return false;

    int next = tm->next_runnable_thread();
    return next >= 0;
}

/* ======================================================================
 * Global Execution Engine
 * ====================================================================== */

static ExecutionEngine g_engine;

namespace exec {

void init_all()
{
    thread::init_all(ptmc_state);
    sig::init();
    syscall_dispatch::init();

    for (int i = 0; i < NP; i++) {
        g_engine.init_tracee(i);
    }

    LOG_INFO("ExecutionEngine initialized for %d tracees", NP);
}

ExecutionEngine* get_engine()
{
    return &g_engine;
}

void cleanup()
{
    thread::cleanup_all();
}

} // namespace exec
