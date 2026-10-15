#ifndef DETSIM_THREAD_CONTROL_H
#define DETSIM_THREAD_CONTROL_H

#include <span>
#include <cstdint>
#include <sys/types.h>

// Read-only precondition for capture and restore. The caller must keep every
// verified thread stopped until the protected operation has completed.
void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected);

uint64_t read_thread_signal_mask(pid_t tid);
void write_thread_signal_mask(pid_t tid, uint64_t mask);
void require_no_pending_signals(pid_t tid);
// The modeled kernel never supports rseq; active native registration is an error.
void require_no_rseq(pid_t tid);

#endif
