#include "thread_pool.h"
#include "thread_carrier.h"
#include "thread_control.h"
#include "thread_manager.h"
#include "state/state.h"
#include "state/xstate.h"
#include "monitor.h"
#include "proc_status.h"
#include "guest.h"
#include <linux/futex.h>
#include <linux/prctl.h>
#include <sys/syscall.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <linux/audit.h>
#include <sys/wait.h>
#include <map>
#include <sched.h>
#include <set>
#include <stdexcept>
#include <sys/ptrace.h>

extern std::unordered_map<int, int> rseq_len;

namespace {
struct CarrierPool {
  pid_t anchor;
  std::vector<pid_t> carriers;
};
std::map<int, CarrierPool> pools;

__ptrace_syscall_info syscall_phase(pid_t tid)
{
  __ptrace_syscall_info phase{};
  if (ptrace(PTRACE_GET_SYSCALL_INFO, tid, reinterpret_cast<void*>(sizeof(phase)), &phase) == -1)
    throw std::runtime_error("Cannot inspect owned thread syscall stop phase");
  return phase;
}

CarrierPool& pool_for(int node)
{
  const auto found = pools.find(node);
  if (node < 0 || node >= NP || found == pools.end() ||
      found->second.anchor != ptmc_state.pids[node])
    throw std::runtime_error("Thread carrier pool has no matching owned process");
  return found->second;
}

std::vector<pid_t> live_bindings(const tracee_state& state)
{
  std::vector<pid_t> expected;
  for (const auto& thread : state.threads)
    if (thread.alive) expected.push_back(thread.physical_tid);
  return expected;
}

// Query without changing guest scratch contents or registers.
uint64_t query_clear_child_tid(pid_t tid)
{
  constexpr uintptr_t offset = 0x300;
  auto* remote = reinterpret_cast<void*>(scratch_page + offset);
  uintptr_t saved = 0, address = 0;
  if (!guest_read_exact(tid, remote, &saved, sizeof(saved)))
    throw std::runtime_error("Cannot preserve clear_child_tid query scratch bytes");
  const auto result = static_cast<int64_t>(tracee_do_syscall(tid, SYS_prctl,
      PR_GET_TID_ADDRESS, reinterpret_cast<uintptr_t>(remote), 0, 0, 0, 0));
  const bool copied = result == 0 && guest_read_exact(tid, remote, &address, sizeof(address));
  if (!guest_write_exact(tid, remote, &saved, sizeof(saved)))
    throw std::runtime_error("Cannot restore clear_child_tid query scratch bytes");
  if (result != 0)
    throw std::runtime_error("Cannot query inherited clear_child_tid: PR_GET_TID_ADDRESS requires CONFIG_CHECKPOINT_RESTORE");
  if (!copied) throw std::runtime_error("Cannot read inherited clear_child_tid address");
  return address;
}

void require_no_rseq(pid_t tid)
{
  __ptrace_rseq_configuration configuration{};
  const auto size = ptrace(PTRACE_GET_RSEQ_CONFIGURATION, tid,
      reinterpret_cast<void*>(sizeof(configuration)), &configuration);
  if (size != sizeof(configuration))
    throw std::runtime_error("Cannot inspect thread rseq registration: PTRACE_GET_RSEQ_CONFIGURATION is required");
  if (configuration.rseq_abi_pointer || configuration.rseq_abi_size)
    throw std::runtime_error("Raw clone thread pool does not support active rseq registration");
}

void require_empty_robust_list(pid_t tid)
{
  robust_list_head* remote = nullptr;
  size_t length = 0;
  if (syscall(SYS_get_robust_list, tid, &remote, &length) != 0)
    throw std::runtime_error("Cannot inspect thread robust futex list before logical exit");
  if (!remote) return;
  if (length != sizeof(robust_list_head))
    throw std::runtime_error("Unsupported thread robust futex list ABI");
  robust_list_head head{};
  if (!guest_read_exact(tid, remote, &head, sizeof(head)))
    throw std::runtime_error("Cannot read thread robust futex list before logical exit");
  if (head.list.next != reinterpret_cast<robust_list*>(remote) || head.list_op_pending)
    throw std::runtime_error("Logical exit does not support nonempty or pending robust futex ownership");
}

void validate_tid_pointer(uint64_t address)
{
  if (address < scratch_page + 4096 && address > scratch_page - sizeof(int32_t))
    throw std::runtime_error("TID write overlaps reserved control memory");
}

// Kernel put_user(u32) does not leave a prefix written on an x86 page fault.
// process_vm_writev can, so verify the whole range while every carrier is stopped.
bool write_tid_word(pid_t pid, uint64_t address, int32_t value, bool clearing = false)
{
  if (!address || address > UINT64_MAX - sizeof(value)) return false;
  std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
  if (!maps) throw std::runtime_error("Cannot inspect TID write mappings");
  const uint64_t end = address + sizeof(value);
  uint64_t cursor = address;
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long long start = 0, stop = 0, offset = 0, inode = 0;
    unsigned major = 0, minor = 0;
    char permissions[5]{};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu",
                    &start, &stop, permissions, &offset, &major, &minor, &inode) != 7)
      throw std::runtime_error("Cannot parse TID write mappings");
    if (stop <= cursor) continue;
    if (start > cursor) return false;
    if (permissions[1] != 'w') {
      // A read-only file page can still provide a shared futex key although
      // put_user fails. That COW/backing distinction is outside this profile.
      if (clearing && inode && (address % alignof(int32_t) == 0))
        throw std::runtime_error("Unsupported clear_child_tid on read-only file mapping");
      return false;
    }
    cursor = std::min<uint64_t>(stop, end);
    if (cursor == end) {
      if (!guest_write_exact(pid, reinterpret_cast<void*>(address), &value, sizeof(value)))
        throw std::runtime_error("TID write failed after writable-range validation");
      return true;
    }
  }
  if (maps.bad()) throw std::runtime_error("Cannot read TID write mappings");
  return false;
}

