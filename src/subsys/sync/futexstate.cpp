/*
 * futexstate.cpp - Futex emulation implementation
 */

#include "futexstate.h"
#include "guest.h"
#include "log_wrapper.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sys/errno.h>

FutexState::FutexState()
{
}

FutexState::~FutexState()
{
}

void FutexState::clear()
{
    waiters_.clear();
    waiter_map_.clear();
}

bool FutexState::is_thread_waiting(pid_t tid) const
{
    return waiter_map_.find(tid) != waiter_map_.end() && waiter_map_.at(tid) != 0;
}

uint64_t FutexState::get_wait_address(pid_t tid) const
{
    auto it = waiter_map_.find(tid);
    if (it != waiter_map_.end()) {
        return it->second;
    }
    return 0;
}

void FutexState::add_waiter(const FutexWaiter &waiter)
{
    waiters_[waiter.uaddr].push_back(waiter);
    waiter_map_[waiter.tid] = waiter.uaddr;
    LOG_DEBUG("Futex: TID %d added to wait queue for addr %p", waiter.tid, (void*)waiter.uaddr);
}

bool FutexState::remove_waiter(pid_t tid)
{
    auto it = waiter_map_.find(tid);
    if (it == waiter_map_.end() || it->second == 0) {
        return false;
    }

    uint64_t uaddr = it->second;
    auto &queue = waiters_[uaddr];

    for (auto qit = queue.begin(); qit != queue.end(); ++qit) {
        if (qit->tid == tid) {
            queue.erase(qit);
            waiter_map_.erase(tid);
            if (queue.empty()) waiters_.erase(uaddr);
            LOG_DEBUG("Futex: TID %d removed from wait queue for addr %p", tid, (void*)uaddr);
            return true;
        }
    }
    return false;
}

int FutexState::handle_futex(pid_t tid, uint64_t uaddr, int futex_op, int val,
                              uint64_t timeout_addr, uint64_t uaddr2, int val3,
                              uint64_t *stackval)
{
    const int op = get_op(futex_op);
    if (op != FUTEX_WAIT && op != FUTEX_WAIT_BITSET && op != FUTEX_WAKE &&
        op != FUTEX_WAKE_BITSET && op != FUTEX_REQUEUE && op != FUTEX_CMP_REQUEUE)
        throw std::runtime_error("unsupported futex operation");
    if ((futex_op & FUTEX_CLOCK_REALTIME) && op != FUTEX_WAIT_BITSET)
        return -ENOSYS;
    if (uaddr % sizeof(uint32_t)) return -EINVAL;
    if ((op == FUTEX_WAIT_BITSET || op == FUTEX_WAKE_BITSET) && val3 == 0)
        return -EINVAL;

    switch (op) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
        if (timeout_addr != 0)
            throw std::runtime_error("timed futex wait is not modeled");
        if (!stackval) return -EFAULT;
        if (static_cast<uint32_t>(*stackval) != static_cast<uint32_t>(val))
            return -EAGAIN;
        if (is_thread_waiting(tid))
            throw std::runtime_error("thread already has a pending futex wait");
        add_waiter(FutexWaiter(tid, uaddr, val,
                              op == FUTEX_WAIT_BITSET ? static_cast<uint32_t>(val3) : ~0u));
        return 0;
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        // Linux standalone WAKE wakes one matching waiter even when val <= 0.
        return futex_wake(uaddr, std::max(1, val),
                          op == FUTEX_WAKE_BITSET ? static_cast<uint32_t>(val3) : ~0u);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        if (uaddr2 % sizeof(uint32_t) || val < 0 || static_cast<int32_t>(timeout_addr) < 0)
            return -EINVAL;
        if (op == FUTEX_CMP_REQUEUE) {
            if (!stackval) return -EFAULT;
            if (static_cast<uint32_t>(*stackval) != static_cast<uint32_t>(val3))
                return -EAGAIN;
        }
        // The fourth syscall argument is val2, not a timeout or val3.
        return futex_requeue(uaddr, uaddr2, val, static_cast<int32_t>(timeout_addr));
    default:
        throw std::runtime_error("unreachable futex operation");
    }
}

