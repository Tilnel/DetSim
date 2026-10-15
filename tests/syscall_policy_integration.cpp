#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "dispatcher.h"
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static int failures;
static const char replacement_name[] = "escaped";
static rlimit guest_limit{};
static timespec guest_zero{}, guest_delay{1,0}, guest_invalid{-1,0};

static void check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

static int stopped(pid_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int status;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (!WIFSTOPPED(status)) throw std::runtime_error("child did not stop");
            return WSTOPSIG(status);
        }
        if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("child stop deadline");
        usleep(1000);
    }
}

struct Result { bool rejected; long value; };
static Result execute(SyscallDispatcher& dispatcher, pid_t pid, int number,
                      std::array<uint64_t, 6> arguments = {}) {
    user_regs_struct original{}, registers{};
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &original)) throw std::runtime_error("get registers");
    registers = original;
    registers.rip = scratch_page + 0xff0;
    registers.rax = number;
    registers.rdi = arguments[0]; registers.rsi = arguments[1]; registers.rdx = arguments[2];
    registers.r10 = arguments[3]; registers.r8 = arguments[4]; registers.r9 = arguments[5];
    if (ptrace(PTRACE_SETREGS, pid, nullptr, &registers)) throw std::runtime_error("set registers");
    auto advance = [&] {
        if (ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr)) throw std::runtime_error("resume syscall");
        if (stopped(pid) != (SIGTRAP | 0x80)) throw std::runtime_error("unexpected syscall stop");
    };
    advance();
    syscall_info info{};
    info.nr = number;
    std::memcpy(info.args, arguments.data(), sizeof(info.args));
    bool rejected = false;
    try { dispatcher.on_enter(pid, number, info); }
    catch (const std::runtime_error&) {
        rejected = true;
        // Consume a harmless skipped syscall exit before restoring the fixture.
        // No rejected syscall is ever resumed with its original syscall number.
        tracee_set_orig_rax(pid, -1);
    }
    advance();
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &registers)) throw std::runtime_error("get syscall result");
    info.rval = registers.rax;
    if (!rejected) {
        try { dispatcher.on_exit(pid, info); }
        catch (const std::runtime_error&) { rejected=true; }
    }
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &registers) ||
        ptrace(PTRACE_SETREGS, pid, nullptr, &original)) throw std::runtime_error("restore registers");
    return {rejected, static_cast<long>(registers.rax)};
}

static std::string child_name(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (!std::getline(input, name)) throw std::runtime_error("read child comm");
    return name;
}