void clear_logical_tid(int node, tracee_state& state, thread_state& thread)
{
  const bool shared_mm = std::any_of(state.threads.begin(), state.threads.end(),
      [&thread](const thread_state& peer) { return &peer != &thread && peer.alive; });
  if (thread.clear_child_tid && shared_mm) {
    const bool written = write_tid_word(thread.physical_tid, thread.clear_child_tid, 0, true);
    if (written && thread.clear_child_tid % alignof(int32_t) == 0 && state.futex_state)
      state.futex_state->futex_wake(thread.clear_child_tid, 1, ~0u, false);
  }
  thread.clear_child_tid = 0;
  thread.child_settid_pending = false;
  thread.pending_child_settid = 0;
  if (state.futex_state) {
    state.futex_state->wake_thread(thread.tid);
    for (size_t i = 0; i < state.threads.size(); ++i)
      ptmc_state.thread_blocked[node][i] = state.futex_state->is_thread_waiting(state.threads[i].tid);
  }
}

void validate_bound_slots(const CarrierPool& pool, const tracee_state& state)
{
  if (state.threads.empty() || state.main_tid != 1 || !state.threads.front().is_main)
    throw std::runtime_error("Thread pool requires logical slot 1 as its unique main thread");
  std::set<pid_t> occupied;
  for (size_t i = 0; i < state.threads.size(); ++i) {
    const auto& thread = state.threads[i];
    if (thread.tid != static_cast<pid_t>(i + 1) || thread.is_main != (i == 0))
      throw std::runtime_error("Invalid logical thread slot or main-thread designation");
    if (!thread.alive) continue;
    if (!std::binary_search(pool.carriers.begin(), pool.carriers.end(), thread.physical_tid) ||
        !occupied.insert(thread.physical_tid).second ||
        (thread.is_main && thread.physical_tid != pool.anchor) ||
        (!thread.is_main && thread.physical_tid == pool.anchor))
      throw std::runtime_error("Logical thread has no unique owned carrier binding");
  }
}
}

pid_t logical_process_id(int node)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Logical process index out of range");
  return node * MAX_THREADS_PER_PROCESS + 1;
}

pid_t logical_thread_id(int node, pid_t slot)
{
  if (slot < 1 || slot > MAX_THREADS_PER_PROCESS)
    throw std::runtime_error("Logical thread slot out of range");
  return logical_process_id(node) + slot - 1;
}

