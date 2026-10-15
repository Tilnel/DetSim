#include "pending_syscall.h"
#include "state/state.h"
#include "monitor.h"
#include "guest.h"
#include "pending_io.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <limits>
#include <time.h>
#include <sys/syscall.h>

namespace {
void validate_deadline(const PendingDeadline& deadline)
{
    if ((deadline.clock_id != CLOCK_REALTIME && deadline.clock_id != CLOCK_MONOTONIC) ||
        deadline.sec < 0 || deadline.nsec < 0 || deadline.nsec >= 1000000000)
        throw std::runtime_error("Invalid pending syscall deadline");
}

thread_state* logical_actor(int node, pid_t logical_tid)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    return ptmc_state.running_state.child[node].find_thread(logical_tid);
}

void validate_model_time(int node)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    const auto& now = ptmc_state.time[node];
    if (now.tv_sec < 0 || now.tv_usec < 0 || now.tv_usec >= 1000000)
        throw std::runtime_error("Invalid modeled clock for pending deadline");
}
thread_state& actor(int node, pid_t physical_tid)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    auto& threads = ptmc_state.running_state.child[node].threads;
    const auto found = std::find_if(threads.begin(), threads.end(), [physical_tid](const auto& thread) {
        return thread.alive && thread.physical_tid == physical_tid;
    });
    if (found == threads.end()) throw std::runtime_error("Pending syscall has no live bound actor");
    return *found;
}
}

PendingDeadline make_pending_deadline(int node, int clock_id, int64_t sec,
    int32_t nsec, bool relative, int64_t timeout_result)
{
    PendingDeadline deadline{clock_id, sec, nsec, timeout_result};
    validate_deadline(deadline);
    validate_model_time(node);
    // Linux ktime_set clamps all seconds >= KTIME_SEC_MAX, and
    // ktime_add_safe saturates an overflowing relative deadline.
    constexpr int64_t limit = std::numeric_limits<int64_t>::max();
    constexpr int64_t billion = 1000000000;
    const auto to_ktime = [](int64_t seconds, int64_t nanos) -> int64_t {
        return seconds >= limit / billion ? limit : seconds * billion + nanos;
    };
    __int128 absolute = to_ktime(sec, nsec);
    if (relative) {
        const auto& now = ptmc_state.time[node];
        absolute += to_ktime(now.tv_sec, now.tv_usec * 1000);
    }
    const int64_t bounded = absolute > limit ? limit : static_cast<int64_t>(absolute);
    deadline.sec = bounded / billion;
    deadline.nsec = static_cast<int32_t>(bounded % billion);
    return deadline;
}

void set_pending_deadline(int node, pid_t logical_tid, const PendingDeadline& deadline)
{
    validate_deadline(deadline);
    if (static_cast<__int128>(deadline.sec) * 1000000000 + deadline.nsec > std::numeric_limits<int64_t>::max())
        throw std::runtime_error("Pending deadline exceeds Linux ktime range");
    auto* thread = logical_actor(node, logical_tid);
    if (!thread || !thread->pending_syscall || thread->pending_syscall->phase == PendingPhase::Ready)
        throw std::runtime_error("Deadline requires an unfinished waiting syscall");
    thread->pending_syscall->deadline = deadline;
}

bool pending_timeout_due(int node, pid_t logical_tid)
{
    const auto* thread = logical_actor(node, logical_tid);
    if (!thread || !thread->pending_syscall) return false;
    const auto& pending = *thread->pending_syscall;
    if ((pending.phase != PendingPhase::Waiting && pending.phase != PendingPhase::Retry) || !pending.deadline)
        return false;
    validate_deadline(*pending.deadline);
    validate_model_time(node);
    const auto& now = ptmc_state.time[node];
    return now.tv_sec > pending.deadline->sec ||
        (now.tv_sec == pending.deadline->sec && now.tv_usec * 1000 >= pending.deadline->nsec);
}

bool expire_pending_syscall(int node, pid_t logical_tid)
{
    if (!pending_timeout_due(node, logical_tid)) return false;
    const auto* thread = logical_actor(node, logical_tid);
    ready_syscall(node, logical_tid, thread->pending_syscall->deadline->timeout_result);
    return true;
}

