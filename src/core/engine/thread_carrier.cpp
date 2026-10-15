#include "thread_carrier.h"
#include "thread_control.h"
#include "guest.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/audit.h>
#include <linux/sched.h>
#include <stdexcept>
#include <string>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto options = PTRACE_O_TRACECLONE | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error("Fatal thread carrier creation failure: " + message);
}

long control(enum __ptrace_request request, pid_t tid, void* address, void* data)
{
    const long result = ptrace(request, tid, address, data);
    if (result == -1) fail("ptrace: " + std::string(std::strerror(errno)));
    return result;
}

user_regs_struct registers(pid_t tid)
{
    user_regs_struct regs{};
    control(PTRACE_GETREGS, tid, nullptr, &regs);
    return regs;
}

int wait_stopped(pid_t tid, Clock::time_point deadline)
{
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(tid, &status, __WALL | WNOHANG);
        if (result == tid) {
            if (!WIFSTOPPED(status)) fail("thread exited during creation");
            return status;
        }
        if (result < 0 && errno != EINTR) fail("waitpid: " + std::string(std::strerror(errno)));
        if (Clock::now() >= deadline) fail("timed out waiting for a ptrace stop");
        usleep(1000);
    }
}

void syscall_stop(pid_t tid, Clock::time_point deadline)
{
    control(PTRACE_SYSCALL, tid, nullptr, nullptr);
    const int status = wait_stopped(tid, deadline);
    if (WSTOPSIG(status) != (SIGTRAP | 0x80) || (status >> 16) != 0)
        fail("expected a syscall stop");
}
} // namespace

pid_t create_thread_carrier(pid_t anchor)
{
    if (anchor <= 0) fail("invalid anchor thread");
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    const auto original = registers(anchor);
    require_no_pending_signals(anchor);
    require_no_rseq(anchor);
    const auto original_mask = read_thread_signal_mask(anchor);
    if (original.cs != 0x33) fail("anchor is not native x86-64");
    __ptrace_syscall_info phase{};
    control(PTRACE_GET_SYSCALL_INFO, anchor, reinterpret_cast<void*>(sizeof phase), &phase);
    if ((phase.op != PTRACE_SYSCALL_INFO_NONE && phase.op != PTRACE_SYSCALL_INFO_EXIT) ||
        (phase.arch && phase.arch != AUDIT_ARCH_X86_64))
        fail("anchor must be at syscall exit or an ordinary native stop");

    const uintptr_t code = scratch_page + 0x400;
    const uintptr_t stack = scratch_page + 0x700;
    std::array<unsigned char, 3> original_code{};
    if (!guest_read_exact(anchor, reinterpret_cast<void*>(code), original_code.data(), original_code.size()))
        fail("cannot preserve reserved scratch bytes");
    constexpr std::array<unsigned char, 3> stub{0x0f, 0x05, 0xcc}; // syscall; int3
    if (!guest_write_exact(anchor, reinterpret_cast<void*>(code), stub.data(), stub.size()))
        fail("cannot write carrier syscall stub");
    control(PTRACE_SETOPTIONS, anchor, nullptr, reinterpret_cast<void*>(options));

    auto injected = original;
    injected.rip = code;
    injected.orig_rax = static_cast<unsigned long long>(-1);
    injected.rax = SYS_clone;
    injected.rdi = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    injected.rsi = stack;
    injected.rdx = injected.r10 = injected.r8 = injected.r9 = 0;
    // The internal child must reach int3 even when the guest blocks SIGTRAP.
    write_thread_signal_mask(anchor, 0);
    control(PTRACE_SETREGS, anchor, nullptr, &injected);
    syscall_stop(anchor, deadline);
    __ptrace_syscall_info entry{};
    control(PTRACE_GET_SYSCALL_INFO, anchor, reinterpret_cast<void*>(sizeof entry), &entry);
    if (entry.op != PTRACE_SYSCALL_INFO_ENTRY || entry.arch != AUDIT_ARCH_X86_64 || entry.entry.nr != SYS_clone)
        fail("unexpected injected clone entry");
    control(PTRACE_SYSCALL, anchor, nullptr, nullptr);
    const int clone_status = wait_stopped(anchor, deadline);
    if (WSTOPSIG(clone_status) != SIGTRAP || (clone_status >> 16) != PTRACE_EVENT_CLONE)
        fail("expected clone event (kernel clone may have failed)");
    unsigned long message = 0;
    control(PTRACE_GETEVENTMSG, anchor, nullptr, &message);
    const pid_t child = static_cast<pid_t>(message);
    if (child <= 0 || static_cast<unsigned long>(child) != message) fail("invalid clone event thread ID");
    const int child_status = wait_stopped(child, deadline);
    if (WSTOPSIG(child_status) != SIGSTOP || (child_status >> 16) != 0)
        fail("expected newborn thread initial stop");
    control(PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(options));
    syscall_stop(anchor, deadline);
    __ptrace_syscall_info exit{};
    control(PTRACE_GET_SYSCALL_INFO, anchor, reinterpret_cast<void*>(sizeof exit), &exit);
    if (exit.op != PTRACE_SYSCALL_INFO_EXIT || exit.exit.is_error || exit.exit.rval != child)
        fail("unexpected parent clone result");
    const auto newborn = registers(child);
    if (newborn.rip != code + 2 || newborn.rsp != stack || newborn.rax != 0)
        fail("unexpected newborn register context");
    control(PTRACE_CONT, child, nullptr, nullptr);
    const int parked_status = wait_stopped(child, deadline);
    if (WSTOPSIG(parked_status) != SIGTRAP || (parked_status >> 16) != 0)
        fail("expected newborn internal breakpoint");
    const auto parked = registers(child);
    if (parked.rip != code + 3 || parked.rsp != stack || parked.rax != 0)
        fail("newborn escaped internal breakpoint");
    if (!guest_write_exact(anchor, reinterpret_cast<void*>(code), original_code.data(), original_code.size()))
        fail("cannot restore reserved scratch bytes");
    control(PTRACE_SETREGS, anchor, nullptr, const_cast<user_regs_struct*>(&original));
    const auto restored = registers(anchor);
    if (std::memcmp(&original, &restored, sizeof original) != 0)
        fail("anchor register restoration mismatch");
    write_thread_signal_mask(anchor, original_mask);
    write_thread_signal_mask(child, 0);
    return child;
}
