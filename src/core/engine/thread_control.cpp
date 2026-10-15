#include "thread_control.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <vector>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected)
{
    if (main_pid <= 0 || expected.empty())
        throw std::runtime_error("Thread stop barrier requires a process and a nonempty thread set");
    std::vector<pid_t> threads(expected.begin(), expected.end());
    std::sort(threads.begin(), threads.end());
    if (threads.front() <= 0 ||
        std::adjacent_find(threads.begin(), threads.end()) != threads.end())
        throw std::runtime_error("Thread stop barrier has invalid or duplicate thread IDs");
    if (get_thread_list(main_pid) != threads)
        throw std::runtime_error("Thread stop barrier found a different physical thread set");
    for (pid_t tid : threads) {
        user_regs_struct regs{};
        if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1) {
            const int error = errno;
            throw std::runtime_error("Thread stop barrier cannot inspect stopped thread " +
                                     std::to_string(tid) + ": " + std::strerror(error));
        }
    }
    for (pid_t tid : threads) {
        require_no_pending_signals(tid);
        require_no_rseq(tid);
        require_no_native_robust_list(tid);
    }
    // Detect a concurrent clone/exit between enumeration and register checks.
    if (get_thread_list(main_pid) != threads)
        throw std::runtime_error("Thread stop barrier thread set changed during verification");
}

uint64_t read_thread_signal_mask(pid_t tid)
{
    uint64_t mask = 0;
    if (ptrace(PTRACE_GETSIGMASK, tid, reinterpret_cast<void*>(sizeof(mask)), &mask) == -1)
        throw std::runtime_error("Cannot read stopped thread signal mask");
    return mask;
}

void write_thread_signal_mask(pid_t tid, uint64_t mask)
{
    if (ptrace(PTRACE_SETSIGMASK, tid, reinterpret_cast<void*>(sizeof(mask)), &mask) == -1)
        throw std::runtime_error("Cannot restore stopped thread signal mask");
}

void require_no_pending_signals(pid_t tid)
{
    // PEEKSIGINFO only exposes queued siginfo objects. The kernel can also
    // have a pending bitmap bit without an allocated sigqueue entry.
    std::ifstream status("/proc/" + std::to_string(tid) + "/status");
    if (!status) throw std::runtime_error("Cannot inspect pending thread signals");
    std::string line;
    bool thread_seen = false, shared_seen = false;
    while (std::getline(status, line)) {
        if (!line.starts_with("SigPnd:") && !line.starts_with("ShdPnd:")) continue;
        uint64_t pending = 0;
        std::istringstream input(line.substr(line.find(':') + 1));
        if (!(input >> std::hex >> pending))
            throw std::runtime_error("Cannot parse pending thread signals");
        input >> std::ws;
        if (!input.eof()) throw std::runtime_error("Invalid pending thread signal bitmap");
        if (pending) throw std::runtime_error("Unsupported pending signal at thread stop barrier");
        if (line.starts_with("SigPnd:")) thread_seen = true;
        else shared_seen = true;
    }
    if (status.bad() || !thread_seen || !shared_seen)
        throw std::runtime_error("Cannot read complete pending thread signal state");
}

void require_no_rseq(pid_t tid)
{
  __ptrace_rseq_configuration configuration{};
  const auto size = ptrace(PTRACE_GET_RSEQ_CONFIGURATION, tid,
      reinterpret_cast<void*>(sizeof(configuration)), &configuration);
  if (size != sizeof(configuration))
    throw std::runtime_error("Cannot inspect thread rseq registration: PTRACE_GET_RSEQ_CONFIGURATION is required");
  if (configuration.rseq_abi_pointer || configuration.rseq_abi_size)
    throw std::runtime_error("Unsupported active rseq registration in rseq-unavailable model");
}

uint64_t read_thread_robust_list(pid_t tid)
{
    robust_list_head* head = nullptr;
    size_t length = 0;
    if (syscall(SYS_get_robust_list, tid, &head, &length) != 0 ||
        length != sizeof(robust_list_head))
        throw std::runtime_error("Cannot inspect native thread robust-list registration");
    return reinterpret_cast<uint64_t>(head);
}

void require_no_native_robust_list(pid_t tid)
{
    if (read_thread_robust_list(tid))
        throw std::runtime_error("Thread carrier has unadopted native robust-list registration");
}