pid_t record_logical_clear_tid(int node, pid_t physical_tid, uint64_t address)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  validate_tid_pointer(address);
  auto& state = ptmc_state.running_state.child[node];
  if (state.threads.empty()) {
    if (physical_tid != ptmc_state.pids[node])
      throw std::runtime_error("Initial logical thread must be the owned leader");
    state.add_thread(physical_tid, 0, 0, 0, true);
  }
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("set_tid_address has no live owned thread");
  found->clear_child_tid = address;
  return logical_thread_id(node, found->tid);
}

void adopt_clear_child_tid(int node, pid_t physical_tid)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  const bool owned = state.threads.empty() ? physical_tid == ptmc_state.pids[node] :
      std::any_of(state.threads.begin(), state.threads.end(),
          [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (!owned) throw std::runtime_error("Cannot adopt unowned thread registration");
  const uint64_t address = query_clear_child_tid(physical_tid);
  // Bootstrap may already have recorded a logical registration and suppressed
  // the kernel call. Preserve it when there is nothing physical to transfer.
  if (address || state.threads.empty()) record_logical_clear_tid(node, physical_tid, address);
  const auto result = static_cast<int64_t>(tracee_do_syscall(physical_tid,
      SYS_set_tid_address, 0, 0, 0, 0, 0, 0));
  if (result != physical_tid)
    throw std::runtime_error("Cannot disable carrier clear_child_tid registration");
}

void complete_thread_start(int node, pid_t physical_tid)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  // Single-thread engine component callers can run before their first snapshot.
  if (found == state.threads.end()) {
    if (state.threads.empty() && physical_tid == ptmc_state.pids[node]) return;
    throw std::runtime_error("Thread start has no live owned carrier");
  }
  if (!found->child_settid_pending) return;
  verify_thread_pool(node);
  const int32_t id = logical_thread_id(node, found->tid);
  write_tid_word(physical_tid, found->pending_child_settid, id);
  found->child_settid_pending = false;
  found->pending_child_settid = 0;
}

void prepare_thread_pool(int node, const tracee_state& state)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto found = pools.find(node);
  if (found == pools.end()) {
    auto carriers = live_bindings(state);
    verify_stopped_thread_set(ptmc_state.pids[node], carriers);
    std::sort(carriers.begin(), carriers.end());
    if (!std::binary_search(carriers.begin(), carriers.end(), ptmc_state.pids[node]))
      throw std::runtime_error("Initial thread pool has no owned leader anchor");
    found = pools.emplace(node, CarrierPool{ptmc_state.pids[node], std::move(carriers)}).first;
  } else verify_thread_pool(node);
  validate_bound_slots(found->second, state);
  for (pid_t tid : found->second.carriers) require_thread_capture_boundary(tid);
}

void verify_thread_pool(int node)
{
  const auto& pool = pool_for(node);
  verify_stopped_thread_set(pool.anchor, pool.carriers);
}

void require_thread_capture_boundary(pid_t tid)
{
  const auto phase = syscall_phase(tid);
  if ((phase.op != PTRACE_SYSCALL_INFO_NONE && phase.op != PTRACE_SYSCALL_INFO_EXIT) ||
      (phase.arch && phase.arch != AUDIT_ARCH_X86_64))
    throw std::runtime_error("Snapshot capture requires a completed native syscall or ordinary stop");
}

void normalize_thread_pool_for_restore(int node)
{
  // The caller has already verified every node, marked images dirty, and kept
  // all carriers stopped. Cancel an entry rejected by the model before using
  // an injector which assumes syscall exit. No guest instruction is resumed.
  const auto& pool = pool_for(node);
  for (pid_t tid : pool.carriers) {
    const auto phase = syscall_phase(tid);
    if (phase.op == PTRACE_SYSCALL_INFO_NONE || phase.op == PTRACE_SYSCALL_INFO_EXIT) continue;
    if (phase.op != PTRACE_SYSCALL_INFO_ENTRY)
      throw std::runtime_error("Unsupported thread stop phase during snapshot rollback");
    user_regs_struct registers{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &registers) == -1)
      throw std::runtime_error("Cannot inspect pending syscall during snapshot rollback");
    registers.orig_rax = static_cast<unsigned long long>(-1);
    if (ptrace(PTRACE_SETREGS, tid, nullptr, &registers) == -1 ||
        ptrace(PTRACE_SYSCALL, tid, nullptr, nullptr) == -1)
      throw std::runtime_error("Cannot suppress pending syscall during snapshot rollback");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    for (;;) {
      const auto waited = waitpid(tid, &status, __WALL | WNOHANG);
      if (waited == tid) break;
      if (waited < 0 && errno != EINTR)
        throw std::runtime_error("Cannot wait for cancelled syscall during snapshot rollback");
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Timed out cancelling pending syscall during snapshot rollback");
      usleep(1000);
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80) ||
        (status >> 16) || syscall_phase(tid).op != PTRACE_SYSCALL_INFO_EXIT)
      throw std::runtime_error("Unexpected stop after cancelling pending syscall during snapshot rollback");
  }
}

