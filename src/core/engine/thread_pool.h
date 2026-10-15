#ifndef DETSIM_THREAD_POOL_H
#define DETSIM_THREAD_POOL_H

#include <cstdint>
#include <sys/types.h>

struct tracee_state;

pid_t logical_process_id(int node);
pid_t logical_thread_id(int node, pid_t slot);

// Raw Linux thread profile only: no TID writes, clear_child_tid, robust lists,
// rseq, asynchronous signals, or pthread runtime reconstruction.
void validate_logical_clone(uint64_t flags);
pid_t create_logical_thread(int node, pid_t parent, uint64_t flags,
                            uint64_t stack, uint64_t tls);
bool exit_logical_thread(int node, pid_t physical_tid, int exit_code);
void exit_logical_process(int node, pid_t caller, int exit_code);

// Pool bookkeeping is runtime-only. Initial registration accepts exactly the
// captured owned slots; subsequent barriers include all parked carriers.
void prepare_thread_pool(int node, const tracee_state& state);
void verify_thread_pool(int node);
void require_thread_capture_boundary(pid_t tid);
void normalize_thread_pool_for_restore(int node);
void bind_thread_carriers(int node, tracee_state& state);
void clear_thread_pools();

#endif