int main() {
    pid_t child = -1;
    try {
        child = fork();
        if (child < 0) throw std::runtime_error("fork");
        if (!child) {
            if (setpgid(0, 0) || prctl(PR_SET_NAME, "policy-child", 0, 0, 0)) _exit(2);
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) _exit(2);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            asm volatile("int3");
            _exit(0);
        }
        if (stopped(child) != SIGTRAP) throw std::runtime_error("unexpected initial stop");
        if (getpgid(child) != child) throw std::runtime_error("fixture process group");
        if (ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL))
            throw std::runtime_error("ptrace options");
        ptmc_state.pids[0] = child;
        ptmc_state.cursor = 0;
        SyscallDispatcher dispatcher;
        check(SyscallRoute{}.action == SyscallAction::REJECT, "default route rejects unknown host effects");
        bool unknown_rejected = true;
        for (const auto number : {-1, 1000000, SYS_prctl, SYS_kill, SYS_ptrace, SYS_reboot,
                                  SYS_clone,SYS_clone3,SYS_exit})
            unknown_rejected &= dispatcher.get_route(number).action == SyscallAction::REJECT;
        check(unknown_rejected, "unmodeled and out-of-range syscalls reject");
        auto result = execute(dispatcher, child, SYS_prctl,
                              {PR_SET_NAME, reinterpret_cast<uint64_t>(replacement_name), 0, 0, 0, 0});
        check(result.rejected && child_name(child) == "policy-child", "PR_SET_NAME rejected before host mutation");
        for (const auto number : {SYS_getrandom, SYS_clock_gettime, SYS_time, SYS_gettimeofday}) {
            const auto action = dispatcher.get_route(number).action;
            check(action == SyscallAction::EMULATE || action == SyscallAction::REJECT,
                  "clock and random input never passes through");
        }
        result = execute(dispatcher, child, SYS_getpid);
        check(!result.rejected && result.value == child, "owned process getpid remains available");
        const auto tid = execute(dispatcher, child, SYS_gettid);
        check(!tid.rejected && tid.value == child, "gettid preserves the owned main-thread kernel identity");
        result = execute(dispatcher, child, SYS_brk);
        check(!result.rejected && result.value > 0, "brk query remains available");
        const auto mapping = execute(dispatcher, child, SYS_mmap,
                                     {0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                      uint64_t(-1), 0});
        check(!mapping.rejected && mapping.value > 0, "private anonymous mmap remains available");
        if (!mapping.rejected && mapping.value > 0) {
            result = execute(dispatcher, child, SYS_mprotect,
                             {uint64_t(mapping.value), 4096, PROT_READ, 0, 0, 0});
            check(!result.rejected && result.value == 0, "mprotect remains available");
            result = execute(dispatcher, child, SYS_mprotect,
                             {uint64_t(mapping.value + 1), 4096, PROT_READ, 0, 0, 0});
            check(!result.rejected && result.value == -EINVAL, "native invalid memory arguments preserve EINVAL");
            result = execute(dispatcher, child, SYS_munmap, {uint64_t(mapping.value), 4096, 0, 0, 0, 0});
            check(!result.rejected && result.value == 0, "munmap remains available");
        }
        result = execute(dispatcher, child, SYS_prlimit64,
                         {0, RLIMIT_NOFILE, 0, reinterpret_cast<uint64_t>(&guest_limit), 0, 0});
        rlimit limit{};
        check(!result.rejected && result.value == 0 &&
              guest_read_exact(child, &guest_limit, &limit, sizeof(limit)) && limit.rlim_cur > 0,
              "prlimit64 self query remains available");
        result = execute(dispatcher, child, SYS_prlimit64,
                         {0, RLIMIT_NOFILE, reinterpret_cast<uint64_t>(&guest_limit), 0, 0, 0});
        check(result.rejected, "prlimit64 mutation rejected before execution");
        result = execute(dispatcher, child, SYS_prlimit64,
                         {uint64_t(getpid()), RLIMIT_NOFILE, 0, reinterpret_cast<uint64_t>(&guest_limit), 0, 0});
        check(result.rejected, "prlimit64 foreign process query rejected");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,uint64_t(-1),0});
        check(result.rejected,"shared anonymous mmap rejected before kernel");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0});
        check(result.rejected,"unreadable anonymous mmap rejected before kernel");
        result=execute(dispatcher,child,SYS_mprotect,{scratch_page,4096,PROT_NONE,0,0,0});
        check(result.rejected,"unreadable mprotect rejected before kernel");
        result=execute(dispatcher,child,SYS_mprotect,{scratch_page,4096,PROT_READ,0,0,0});
        check(result.rejected,"mprotect cannot change reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_munmap,{scratch_page,4096,0,0,0,0});
        check(result.rejected,"munmap cannot remove reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_mmap,
                       {scratch_page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,uint64_t(-1),0});
        check(result.rejected,"fixed mmap cannot replace reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_mmap,
                       {scratch_page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,uint64_t(-1),0});
        check(result.rejected,"fixed noreplace mmap cannot target reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_munmap,{UINT64_MAX-4095,8192,0,0,0,0});
        check(result.rejected,"mapping address overflow rejected before kernel");
        result=execute(dispatcher,child,SYS_mprotect,{0,UINT64_MAX,PROT_READ,0,0,0});
        check(result.rejected,"mapping length overflow rejected before kernel");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_zero),0,0,0,0,0});
        check(!result.rejected && result.value==0,"valid zero nanosleep completes");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_delay),0,0,0,0,0});
        check(result.rejected,"nonzero nanosleep never manufactures success");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_invalid),0,0,0,0,0});
        check(!result.rejected && result.value==-EINVAL,"invalid nanosleep duration returns EINVAL");
        result=execute(dispatcher,child,SYS_nanosleep,{1,0,0,0,0,0});
        check(!result.rejected && result.value==-EFAULT,"invalid nanosleep pointer returns EFAULT");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::fprintf(stderr, "syscall policy: %s\n", error.what());
        return 1;
    }
}