void bind_thread_carriers(int node, tracee_state& state)
{
  const auto& pool = pool_for(node);
  size_t next = 0;
  for (auto& thread : state.threads) {
    thread.physical_tid = 0;
    if (!thread.alive) continue;
    if (thread.is_main) { thread.physical_tid = pool.anchor; continue; }
    while (next < pool.carriers.size() && pool.carriers[next] == pool.anchor) ++next;
    if (next == pool.carriers.size())
      throw std::runtime_error("Snapshot needs more thread carriers than retained by its owner");
    thread.physical_tid = pool.carriers[next++];
  }
  validate_bound_slots(pool, state);
}

void clear_thread_pools() { pools.clear(); }

void validate_logical_clone(uint64_t flags)
{
  constexpr uint64_t required = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
  constexpr uint64_t allowed = required | CLONE_SYSVSEM | CLONE_SETTLS |
      CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
  if ((flags & required) != required || (flags & ~allowed))
    throw std::runtime_error("Unsupported raw clone thread flags");
}

pid_t create_logical_thread(int node, pid_t parent, uint64_t flags,
                            uint64_t stack, uint64_t tls)
{
  return create_logical_thread(node, parent, CloneRequest{flags, stack, tls, 0, 0});
}

pid_t create_logical_thread(int node, pid_t parent, const CloneRequest& request)
{
  const auto [flags, stack, tls, parent_tid, child_tid] = request;
  validate_logical_clone(flags);
  if (flags & CLONE_PARENT_SETTID) validate_tid_pointer(parent_tid);
  if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) validate_tid_pointer(child_tid);
  if (!stack) throw std::runtime_error("Raw clone requires a child stack");
  if ((stack >= scratch_page && stack <= scratch_page + 4096) ||
      ((flags & CLONE_SETTLS) && tls >= scratch_page && tls < scratch_page + 4096))
    throw std::runtime_error("Raw clone stack or TLS overlaps reserved control memory");
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  prepare_thread_pool(node, state);
  auto& pool = pool_for(node);
  if (rseq_len[pool.anchor] != 0)
    throw std::runtime_error("Raw clone thread pool does not support active rseq registration");
  for (pid_t tid : pool.carriers) require_no_rseq(tid);
  for (pid_t tid : pool.carriers)
    if (query_clear_child_tid(tid))
      throw std::runtime_error("Thread carrier has unadopted inherited clear_child_tid registration");
  if (state.threads.size() >= MAX_THREADS_PER_PROCESS)
    throw std::runtime_error("Logical thread birth limit exceeded");
  const auto source = std::find_if(state.threads.begin(), state.threads.end(),
      [parent](const thread_state& thread) { return thread.alive && thread.physical_tid == parent; });
  if (source == state.threads.end()) throw std::runtime_error("Raw clone has no live owned parent");
  user_regs_struct regs{};
  if (ptrace(PTRACE_GETREGS, parent, nullptr, &regs) == -1)
    throw std::runtime_error("Cannot capture raw clone parent registers");
  const auto xstate = capture_xstate(parent);
  const auto parent_slot = static_cast<size_t>(source - state.threads.begin());
  std::set<pid_t> occupied;
  for (const auto& thread : state.threads)
    if (thread.alive) occupied.insert(thread.physical_tid);
  pid_t carrier = 0;
  for (pid_t candidate : pool.carriers)
    if (candidate != pool.anchor && !occupied.count(candidate)) { carrier = candidate; break; }
  if (!carrier) {
    if (pool.carriers.size() >= MAX_THREADS_PER_PROCESS)
      throw std::runtime_error("Physical thread carrier limit exceeded");
    carrier = create_thread_carrier(pool.anchor);
    pool.carriers.push_back(carrier);
    std::sort(pool.carriers.begin(), pool.carriers.end());
  }
  regs.rax = 0;
  regs.rsp = stack;
  if (flags & CLONE_SETTLS) regs.fs_base = tls;
  if (ptrace(PTRACE_SETREGS, carrier, nullptr, &regs) == -1)
    throw std::runtime_error("Cannot initialize logical child registers");
  restore_xstate(carrier, xstate);
  state.add_thread(carrier, flags, stack, parent, false);
  auto& child = state.threads.back();
  child.regs = regs;
  child.xstate = xstate;
  child.clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? child_tid : 0;
  child.child_settid_pending = flags & CLONE_CHILD_SETTID;
  child.pending_child_settid = child.child_settid_pending ? child_tid : 0;
  if (flags & CLONE_PARENT_SETTID) {
    const int32_t id = logical_thread_id(node, child.tid);
    // Linux ignores put_user failure for clone TID writeback.
    write_tid_word(parent, parent_tid, id);
  }
  const auto child_slot = state.threads.size() - 1;
  state.si[child_slot] = state.si[parent_slot];
  state.si[child_slot].rval = 0;
  ptmc_state.thread_blocked[node][child_slot] = false;
  ptmc_state.thread_exited[node][child_slot] = false;
  return child.tid;
}

