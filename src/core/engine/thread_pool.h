#ifndef DETSIM_THREAD_POOL_H
#define DETSIM_THREAD_POOL_H

#include <cstdint>
#include <sys/types.h>

struct tracee_state;

pid_t logical_process_id(int node);
pid_t logical_thread_id(int node, pid_t slot);

// Supported Linux shared-address-space thread profile. PI robust ownership,
// rseq and asynchronous signal delivery remain unsupported.
struct CloneRequest {
  uint64_t flags = 0, stack = 0, tls = 0, parent_tid = 0, child_tid = 0;
};
pid_t create_logical_thread(int node, pid_t parent, const CloneRequest& request);
// Run after dirtying the image and before the selected child resumes user code.
void complete_thread_start(int node, pid_t physical_tid);
pid_t record_logical_clear_tid(int node, pid_t physical_tid, uint64_t address);
// Bootstrap only, after all nodes are stopped and scratch memory is installed.
// Transfer a physical registration into model state, then disable it in Linux.
void adopt_clear_child_tid(int node, pid_t physical_tid);
void adopt_thread_signal_mask(int node, pid_t physical_tid);
void adopt_thread_robust_list(int node, pid_t physical_tid);
long emulate_set_robust_list(int node, pid_t physical_tid, uint64_t head, uint64_t length);
long emulate_get_robust_list(int node, pid_t caller, int32_t target,
                            uint64_t head_out, uint64_t length_out);
long emulate_thread_sigprocmask(int node, pid_t physical_tid, uint64_t how,
                               uint64_t new_mask, uint64_t old_mask, uint64_t size);
void restore_thread_signal_masks(int node, const tracee_state& state);
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
