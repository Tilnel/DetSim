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
    ptmc_state.sock_states[0] = SockState(std::make_shared<FdManager>());
    int fd = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "emulated socket");
    int word = 0;
    timespec zero{};
    pid_t child = fork();
    require(child >= 0, "fork tracee");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
        raise(SIGSTOP);
        for (;;) {
            if (mode == 0)
                syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
            else if (mode == 1)
                syscall(SYS_recvfrom, fd, &word, sizeof(word), MSG_DONTWAIT, 0, 0);
            else if (mode == 2)
                syscall(SYS_nanosleep, &zero, nullptr);
            else if (mode == 3)
                syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &zero, nullptr);
            else
                syscall(SYS_getpid);
        }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "initial stop");
    require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
                   PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
    ptmc_state.cursor = 0;
    ptmc_state.pids[0] = child;
    ptmc_state.current_thread_idx[0] = 0;
    ptmc_state.running_state.child[0].add_thread(child, 0, 0, 0, true);
    thread::init_all(ptmc_state);
    ExecutionEngine engine;
    syscall_info info{};
    info.nr = SYS_getpid;
    info.args[1] = 127; // Unrelated previous syscall argument, not a futex opcode.
    auto_mode = 1;
    if (mode == 0) {
        require(engine.execute_syscall(child, info) == 1, "futex checkpoint");
        user_regs_struct regs{};
        require(ptrace(PTRACE_GETREGS, child, nullptr, &regs) == 0, "get regs");
        require(static_cast<long>(regs.orig_rax) == -1,
                "current futex args must suppress the real kernel syscall");
        require(info.nr == SYS_futex && info.args[0] == reinterpret_cast<uint64_t>(&word)
                && info.args[1] == FUTEX_WAKE_PRIVATE && info.args[2] == 1
                && info.args[3] == 0 && info.args[4] == 0 && info.args[5] == 0,
                "entry arguments were not captured faithfully");
        require(info.rval == 0, "wake without waiters");
    } else {
        for (int i = 0; i < 3; ++i) {
            auto result = engine.execute_step(ptmc_state.running_state, info);
            require(result.status == ExecStatus::SUCCESS && result.checkpoint_decision == 1,
                    "each retry or sleep must return a scheduling checkpoint");
            require(info.nr == (mode == 1 ? SYS_recvfrom : mode == 2 ? SYS_nanosleep : mode == 3 ? SYS_clock_nanosleep : SYS_getpid),
                    "unexpected syscall boundary");
            if (mode == 1) require(static_cast<long>(info.rval) == -EAGAIN, "empty receive must return EAGAIN");
        }
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

int main() {
    const char *names[] = {"current syscall entry args", "receive retry yields",
                           "nanosleep yields", "clock_nanosleep yields", "getpid busy loop yields"};
    int failures = 0;
    for (int mode = 0; mode < 5; ++mode) {
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
