#pragma once

#include <sys/types.h>
struct syscall_info;

// Low-level emulators are nonblocking probes. The execution layer uses these
// predicates to retain a logical call instead of exposing EAGAIN/false timeout.
bool supports_pending_io(int nr);
bool io_should_suspend(int node, const syscall_info& call, long result);
void configure_pending_io(int node, pid_t tid, const syscall_info& call);
bool io_retry_ready(int node, const syscall_info& call);
bool retry_pending_io(int node, pid_t tid, syscall_info& call);
void require_no_pending_fd(int node, int fd);

// Zero-descriptor pselect6 timer admission; unsupported multiplexing/masks reject.
void start_pselect_timer(int node, pid_t tid, syscall_info& call);

// Normal process exit closes virtual descriptors only after all logical tasks
// have exited. Unsupported TCP reset cases reject in the preflight phase.
void preflight_process_io_exit(int node);
void close_process_io(int node);
