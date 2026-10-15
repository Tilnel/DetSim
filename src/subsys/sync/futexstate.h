/*
 * futexstate.h - Futex emulation for multi-threading support
 *
 * This module provides user-space emulation of Linux futex operations,
 * allowing DetSim to control thread synchronization deterministically.
 */

#ifndef __FUTEXSTATE_H
#define __FUTEXSTATE_H

#include <cstdint>
#include <map>
#include <set>
#include <vector>
#include <sys/types.h>

// Futex operation codes (from Linux kernel)
#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10

// Futex flags
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

// Futex waiter state
struct FutexWaiter
{
    pid_t tid;                      // Waiting thread TID
    uint64_t uaddr;                 // Futex address
    int val;                        // Expected value
    uint32_t bitset;                // For bitset operations
    bool private_wait = false;      // Distinct Linux private/shared futex key

    FutexWaiter() : tid(0), uaddr(0), val(0), bitset(~0) {}
    FutexWaiter(pid_t t, uint64_t addr, int v, uint32_t bs = ~0, bool priv = false)
        : tid(t), uaddr(addr), val(v), bitset(bs), private_wait(priv) {}

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(tid, uaddr, val, bitset, private_wait);
    }
};

/*
 * FutexState - Manages futex operations for deterministic thread synchronization
 *
 * This class emulates Linux futex syscalls, maintaining wait queues and
 * handling wake operations. It allows DetSim to control which threads run
 * and when they are unblocked.
 */
class FutexState
{
public:
    FutexState();
    ~FutexState();

    // Handle futex syscall
    // Returns: >= 0 for success (wake count), < 0 for error (-errno)
    int handle_futex(pid_t tid, uint64_t uaddr, int futex_op, int val,
                     uint64_t timeout_addr, uint64_t uaddr2, int val3,
                     uint64_t *stackval = nullptr);

    // Check if a thread is waiting on any futex
    bool is_thread_waiting(pid_t tid) const;

    // Get the futex address a thread is waiting on (0 if not waiting)
    uint64_t get_wait_address(pid_t tid) const;

    // Wake a specific thread (used for timeout or forced wake)
    bool wake_thread(pid_t tid);

    // Get list of threads that can be woken from an address
    std::vector<pid_t> get_waiters(uint64_t uaddr) const;

    // Wake threads waiting on an address
    // Returns: number of threads woken
    int futex_wake(uint64_t uaddr, int nr_wake, uint32_t bitset = ~0, bool private_wait = false);

    // Requeue waiters from one address to another; return woken + moved.
    int futex_requeue(uint64_t uaddr1, uint64_t uaddr2, int nr_wake, int nr_requeue, bool private_wait = false);

    // Wake all waiters whose address falls within [start, end) range
    // This is used when madvise(MADV_DONTNEED) discards memory pages
    int wake_waiters_in_range(uint64_t start, uint64_t end);

    // Clear all state (used on recovery)
    void clear();

    // Serialization support
    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(waiters_, waiter_map_);
    }

    const std::map<uint64_t, std::vector<FutexWaiter>>& wait_queues() const { return waiters_; }

    // Debug output
    void dump_state() const;

private:
    // Wait queue: address -> list of waiters (in order of arrival)
    std::map<uint64_t, std::vector<FutexWaiter>> waiters_;

    // Fast lookup: tid -> address it's waiting on (0 if not waiting)
    std::map<pid_t, uint64_t> waiter_map_;

    // Helper methods
    void add_waiter(const FutexWaiter &waiter);
    bool remove_waiter(pid_t tid);
    int get_op(int futex_op) const { return futex_op & FUTEX_CMD_MASK; }
    bool is_private(int futex_op) const { return futex_op & FUTEX_PRIVATE_FLAG; }
};

#endif /* __FUTEXSTATE_H */