int FutexState::futex_wake(uint64_t uaddr, int nr_wake, uint32_t bitset)
{
    auto it = waiters_.find(uaddr);
    if (it == waiters_.end() || it->second.empty()) {
        return 0;
    }

    auto &queue = it->second;
    int woken = 0;
    std::vector<FutexWaiter> remaining;

    for (auto &waiter : queue) {
        if (woken < nr_wake && (waiter.bitset & bitset)) {
            // Wake this thread
            waiter_map_.erase(waiter.tid);
            woken++;
            LOG_DEBUG("Futex: waking TID %d from %p", waiter.tid, (void*)uaddr);
        } else {
            remaining.push_back(waiter);
        }
    }

    if (remaining.empty()) {
        waiters_.erase(it);
    } else {
        queue = std::move(remaining);
    }

    return woken;
}

bool FutexState::wake_thread(pid_t tid)
{
    return remove_waiter(tid);
}

std::vector<pid_t> FutexState::get_waiters(uint64_t uaddr) const
{
    std::vector<pid_t> result;
    auto it = waiters_.find(uaddr);
    if (it != waiters_.end()) {
        for (const auto &w : it->second) {
            result.push_back(w.tid);
        }
    }
    return result;
}

int FutexState::futex_requeue(uint64_t uaddr1, uint64_t uaddr2, int nr_wake, int nr_requeue)
{
    if (uaddr1 % sizeof(uint32_t) || uaddr2 % sizeof(uint32_t) || nr_wake < 0 || nr_requeue < 0)
        return -EINVAL;
    const int woken = futex_wake(uaddr1, nr_wake);
    auto source = waiters_.find(uaddr1);
    if (source == waiters_.end()) return woken;
    const int moved = std::min<size_t>(nr_requeue, source->second.size());
    // Linux permits non-PI same-key requeue: count the selected waiters,
    // retaining their queue and arrival order without appending into itself.
    if (uaddr1 == uaddr2) return woken + moved;
    auto &queue = source->second;
    auto &target = waiters_[uaddr2];
    for (int i = 0; i < moved; ++i) {
        auto waiter = queue[i];
        waiter.uaddr = uaddr2;
        target.push_back(waiter);
        waiter_map_[waiter.tid] = uaddr2;
    }
    queue.erase(queue.begin(), queue.begin() + moved);
    if (queue.empty()) waiters_.erase(source);
    if (target.empty()) waiters_.erase(uaddr2);
    return woken + moved;
}

int FutexState::wake_waiters_in_range(uint64_t start, uint64_t end)
{
    int woken = 0;

    // Iterate through all wait queues
    for (auto it = waiters_.begin(); it != waiters_.end(); ) {
        uint64_t addr = it->first;

        // Check if this address falls within the range being discarded
        if (addr >= start && addr < end) {
            // Wake all waiters at this address
            auto &queue = it->second;
            for (const auto &waiter : queue) {
                waiter_map_.erase(waiter.tid);  // Mark as no longer waiting
                woken++;
                LOG_DEBUG("Futex: woke TID %d from addr %p (page discarded)",
                          waiter.tid, (void*)addr);
            }
            // Remove this wait queue entirely
            it = waiters_.erase(it);
        } else {
            ++it;
        }
    }

    if (woken > 0) {
        LOG_INFO("Futex: woke %d waiters due to memory range [%p, %p) being discarded",
                 woken, (void*)start, (void*)end);
    }

    return woken;
}

void FutexState::dump_state() const
{
    LOG_INFO("FutexState dump:");
    LOG_INFO("  Wait queues: %zu", waiters_.size());
    for (const auto &pair : waiters_) {
        LOG_INFO("  Addr %p: %zu waiters", (void*)pair.first, pair.second.size());
        for (const auto &w : pair.second) {
            LOG_INFO("    TID %d (val=%d, bitset=%u)", w.tid, w.val, w.bitset);
        }
    }
}