void suspend_syscall(int node, pid_t physical_tid, syscall_info& info, PendingReason reason)
{
    auto& thread = actor(node, physical_tid);
    if (thread.pending_syscall) {
        const auto& previous = *thread.pending_syscall;
        if (previous.phase != PendingPhase::Retry || previous.reason != reason ||
            previous.nr != info.nr || !std::equal(std::begin(previous.args), std::end(previous.args), info.args))
            throw std::runtime_error("Cannot replace an unfinished syscall");
    }
    PendingSyscall pending;
    if (thread.pending_syscall) pending.deadline = thread.pending_syscall->deadline;
    pending.nr = info.nr;
    pending.reason = reason;
    std::copy(std::begin(info.args), std::end(info.args), pending.args);
    // This is an internal sentinel, never a completed guest-visible result.
    tracee_set_rax(physical_tid, static_cast<uint64_t>(-ENOSYS));
    info.rval = static_cast<uint64_t>(-ENOSYS);
    thread.pending_syscall = pending;
    sync_pending_blocked(node);
}

void ready_syscall(int node, pid_t logical_tid, int64_t result)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    auto* thread = ptmc_state.running_state.child[node].find_thread(logical_tid);
    if (!thread || !thread->pending_syscall || (thread->pending_syscall->phase != PendingPhase::Waiting &&
         thread->pending_syscall->phase != PendingPhase::Retry))
        throw std::runtime_error("Only a waiting syscall can receive a completion");
    auto& pending = *thread->pending_syscall;
    auto& state = ptmc_state.running_state.child[node];
    if (pending.reason == PendingReason::Futex && state.futex_state)
        state.futex_state->wake_thread(logical_tid);
    pending.phase = PendingPhase::Ready;
    pending.result = result;
    sync_pending_blocked(node);
}

bool complete_pending_syscall(int node, pid_t physical_tid, syscall_info& info)
{
    // Bootstrap and component fixtures may have no logical slot until capture.
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    const auto& threads = ptmc_state.running_state.child[node].threads;
    if (threads.empty()) return false;
    auto& thread = actor(node, physical_tid);
    if (!thread.pending_syscall) return false;
    const PendingSyscall pending = *thread.pending_syscall;
    if (pending.phase == PendingPhase::Waiting)
        throw std::runtime_error("Cannot execute a waiting syscall before completion is available");
    if (pending.phase != PendingPhase::Ready && pending.phase != PendingPhase::Retry)
        throw std::runtime_error("Invalid pending syscall phase");
    ptmc_state.tracee_image_dirty[node] = true;
    ptmc_state.n_choose = 0;
    ptmc_state.choose = -1;
    info.nr = pending.nr;
    std::copy(std::begin(pending.args), std::end(pending.args), info.args);
    if (pending.phase == PendingPhase::Retry) {
        if (pending.reason != PendingReason::Io)
            throw std::runtime_error("Only I/O pending calls may retry");
        if (!retry_pending_io(node, physical_tid, info)) {
            if (!thread.pending_syscall)
                throw std::runtime_error("I/O retry lost its unfinished call");
            thread.pending_syscall->phase = PendingPhase::Waiting;
            thread.pending_syscall->result = 0;
            info.rval = static_cast<uint64_t>(-ENOSYS);
            tracee_set_rax(physical_tid, info.rval);
            sync_pending_blocked(node);
            return true;
        }
    } else {
        info.rval = static_cast<uint64_t>(pending.result);
        if (pending.nr == SYS_pselect6 && pending.result == 0 && pending.args[4]) {
            // Linux raw pselect6 writes remaining time back best-effort: even a
            // readable but read-only timeout returns success when copyout fails.
            const timespec remaining{};
            (void)guest_write_exact(physical_tid, reinterpret_cast<void*>(pending.args[4]),
                                    &remaining, sizeof(remaining));
        }
    }
    tracee_set_rax(physical_tid, info.rval);
    thread.pending_syscall.reset();
    sync_pending_blocked(node);
    return true;
}

void sync_pending_blocked(int node)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    const auto& threads = ptmc_state.running_state.child[node].threads;
    for (int slot = 0; slot < MAX_THREADS_PER_PROCESS; ++slot)
        ptmc_state.thread_blocked[node][slot] = static_cast<size_t>(slot) < threads.size() &&
            threads[slot].alive && threads[slot].pending_syscall &&
            threads[slot].pending_syscall->phase == PendingPhase::Waiting;
}

