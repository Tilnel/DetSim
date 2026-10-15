#include "sync/futexstate.h"
#include "syscall/handlers.h"
#include <sys/mman.h>
#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include <cstdio>
#include <cstdlib>
#include <linux/futex.h>

#include <stdexcept>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

// Each case runs in a dedicated process group with a parent-enforced deadline.
// Even the pre-fix infinite atom cannot retain a tracee or hang the test suite.
static void run_case(int mode) {
    loglevel = 4;
    alignas(4) int words[2] = {-1, 0};
    timespec timeout{1, 0};
    int op = FUTEX_WAKE_PRIVATE, val = 1, val3 = 0;
    uint64_t arg4 = 0;
    int *mapping = static_cast<int *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(mapping != MAP_FAILED, "map futex page");
    *mapping = 73;
    int *addr = words;
    int *addr2 = words + 1;
    if (mode == 1) { op = FUTEX_WAIT_PRIVATE; val = -1; }
    if (mode == 2) { op = FUTEX_CMP_REQUEUE_PRIVATE; arg4 = 2; val3 = 7; }
    if (mode == 3) { op = FUTEX_CMP_REQUEUE_PRIVATE; arg4 = 1; val3 = -1; }
    if (mode == 4) { op = FUTEX_REQUEUE_PRIVATE; arg4 = 2; addr2 = words; }
    if (mode == 5) { op = FUTEX_WAIT_PRIVATE; addr = reinterpret_cast<int *>(0x1000); val = 0; }
    if (mode == 6) { op = FUTEX_WAIT_PRIVATE; val = -1; arg4 = reinterpret_cast<uint64_t>(&timeout); }
    if (mode == 7) { op = FUTEX_WAIT_BITSET_PRIVATE; val = -1; val3 = 0; }
    if (mode == 8) { op = FUTEX_LOCK_PI_PRIVATE; }
    if (mode == 11) val = 0;
    if (mode == 12) val = -1;
    if (mode == 9) { op = FUTEX_WAKE_BITSET_PRIVATE; val3 = 2; }
    pid_t child = fork();
    require(child >= 0, "fork tracee");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
        raise(SIGSTOP);
        if (mode == 10) syscall(SYS_madvise, mapping, 4096, MADV_DONTNEED);
        else syscall(SYS_futex, addr, op, val, arg4, addr2, val3);
        _exit(0);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "initial stop");
    require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
                   PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
    ptmc_state.cursor = 0;
    ptmc_state.pids[0] = child;
    ptmc_state.current_thread_idx[0] = 0;
    auto &state = ptmc_state.running_state.child[0];
    state.add_thread(child, 0, 0, 0, true);
    state.futex_state.emplace();
    auto &futex = *state.futex_state;
    uint64_t address = reinterpret_cast<uint64_t>(mode == 10 ? mapping : words), value = 7;
    if (mode != 1 && mode != 5 && mode != 6 && mode != 7 && mode != 8) {
        for (int tid = 2; tid <= 4; ++tid) {
            thread_state waiter{};
            waiter.tid = tid;
            // Synthetic peers are never resumed; this handler-only fixture
            // gives each a distinct scheduling binding without host tasks.
            waiter.physical_tid = child + tid;
            waiter.alive = true;
            waiter.pending_syscall.emplace();
            auto& pending = *waiter.pending_syscall;
            pending.nr = SYS_futex;
            pending.args[0] = address;
            pending.args[1] = FUTEX_WAIT_BITSET_PRIVATE;
            pending.args[2] = 7;
            pending.args[5] = tid == 3 ? 2 : 1;
            waiter.regs.orig_rax = -1;
            waiter.regs.rax = -ENOSYS;
            state.si[tid - 1].nr = pending.nr;
            state.si[tid - 1].rval = -ENOSYS;
            std::copy(std::begin(pending.args), std::end(pending.args), state.si[tid - 1].args);
            state.threads.push_back(waiter);
            require(futex.handle_futex(tid, address, FUTEX_WAIT_BITSET_PRIVATE,
                                      7, 0, 0, tid == 3 ? 2 : 1, &value) == 0, "seed wait");
            ptmc_state.thread_blocked[0][tid - 1] = true;
        }
    }
    thread::init_all(ptmc_state);
    ExecutionEngine engine;
    syscall_info info{};
    bool rejected = false;
    try {
        const int checkpoint = engine.execute_syscall(child, info);
        require(checkpoint == 1, "waiting successor must be checkpointed");
    } catch (const std::runtime_error &) { rejected = true; }
    if (mode == 10) {
        require(rejected && futex.get_waiters(address).size() == 3 &&
            ptmc_state.thread_blocked[0][1] && *mapping == 73,
            "unsupported madvise must preserve waiters and tracer memory");
    } else if (mode == 8) require(rejected, "unsupported futex must fail checker");
    else {
        require(!rejected, "supported operation rejected");
        long result = static_cast<long>(info.rval);
        if (mode == 1) require(result == -ENOSYS && futex.is_thread_waiting(1) &&
            ptmc_state.thread_blocked[0][0] && state.threads[0].pending_syscall &&
            state.threads[0].pending_syscall->phase == PendingPhase::Waiting,
            "32-bit signed wait must remain unfinished until wake");
        if (mode == 6) {
            require(result == -ENOSYS && state.threads[0].pending_syscall &&
                    state.threads[0].pending_syscall->deadline &&
                    !expire_pending_syscall(0,1),"timed WAIT did not retain its future deadline");
            const auto deadline=*state.threads[0].pending_syscall->deadline;
            ptmc_state.time[0].tv_sec=deadline.sec+1;
            require(expire_pending_syscall(0,1) && !futex.is_thread_waiting(1),"due WAIT did not grant timeout result");
            user_regs_struct before{},after{};
            require(ptrace(PTRACE_GETREGS,child,nullptr,&before)==0,"timed completion registers");
            require(engine.execute_syscall(child,info)==1 && static_cast<long>(info.rval)==-ETIMEDOUT &&
                    !state.threads[0].pending_syscall,"timed WAIT completion lost ETIMEDOUT");
            require(ptrace(PTRACE_GETREGS,child,nullptr,&after)==0 && after.rip==before.rip,
                    "timed completion executed user code");
        }
        for (size_t i = 1; i < state.threads.size(); ++i) {
            const auto& pending = state.threads[i].pending_syscall;
            require(pending && pending->phase == (futex.is_thread_waiting(i + 1) ?
                    PendingPhase::Waiting : PendingPhase::Ready),
                    "wake must ready only removed pending calls");
            require(state.si[i].rval == static_cast<uintptr_t>(-ENOSYS),
                    "wake prematurely published a completed wait");
        }
        if (mode == 0 || mode == 4 || mode == 11 || mode == 12) require(result == (mode == 4 ? 3 : 1) && !futex.is_thread_waiting(2)
            && !ptmc_state.thread_blocked[0][1] && ptmc_state.thread_blocked[0][2] && ptmc_state.thread_blocked[0][3], "wake must unblock only removed waiter");
        if (mode == 2) require(result == -EAGAIN && futex.get_waiters(address).size() == 3
            && ptmc_state.thread_blocked[0][1], "failed CMP_REQUEUE must preserve queues");
        if (mode == 3) require(result == 2 && !ptmc_state.thread_blocked[0][1]
            && ptmc_state.thread_blocked[0][2] && ptmc_state.thread_blocked[0][3]
            && futex.get_wait_address(3) == address + 4 && futex.get_wait_address(4) == address,
            "requeue must use arg4 count and arg6 source comparison");
        if (mode == 5) require(result == -EFAULT && !futex.is_thread_waiting(1), "unreadable futex must not be zero-filled");
        if (mode == 7) require(result == -EINVAL && !futex.is_thread_waiting(1), "zero bitset invalid");
        if (mode == 9) require(result == 1 && ptmc_state.thread_blocked[0][1]
            && !ptmc_state.thread_blocked[0][2] && ptmc_state.thread_blocked[0][3], "bitset wake unblocks matched waiter only");
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

int main() {
    const char *names[] = {"wake flags", "signed wait checkpoint", "cmp mismatch", "requeue ABI",
        "same-address requeue", "bad address", "timed wait registration and controlled expiry", "zero bitset", "PI rejection", "bitset wake flags", "madvise preserves waiters", "zero wake count", "negative wake count"};
    int failures = 0;
    for (int mode = 0; mode < 13; ++mode) {
        fflush(nullptr);
        pid_t test = fork();
        if (test == 0) {
            setpgid(0, 0);
            try { run_case(mode); _exit(0); }
            catch (const std::exception &e) { fprintf(stderr, "%s: %s\n", names[mode], e.what()); _exit(1); }
        }
        require(test > 0, "fork test");
        setpgid(test, test);
        int status = 0;
        bool done = false;
        for (int ticks = 0; ticks < 200; ++ticks) {
            if (waitpid(test, &status, WNOHANG) == test) { done = true; break; }
            usleep(10000);
        }
        kill(-test, SIGKILL);
        if (!done) waitpid(test, &status, 0);
        bool passed = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("%s: %s\n", names[mode], passed ? "PASS" : "FAIL (error or 2s timeout)");
        failures += !passed;
    }
    return failures ? 1 : 0;
}
