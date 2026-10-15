#include "engine/thread_carrier.h"
#include "engine/thread_control.h"
#include "guest.h"
#include "ncurses_ui.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static volatile unsigned long canary = 0x12345678;

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static int stopped(pid_t tid)
{
    for (int tick = 0; tick < 1000; ++tick) {
        int status = 0;
        const pid_t result = waitpid(tid, &status, __WALL | WNOHANG);
        if (result == tid) {
            require(WIFSTOPPED(status), "tracee exited before stop");
            return status;
        }
        require(result == 0 || (result < 0 && errno == EINTR), "waitpid tracee");
        usleep(1000);
    }
    throw std::runtime_error("tracee stop deadline");
}

static void run_case(bool syscall_exit)
{
    pid_t child = fork();
    require(child >= 0, "fork tracee");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (__rseq_size && syscall(SYS_rseq,
            static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
            32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(2);
        auto *page = static_cast<unsigned char*>(mmap(reinterpret_cast<void*>(scratch_page), 4096,
            PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (page == MAP_FAILED) _exit(3);
        std::memset(page, 0xa5, 4096);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
        asm volatile("int3");
        syscall(SYS_getpid);
        canary = 0xbad;
        for (;;) asm volatile("" ::: "memory");
    }
    stopped(child);
    require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
            PTRACE_O_TRACECLONE | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
    if (syscall_exit) {
        require(ptrace(PTRACE_SYSCALL, child, nullptr, nullptr) == 0, "advance syscall entry");
        require(WSTOPSIG(stopped(child)) == (SIGTRAP | 0x80), "syscall entry");
        bool rejected = false;
        try { (void)create_thread_carrier(child); }
        catch (const std::runtime_error &error) {
            rejected = std::string(error.what()).find("anchor must be at syscall exit") != std::string::npos;
        }
        require(rejected, "carrier creation accepted syscall-entry anchor");
        require(ptrace(PTRACE_SYSCALL, child, nullptr, nullptr) == 0, "advance syscall exit");
        require(WSTOPSIG(stopped(child)) == (SIGTRAP | 0x80), "syscall exit");
    }
    user_regs_struct original{};
    require(ptrace(PTRACE_GETREGS, child, nullptr, &original) == 0, "save anchor regs");
    std::array<unsigned char, 4096> before{}, after{};
    require(guest_read_exact(child, reinterpret_cast<void*>(scratch_page), before.data(), before.size()), "read scratch before");
    std::vector<pid_t> threads{child};
    for (int i = 0; i < 3; ++i) {
        verify_stopped_thread_set(child, threads);
        const pid_t carrier = create_thread_carrier(child);
        threads.push_back(carrier);
        verify_stopped_thread_set(child, threads);
        user_regs_struct restored{}, parked{};
        require(ptrace(PTRACE_GETREGS, child, nullptr, &restored) == 0, "anchor remains stopped");
        require(std::memcmp(&original, &restored, sizeof original) == 0, "anchor registers changed");
        require(ptrace(PTRACE_GETREGS, carrier, nullptr, &parked) == 0, "carrier remains stopped");
        require(parked.rip == scratch_page + 0x403 && parked.rsp == scratch_page + 0x700,
                "carrier escaped internal stub");
        require(guest_read_exact(child, reinterpret_cast<void*>(scratch_page), after.data(), after.size()), "read scratch after");
        require(before == after, "carrier creation changed scratch/stack bytes");
        unsigned long observed = 0;
        require(guest_read_exact(child, const_cast<unsigned long*>(&canary), &observed, sizeof observed), "read canary");
        require(observed == 0x12345678, "guest code ran during carrier creation");
    }
    kill(child, SIGKILL);
    while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
}

int main()
{
    bool all_passed = true;
    for (int mode = 0; mode < 2; ++mode) {
        const pid_t test = fork();
        if (!test) {
            setpgid(0, 0);
            try { run_case(mode != 0); _exit(0); }
            catch (const std::exception &error) {
                std::fprintf(stderr, "carrier: %s\n", error.what()); _exit(1);
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
        std::printf("%s create three stopped carriers from %s\n", passed ? "PASS" : "FAIL",
                    mode ? "syscall exit (entry rejected)" : "breakpoint");
        all_passed &= passed;
    }
    return all_passed ? 0 : 1;
}
