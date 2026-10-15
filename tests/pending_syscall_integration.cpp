#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include "engine/pending_syscall.h"
#include "guest.h"
#include "state_store.h"
#include "cereal/archives/binary.hpp"
#include <filesystem>
#include <sstream>
#include <limits>
#include <time.h>
#include <sys/mman.h>
#include <sys/rseq.h>
#include <cstdio>
#include <stdexcept>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static timespec sleep_request{2, 1234};
static void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main(int argc, char** argv)
{
    pid_t child = -1;
    char directory[] = "/tmp/detsim-pending-syscall-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    try {
        loglevel = 4;
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        int word = 7;
        child = fork();
        require(child >= 0, "fork");
        if (!child) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (__rseq_size && syscall(SYS_rseq,
                static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(3);
            if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(4);
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(5);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            raise(SIGSTOP);
            syscall(SYS_futex, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 7, 0, 0, 0);
            syscall(SYS_getpid);
            syscall(SYS_nanosleep, &sleep_request, nullptr);
            syscall(SYS_getpid);
            _exit(0);
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "initial stop");
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0,
                "ptrace options");
        ptmc_state.cursor = 0;
        ptmc_state.pids[0] = child;
        ptmc_state.fd_managers[0] = std::make_shared<FdManager>();
        StateStore::Config config;
        if (argc == 2 && std::string(argv[1]) == "--tiny-cache")
            config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        auto& state = ptmc_state.running_state.child[0];
        state.add_thread(child, 0, 0, 0, true);
        thread::init_all(ptmc_state);
        ExecutionEngine engine;
        syscall_info info{};
        require(engine.execute_syscall(child, info) == 1, "WAIT must produce a checkpoint");
        user_regs_struct regs{};
        require(ptrace(PTRACE_GETREGS, child, nullptr, &regs) == 0, "read parked registers");
        require(static_cast<long>(regs.rax) == -ENOSYS,
                "WAIT published a successful return before completion");
        require(ptmc_state.thread_blocked[0][0], "WAIT was not blocked");
        require(state.threads[0].pending_syscall &&
                state.threads[0].pending_syscall->phase == PendingPhase::Waiting,
                "WAIT has no unfinished call");
        state.si[0] = info;
        tracee_state waiting(0, state.si);
        const auto waiting_hash = waiting.save(child);
        tracee_state disk_waiting(waiting_hash);
        require(disk_waiting.threads[0].pending_syscall &&
                disk_waiting.threads[0].pending_syscall->args[0] == reinterpret_cast<uint64_t>(&word),
                "disk archive lost pending args");
        auto branch = waiting;
        branch.threads[0].pending_syscall->args[2]++;
        require(!branch.metadata_equal(waiting) && waiting.threads[0].pending_syscall->args[2] == 7,
                "pending arguments absent from identity or branch copy aliases");
        auto invalid = waiting;
        invalid.threads[0].pending_syscall->result = 42;
        bool invalid_rejected = false;
        try { validate_pending_syscalls(invalid); }
        catch (const std::runtime_error&) { invalid_rejected = true; }
        require(invalid_rejected, "Waiting accepted a committed result");
        invalid = waiting;
        invalid.threads[0].pending_syscall.reset();
        invalid_rejected = false;
        try { validate_pending_syscalls(invalid); }
        catch (const std::runtime_error&) { invalid_rejected = true; }
        require(invalid_rejected, "queue without pending call accepted");
        bool rejected = false;
        try { engine.execute_syscall(child, info); }
        catch (const std::runtime_error&) { rejected = true; }
        user_regs_struct after{};
        require(rejected && ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 && after.rip == regs.rip,
                "blocked actor advanced user instructions");
        ptmc_state.time[0] = {10, 250000};
        timespec requested{0, 500000001};
        const auto deadline = make_pending_deadline(0, CLOCK_MONOTONIC,
            requested.tv_sec, requested.tv_nsec, true, -ETIMEDOUT);
        require(deadline.sec == 10 && deadline.nsec == 750000001,
                "relative timeout lost nanosecond precision");
        const auto saturated = make_pending_deadline(0, CLOCK_MONOTONIC,
            std::numeric_limits<int64_t>::max(), 999999999, true, -ETIMEDOUT);
        require(saturated.sec == 9223372036LL && saturated.nsec == 854775807,
                "relative timeout did not saturate at KTIME_MAX");
        const auto absolute_saturated = make_pending_deadline(0, CLOCK_REALTIME,
            9223372036LL, 0, false, -ETIMEDOUT);
        require(absolute_saturated.sec == saturated.sec && absolute_saturated.nsec == saturated.nsec,
                "absolute timeout disagrees with Linux ktime_set saturation");
        set_pending_deadline(0, 1, deadline);
        requested = {99, 0};
        tracee_state timed(0, state.si);
        const auto timed_hash = timed.save(child);
        tracee_state disk_timed(timed_hash);
        auto changed_deadline = timed;
        changed_deadline.threads[0].pending_syscall->deadline->nsec++;
        require(!changed_deadline.metadata_equal(timed), "deadline missing from state identity");
        require(!pending_timeout_due(0, 1) && !expire_pending_syscall(0, 1),
                "deadline expired before modeled time reached it");
        ptmc_state.time[0] = {10, 750000};
        require(!expire_pending_syscall(0, 1), "deadline rounded down by a nanosecond");
        ptmc_state.time[0] = {10, 750001};
        refresh_pending_syscalls();
        require(state.threads[0].pending_syscall->phase == PendingPhase::Waiting,
                "readiness refresh automatically expired a deadline");
        require(pending_timeout_due(0, 1) && expire_pending_syscall(0, 1) &&
                !expire_pending_syscall(0, 1), "controlled expiry was not a single grant");
        require(!state.futex_state->is_thread_waiting(1) &&
                engine.execute_syscall(child, info) == 1 && static_cast<long>(info.rval) == -ETIMEDOUT,
                "deadline completion did not remove wait and return timeout");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 && after.rip == regs.rip,
                "deadline completion resumed user instructions");
        disk_timed.recover_running_state(0, timed_hash);
        require(state.threads[0].pending_syscall->deadline->nsec == 750000001 &&
                state.threads[0].pending_syscall->phase == PendingPhase::Waiting &&
                ptmc_state.time[0].tv_sec == 10 && ptmc_state.time[0].tv_usec == 250000 &&
                !pending_timeout_due(0, 1), "timed source restore lost frozen deadline or clock");
        disk_waiting.recover_running_state(0, waiting_hash);
        require(!state.threads[0].pending_syscall->deadline,
                "untimed source restore retained a later deadline");
        ready_syscall(0, 1, -ETIMEDOUT);
        require(!state.futex_state->is_thread_waiting(1) && !ptmc_state.thread_blocked[0][0],
                "controlled completion left its futex registration queued");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 &&
                after.rip == regs.rip && static_cast<long>(after.rax) == -ENOSYS,
                "grant published a result before completion");
        tracee_state granted(0, state.si);
        const auto granted_hash = granted.save(child);
        tracee_state disk_granted(granted_hash);
        disk_granted.recover_running_state(0, granted_hash);
        require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_futex &&
                static_cast<long>(info.rval) == -ETIMEDOUT,
                "controlled negative completion lost its result");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 &&
                after.rip == regs.rip && static_cast<long>(after.rax) == -ETIMEDOUT,
                "controlled completion resumed user instructions");
        disk_waiting.recover_running_state(0, waiting_hash);
        require(state.threads[0].pending_syscall->phase == PendingPhase::Waiting &&
                state.threads[0].pending_syscall->result == 0 && state.futex_state->is_thread_waiting(1),
                "restoring waiting branch retained a granted completion");
        require(state.futex_state->wake_thread(1), "wake pending waiter");
        refresh_futex_pending(0);
        require(!ptmc_state.thread_blocked[0][0] &&
                state.threads[0].pending_syscall->phase == PendingPhase::Ready,
                "wake failed to grant completion");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 &&
                after.rip == regs.rip && static_cast<long>(after.rax) == -ENOSYS,
                "wake prematurely completed syscall");
        tracee_state ready(0, state.si);
        const auto ready_hash = ready.save(child);
        require(ready_hash != waiting_hash && !ready.metadata_equal(waiting),
                "waiting and ready states have identical identity");
        tracee_state disk_ready(ready_hash);
        for (int replay = 0; replay < 3; ++replay) {
            disk_waiting.recover_running_state(0, waiting_hash);
            disk_waiting.recover_running_state(0, waiting_hash);
            require(ptmc_state.thread_blocked[0][0] && state.futex_state->is_thread_waiting(1),
                    "waiting restore or same-hash restore lost block");
            require(state.futex_state->wake_thread(1), "replay wake");
            refresh_futex_pending(0);
            ptmc_state.n_choose = 7; ptmc_state.choose = 3;
            require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_futex && info.rval == 0 &&
                    !state.threads[0].pending_syscall && !ptmc_state.thread_blocked[0][0] &&
                    ptmc_state.n_choose == 0 && ptmc_state.choose == -1,
                    "explicit completion failed");
            require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 &&
                    after.rip == regs.rip && after.rax == 0,
                    "completion resumed user instructions");
        }
        disk_ready.recover_running_state(0, ready_hash);
        require(state.threads[0].pending_syscall->phase == PendingPhase::Ready &&
                !ptmc_state.thread_blocked[0][0], "ready restore lost eligibility");
        require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_futex && info.rval == 0,
                "restored ready completion failed");
        require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_getpid && info.rval == 1,
                "user execution did not resume after completion");
        require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_nanosleep &&
                state.threads[0].pending_syscall &&
                state.threads[0].pending_syscall->reason == PendingReason::Timer &&
                state.threads[0].pending_syscall->phase == PendingPhase::Waiting,
                "positive nanosleep did not register an unfinished timer");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &regs) == 0 &&
                static_cast<long>(regs.rax) == -ENOSYS, "sleep exposed a premature return");
        state.si[0] = info;
        tracee_state timer(0, state.si);
        const auto timer_hash = timer.save(child);
        tracee_state disk_timer(timer_hash);
        for (int replay = 0; replay < 2; ++replay) {
            disk_timer.recover_running_state(0, timer_hash);
            const auto deadline = *state.threads[0].pending_syscall->deadline;
            require(!expire_pending_syscall(0, 1), "sleep expired before its deadline");
            ptmc_state.time[0] = {deadline.sec, (deadline.nsec + 999) / 1000};
            if (ptmc_state.time[0].tv_usec == 1000000) {
                ++ptmc_state.time[0].tv_sec;
                ptmc_state.time[0].tv_usec = 0;
            }
            refresh_pending_syscalls();
            require(state.threads[0].pending_syscall->phase == PendingPhase::Waiting,
                    "timer readiness silently advanced execution");
            require(expire_pending_syscall(0, 1) && engine.execute_syscall(child, info) == 1 &&
                    info.nr == SYS_nanosleep && info.rval == 0,
                    "controlled sleep completion failed");
            require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 && after.rip == regs.rip && after.rax == 0,
                    "sleep completion executed user instructions");
        }
        require(engine.execute_syscall(child, info) == 1 && info.nr == SYS_getpid,
                "user execution did not resume after sleep completion");
        kill(child, SIGKILL); waitpid(child, nullptr, 0); child = -1;
        StateStore::instance().shutdown();
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(directory);
        std::puts("PASS pending registration, wake, explicit completion, archive identity and repeated restore");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(directory);
        std::fprintf(stderr, "pending syscall: %s\n", error.what());
        return 1;
    }
}
