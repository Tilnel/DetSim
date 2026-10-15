#ifndef DETSIM_THREAD_CARRIER_H
#define DETSIM_THREAD_CARRIER_H

#include <sys/types.h>

// Create a raw kernel thread and park it at an internal breakpoint. The caller
// must keep all existing threads stopped and provide the reserved RWX scratch
// page. anchor must be at syscall exit or an ordinary ptrace stop, not entry.
// This does not initialize guest TLS/libc or publish a logical thread. On any
// error the process is unsafe to resume; the caller must abort exploration.
pid_t create_thread_carrier(pid_t anchor);

#endif
