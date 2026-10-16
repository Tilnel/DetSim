#ifndef DETSIM_PENDING_SYSCALL_H
#define DETSIM_PENDING_SYSCALL_H

#include <cstdint>
#include <optional>
#include <sys/types.h>
#include <sys/time.h>

struct syscall_info;
struct tracee_state;

enum class PendingPhase : uint8_t { Waiting, Retry, Ready };
enum class PendingReason : uint8_t { Futex, Io, Timer };

// Absolute deadline in the model clock domain; no host timer is armed.
struct PendingDeadline {
    int clock_id = 0;
    int64_t sec = 0;
    int32_t nsec = 0;
    int64_t timeout_result = 0;

    template<class Archive> void serialize(Archive& ar) {
        ar(clock_id, sec, nsec, timeout_result);
    }
};

// A skipped host syscall remains logically unfinished at its exit stop.
// Retry is an opportunity to recheck I/O, not a promised successful result.
struct PendingSyscall {
    PendingPhase phase = PendingPhase::Waiting;
    PendingReason reason = PendingReason::Futex;
    int nr = -1;
    uint64_t args[6]{};
    int64_t result = 0; // Meaningful only in Ready; otherwise canonical zero.
    std::optional<PendingDeadline> deadline;

    template<class Archive> void serialize(Archive& ar) {
        ar(phase, reason, nr, args, result);
        bool timed = deadline.has_value();
        ar(timed);
        if constexpr (Archive::is_loading::value) {
            if (timed) deadline.emplace();
            else deadline.reset();
        }
        if (timed) ar(*deadline);
    }
};

// Validate a native timespec and freeze a relative timeout into an absolute
// deadline. Conversion and addition saturate at Linux KTIME_MAX (signed 64-bit ns).
PendingDeadline make_pending_deadline(int node, int clock_id, int64_t sec,
    int32_t nsec, bool relative, int64_t timeout_result);
// Earliest microsecond clock at/after deadline; never moves the source backwards.
timeval pending_timeout_clock(const timeval& source, const PendingDeadline& deadline);
void set_pending_deadline(int node, pid_t logical_tid, const PendingDeadline& deadline);
bool pending_timeout_due(int node, pid_t logical_tid);
// A control operation only: no virtual time advance and no user-code resume.
bool expire_pending_syscall(int node, pid_t logical_tid);

void suspend_syscall(int node, pid_t physical_tid, syscall_info& info, PendingReason reason);
// Grant a result and atomically remove the futex wait registration, if any.
// This does not advance time, write guest registers, or resume user code.
void ready_syscall(int node, pid_t logical_tid, int64_t result);
// Returns true if an unfinished call was processed. Never resumes user code.
// Waiting is rejected; Retry can become Waiting again after an I/O recheck.
bool complete_pending_syscall(int node, pid_t physical_tid, syscall_info& info);
void refresh_futex_pending(int node);
// Recompute completion eligibility without advancing time or expiring timers.
void refresh_pending_syscalls();
void sync_pending_blocked(int node);
void validate_pending_syscalls(const tracee_state& state);

#endif
