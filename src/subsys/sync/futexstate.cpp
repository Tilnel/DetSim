/*
 * futexstate.cpp - Futex emulation implementation
 */

#include "futexstate.h"
#include "guest.h"
#include "log_wrapper.h"
#include <cstring>
#include <ctime>
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
            waiter_map_[tid] = 0;
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
    int op = get_op(futex_op);
    bool private_flag = is_private(futex_op);

    (void)private_flag; // May use for validation later

    switch (op) {
    case FUTEX_WAIT: {
        // Check if the value at uaddr matches val
        if (stackval) {
            if (*stackval != (uint64_t)val) {
                LOG_DEBUG("FUTEX_WAIT: value mismatch (expected %d, got %lu)", val, *stackval);
                return -EAGAIN;
            }
        }

        // Add thread to wait queue
        FutexWaiter waiter(tid, uaddr, val);
        if (timeout_addr != 0) {
            waiter.timeout = true;
        }
        add_waiter(waiter);

        LOG_DEBUG("FUTEX_WAIT: TID %d waiting on %p (val=%d)", tid, (void*)uaddr, val);
        return 0;
    }

    case FUTEX_WAKE: {
        int woken = futex_wake(uaddr, val);
        LOG_DEBUG("FUTEX_WAKE: woke %d threads from %p", woken, (void*)uaddr);
        return woken;
    }

    case FUTEX_WAIT_BITSET: {
        uint32_t bitset = (uint32_t)val3;
        if (bitset == 0) {
            return -EINVAL;
        }

        if (stackval) {
            if (*stackval != (uint64_t)val) {
                return -EAGAIN;
            }
        }

        FutexWaiter waiter(tid, uaddr, val, bitset);
        add_waiter(waiter);
        LOG_DEBUG("FUTEX_WAIT_BITSET: TID %d waiting on %p (val=%d, bitset=%u)",
                  tid, (void*)uaddr, val, bitset);
        return 0;
    }

    case FUTEX_WAKE_BITSET: {
        uint32_t bitset = (uint32_t)val3;
        if (bitset == 0) {
            return -EINVAL;
        }
        int woken = futex_wake(uaddr, val, bitset);
        LOG_DEBUG("FUTEX_WAKE_BITSET: woke %d threads from %p (bitset=%u)",
                  woken, (void*)uaddr, bitset);
        return woken;
    }

    case FUTEX_REQUEUE: {
        // val = nr_wake, val3 = nr_requeue
        int nr_wake = val;
        int nr_requeue = val3;
        int woken = futex_wake(uaddr, nr_wake);
        int requeued = 0;

        auto it = waiters_.find(uaddr);
        if (it != waiters_.end()) {
            auto &queue = it->second;
            while (requeued < nr_requeue && !queue.empty()) {
                FutexWaiter waiter = queue.front();
                queue.erase(queue.begin());
                waiter.uaddr = uaddr2;
                add_waiter(waiter);
                requeued++;
            }
        }

        LOG_DEBUG("FUTEX_REQUEUE: woken=%d, requeued=%d from %p to %p",
                  woken, requeued, (void*)uaddr, (void*)uaddr2);
        return woken;
    }

    case FUTEX_CMP_REQUEUE: {
        // val = nr_wake, val3 = nr_requeue, uaddr2's value = cmpval
        // Simplified: just do regular requeue
        return handle_futex(tid, uaddr, FUTEX_REQUEUE | (futex_op & ~FUTEX_CMD_MASK),
                            val, timeout_addr, uaddr2, val3, stackval);
    }

    default:
        LOG_WARN("Futex: unsupported operation %d", op);
        return -ENOSYS;
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
            waiter_map_[waiter.tid] = 0;
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
    int woken = futex_wake(uaddr1, nr_wake);
    int requeued = 0;

    auto it1 = waiters_.find(uaddr1);
    if (it1 == waiters_.end()) {
        return woken;
    }

    auto &queue1 = it1->second;
    std::vector<FutexWaiter> remaining;

    for (auto &waiter : queue1) {
        if (requeued < nr_requeue) {
            // Requeue to uaddr2
            waiter.uaddr = uaddr2;
            waiters_[uaddr2].push_back(waiter);
            waiter_map_[waiter.tid] = uaddr2;
            requeued++;
        } else {
            remaining.push_back(waiter);
        }
    }

    if (remaining.empty()) {
        waiters_.erase(it1);
    } else {
        queue1 = std::move(remaining);
    }

    LOG_DEBUG("FUTEX_REQUEUE: woken=%d, requeued=%d", woken, requeued);
    return woken;
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
                waiter_map_[waiter.tid] = 0;  // Mark as no longer waiting
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
