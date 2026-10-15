#ifndef DETSIM_THREAD_CONTROL_H
#define DETSIM_THREAD_CONTROL_H

#include <span>
#include <sys/types.h>

// Read-only precondition for capture and restore. The caller must keep every
// verified thread stopped until the protected operation has completed.
void verify_stopped_thread_set(pid_t main_pid, std::span<const pid_t> expected);

#endif
