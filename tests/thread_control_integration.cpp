#include "engine/thread_control.h"
#include "ncurses_ui.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static int stopped(pid_t tid)
{
    int status = 0;
    for (int tick = 0; tick < 500; ++tick) {
        const pid_t result = waitpid(tid, &status, __WALL | WNOHANG);
        if (result == tid) {
            require(WIFSTOPPED(status), "tracee exited instead of stopping");
            return status;
        }
        require(result == 0 || (result < 0 && errno == EINTR), "waitpid tracee");
        usleep(1000);
    }
    throw std::runtime_error("timed out waiting for ptrace stop");
}

template<class F> static void rejected(F action)
{
    bool failed = false;
    try { action(); } catch (const std::runtime_error &) { failed = true; }
    require(failed, "invalid thread barrier unexpectedly passed");
}

static void *spin(void *)
{
    for (;;) asm volatile("" ::: "memory");
}

static void run_case()
{
    pid_t child = fork();
    require(child >= 0, "fork tracee");
    if (!child) {
        if (__rseq_size && syscall(SYS_rseq,
                static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(9);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
        raise(SIGSTOP);
        pthread_t worker;
        if (pthread_create(&worker, nullptr, spin, nullptr)) _exit(3);
        for (;;) asm volatile("" ::: "memory");
    }
    stopped(child);
    require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
                   PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL) == 0, "set ptrace options");
    require(ptrace(PTRACE_CONT, child, nullptr, nullptr) == 0, "continue to clone");
    int status = stopped(child);
    require((status >> 16) == PTRACE_EVENT_CLONE, "expected clone event");
    unsigned long event = 0;
    require(ptrace(PTRACE_GETEVENTMSG, child, nullptr, &event) == 0, "clone event message");
    const pid_t worker = static_cast<pid_t>(event);
    stopped(worker);
    // Deliberately reverse enumeration order: the gate compares sets, not slots.
    const std::array<pid_t, 2> tids{worker, child};
    verify_stopped_thread_set(child, tids);
    rejected([&] { verify_stopped_thread_set(child, {}); });
    rejected([&] { verify_stopped_thread_set(-1, tids); });
    const std::array<pid_t, 2> duplicate{child, child};
    rejected([&] { verify_stopped_thread_set(child, duplicate); });
    const std::array<pid_t, 2> invalid{child, 0};
    rejected([&] { verify_stopped_thread_set(child, invalid); });
    const std::array<pid_t, 1> missing{child};
    rejected([&] { verify_stopped_thread_set(child, missing); });
    const std::array<pid_t, 1> foreign{getpid()};
    rejected([&] { verify_stopped_thread_set(getpid(), foreign); });

    user_regs_struct before{}, after{};
    require(ptrace(PTRACE_GETREGS, child, nullptr, &before) == 0, "leader registers before");
    require(ptrace(PTRACE_CONT, worker, nullptr, nullptr) == 0, "resume worker");
    // PTRACE_CONT returns only after removing the ptrace stop; GETREGS must fail
    // even if this thread has not yet been scheduled on another CPU.
    rejected([&] { verify_stopped_thread_set(child, tids); });
    require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0, "leader remains stopped");
    require(std::memcmp(&before, &after, sizeof before) == 0,
            "failed barrier modified leader registers");
    require(syscall(SYS_tgkill, child, worker, SIGSTOP) == 0, "stop running worker");
    stopped(worker);
    verify_stopped_thread_set(child, tids);
    kill(child, SIGKILL);
    while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
}

int main()
{
    // Enforce a deadline outside the tracer, including failed fixture setup.
    const pid_t test = fork();
    if (!test) {
        setpgid(0, 0);
        try { run_case(); _exit(0); }
        catch (const std::exception &error) {
            std::fprintf(stderr, "thread control: %s\n", error.what());
            _exit(1);
        }
    }
    if (test < 0) return 1;
    setpgid(test, test);
    int status = 0;
    bool done = false;
    for (int tick = 0; tick < 500; ++tick) {
        if (waitpid(test, &status, WNOHANG) == test) { done = true; break; }
        usleep(10000);
    }
    kill(-test, SIGKILL);
    if (!done) waitpid(test, &status, 0);
    const bool passed = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s thread stop barrier (real clone, running worker, no mutation)\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
