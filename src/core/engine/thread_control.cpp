#include "thread_control.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
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
    // Detect a concurrent clone/exit between enumeration and register checks.
    if (get_thread_list(main_pid) != threads)
        throw std::runtime_error("Thread stop barrier thread set changed during verification");
}