bool exit_logical_thread(int node, pid_t physical_tid, int exit_code)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  prepare_thread_pool(node, state);
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("Logical exit has no live owned thread");
  if (query_clear_child_tid(physical_tid))
    throw std::runtime_error("Thread carrier has unadopted inherited clear_child_tid registration");
  require_empty_robust_list(physical_tid);
  if (ptrace(PTRACE_GETREGS, physical_tid, nullptr, &found->regs) == -1)
    throw std::runtime_error("Cannot capture exiting thread registers");
  found->regs.rax = 0;
  found->xstate = capture_xstate(physical_tid);
  found->exit_code = exit_code & 255;
  const auto slot = static_cast<size_t>(found - state.threads.begin());
  clear_logical_tid(node, state, *found);
  found->alive = false;
  found->physical_tid = 0;
  ptmc_state.thread_exited[node][slot] = true;
  ptmc_state.thread_blocked[node][slot] = false;
  const bool last = std::none_of(state.threads.begin(), state.threads.end(),
      [](const thread_state& thread) { return thread.alive; });
  if (last) ptmc_state.status[node] = dstatus_exit(exit_code);
  return last;
}

void exit_logical_process(int node, pid_t caller, int exit_code)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  prepare_thread_pool(node, state);
  if (std::none_of(state.threads.begin(), state.threads.end(),
      [caller](const thread_state& thread) { return thread.alive && thread.physical_tid == caller; }))
    throw std::runtime_error("Logical process exit has no live owned caller");
  // Check every owner before changing any peer's lifecycle or futex queues.
  for (const auto& thread : state.threads) {
    if (!thread.alive) continue;
    require_empty_robust_list(thread.physical_tid);
    if (query_clear_child_tid(thread.physical_tid))
      throw std::runtime_error("Thread carrier has unadopted inherited clear_child_tid registration");
  }
  for (size_t i = 0; i < state.threads.size(); ++i) {
    auto& thread = state.threads[i];
    if (!thread.alive) continue;
    // Linux performs the newborn's CHILD_SETTID before processing group death.
    // Keep this paired with each slot's exit so aliased TID stores retain order.
    complete_thread_start(node, thread.physical_tid);
    if (ptrace(PTRACE_GETREGS, thread.physical_tid, nullptr, &thread.regs) == -1)
      throw std::runtime_error("Cannot capture terminating thread registers");
    thread.xstate = capture_xstate(thread.physical_tid);
    if (thread.physical_tid == caller) thread.regs.rax = 0;
    thread.exit_code = exit_code & 255;
    clear_logical_tid(node, state, thread);
    thread.alive = false;
    thread.physical_tid = 0;
    ptmc_state.thread_blocked[node][i] = false;
    ptmc_state.thread_exited[node][i] = true;
  }
  ptmc_state.status[node] = dstatus_exit(exit_code);
}
