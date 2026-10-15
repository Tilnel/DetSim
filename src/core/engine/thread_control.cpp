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
    for (pid_t tid : threads) require_no_pending_signals(tid);
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