void refresh_futex_pending(int node)
{
    if (node < 0 || node >= NP) throw std::runtime_error("Pending syscall node out of range");
    auto& state = ptmc_state.running_state.child[node];
    for (auto& thread : state.threads) {
        if (!thread.alive || !thread.pending_syscall) continue;
        auto& pending = *thread.pending_syscall;
        if (pending.reason == PendingReason::Futex && pending.phase == PendingPhase::Waiting &&
            (!state.futex_state || !state.futex_state->is_thread_waiting(thread.tid))) {
            pending.phase = PendingPhase::Ready;
            pending.result = 0;
        }
    }
    sync_pending_blocked(node);
}

void refresh_pending_syscalls()
{
    for (int node = 0; node < NP; ++node) {
        refresh_futex_pending(node);
        for (auto& thread : ptmc_state.running_state.child[node].threads) {
            if (!thread.alive || !thread.pending_syscall) continue;
            auto& pending = *thread.pending_syscall;
            if (pending.reason != PendingReason::Io || pending.phase == PendingPhase::Ready) continue;
            syscall_info call{};
            call.nr = pending.nr;
            std::copy(std::begin(pending.args), std::end(pending.args), call.args);
            pending.phase = io_retry_ready(node, call) ? PendingPhase::Retry : PendingPhase::Waiting;
        }
        sync_pending_blocked(node);
    }
}

void validate_pending_syscalls(const tracee_state& state)
{
    for (const auto& thread : state.threads) {
        const bool queued = state.futex_state && state.futex_state->is_thread_waiting(thread.tid);
        if (!thread.pending_syscall) {
            if (queued) throw std::runtime_error("Futex waiter has no unfinished syscall");
            continue;
        }
        if (!thread.alive) throw std::runtime_error("Exited thread retains an unfinished syscall");
        const auto& pending = *thread.pending_syscall;
        if (pending.nr < 0 || (pending.reason != PendingReason::Futex && pending.reason != PendingReason::Io && pending.reason != PendingReason::Timer) ||
            (pending.phase != PendingPhase::Waiting && pending.phase != PendingPhase::Retry && pending.phase != PendingPhase::Ready) ||
            (pending.phase != PendingPhase::Ready && pending.result != 0))
            throw std::runtime_error("Invalid unfinished syscall state");
        if (pending.reason == PendingReason::Futex &&
            (pending.nr != SYS_futex || pending.phase == PendingPhase::Retry))
            throw std::runtime_error("Invalid unfinished futex syscall");
        if (pending.reason == PendingReason::Timer &&
            ((pending.nr != SYS_nanosleep && pending.nr != SYS_clock_nanosleep && pending.nr != SYS_pselect6) ||
             pending.phase == PendingPhase::Retry ||
             (!pending.deadline && pending.nr != SYS_pselect6)))
            throw std::runtime_error("Invalid pending timer syscall");
        if (pending.nr == SYS_pselect6 &&
            (pending.reason != PendingReason::Timer || static_cast<int>(pending.args[0]) != 0 ||
             pending.args[5] || pending.deadline.has_value() != bool(pending.args[4]) ||
             (pending.args[4] < scratch_page + 4096 &&
              pending.args[4] > scratch_page - sizeof(timespec)) ||
             (pending.deadline && (pending.deadline->clock_id != CLOCK_MONOTONIC ||
                                   pending.deadline->timeout_result != 0))))
            throw std::runtime_error("Invalid pending pselect6 timer");
        if (pending.reason == PendingReason::Io && !supports_pending_io(pending.nr))
            throw std::runtime_error("Unsupported unfinished I/O syscall");
        if (pending.deadline) {
            validate_deadline(*pending.deadline);
            if (static_cast<__int128>(pending.deadline->sec) * 1000000000 + pending.deadline->nsec >
                    std::numeric_limits<int64_t>::max())
                throw std::runtime_error("Pending deadline exceeds Linux ktime range");
        }
        const bool expected_queue = pending.reason == PendingReason::Futex && pending.phase == PendingPhase::Waiting;
        if (queued != expected_queue) throw std::runtime_error("Futex queue disagrees with unfinished syscall phase");
    }
    if (state.futex_state) {
        for (const auto& [address, waiters] : state.futex_state->wait_queues()) {
            (void)address;
            for (const auto& waiter : waiters)
                if (!state.find_thread(waiter.tid))
                    throw std::runtime_error("Futex queue references a missing live thread");
        }
    }
}
