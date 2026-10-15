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
#include <csignal>
#include <linux/audit.h>
#include <sys/wait.h>
#include <map>
#include <sched.h>
#include <set>
#include <stdexcept>
#include <sys/ptrace.h>

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

void validate_tid_pointer(uint64_t address)
{
  if (address < scratch_page + 4096 && address > scratch_page - sizeof(int32_t))
    throw std::runtime_error("TID write overlaps reserved control memory");
}

// Kernel put_user(u32) does not leave a prefix written on an x86 page fault.
// process_vm_writev can, so verify the whole range while every carrier is stopped.
bool writable_guest_range(pid_t pid, uint64_t address, size_t size, bool clearing = false)
{
  if (!address || address > UINT64_MAX - size) return false;
  std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
  if (!maps) throw std::runtime_error("Cannot inspect TID write mappings");
  const uint64_t end = address + size;
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
    if (cursor == end) return true;
  }
  if (maps.bad()) throw std::runtime_error("Cannot read TID write mappings");
  return false;
}

template <class T>
bool write_guest_scalar(pid_t pid, uint64_t address, T value, bool clearing = false)
{
  if (!writable_guest_range(pid, address, sizeof(value), clearing)) return false;
  if (!guest_write_exact(pid, reinterpret_cast<void*>(address), &value, sizeof(value)))
    throw std::runtime_error("Scalar write failed after writable-range validation");
  return true;
}

bool write_tid_word(pid_t pid, uint64_t address, int32_t value, bool clearing = false)
{
  return write_guest_scalar(pid, address, value, clearing);
}

void validate_robust_range(uint64_t address, size_t size)
{
  if (address < scratch_page + 4096 &&
      (address >= scratch_page || size > scratch_page - address))
    throw std::runtime_error("Robust-list access overlaps reserved control memory");
}

struct RobustEffect {
  uint64_t address;
  uint32_t value;
  bool write;
  bool wake;
};

// All carriers are stopped. Simulate stores before applying any effects, so
// malformed aliases and cycles see the same ordered values as Linux, while a
// reachable unsupported PI entry rejects before mutating the branch.
class RobustExitPlan {
  pid_t pid;
  std::map<uint64_t, uint8_t> bytes;

  template <class T> bool read(uint64_t address, T& value)
  {
    validate_robust_range(address, sizeof(value));
    if (!address || address > UINT64_MAX - sizeof(value) ||
        !guest_read_exact(pid, reinterpret_cast<void*>(address), &value, sizeof(value)))
      return false;
    auto* result = reinterpret_cast<uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) {
      const auto found = bytes.find(address + i);
      if (found != bytes.end()) result[i] = found->second;
    }
    return true;
  }

  bool death(uint64_t address, bool pi, bool pending, pid_t owner,
             std::vector<RobustEffect>& effects)
  {
    if (pi) throw std::runtime_error("Unsupported PI robust futex ownership");
    validate_robust_range(address, sizeof(uint32_t));
    if (address % sizeof(uint32_t)) return false;
    uint32_t value = 0;
    if (!read(address, value)) return false;
    if (pending && !(value & FUTEX_TID_MASK)) {
      // Shared futex keys for read-only anonymous pages cannot be acquired.
      // Read-only file-backed keys need a backing-object model and reject.
      if (writable_guest_range(pid, address, sizeof(value), true))
        effects.push_back({address, value, false, true});
      return true;
    }
    if ((value & FUTEX_TID_MASK) != static_cast<uint32_t>(owner)) return true;
    const uint32_t replacement = (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
    if (!store(address, replacement)) return false;
    effects.push_back({address, replacement, true, bool(value & FUTEX_WAITERS)});
    return true;
  }

public:
  explicit RobustExitPlan(pid_t physical_tid) : pid(physical_tid) {}

  bool store(uint64_t address, uint32_t value, bool clearing = false)
  {
    validate_robust_range(address, sizeof(value));
    if (!writable_guest_range(pid, address, sizeof(value), clearing)) return false;
    const auto* source = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) bytes[address + i] = source[i];
    return true;
  }

  std::vector<RobustEffect> collect(const thread_state& thread, pid_t owner)
  {
    std::vector<RobustEffect> effects;
    const uint64_t head = thread.robust_list_head;
    if (!head) return effects;
    robust_list_head registration{};
    if (!read(head, registration)) return effects;
    uint64_t tagged = reinterpret_cast<uint64_t>(registration.list.next);
    const uint64_t pending_tagged = reinterpret_cast<uint64_t>(registration.list_op_pending);
    const uint64_t pending = pending_tagged & ~uint64_t(1);
    const uint64_t offset = static_cast<uint64_t>(registration.futex_offset);
    for (unsigned remaining = ROBUST_LIST_LIMIT; (tagged & ~uint64_t(1)) != head; ) {
      const uint64_t entry = tagged & ~uint64_t(1);
      uint64_t next = 0;
      const bool next_read = read(entry, next);
      if (entry != pending && !death(entry + offset, tagged & 1, false, owner, effects))
        return effects;
      // Linux handles the current owner even when fetching next faulted.
      if (!next_read) return effects;
      tagged = next;
      if (!--remaining) break;
    }
    if (pending) death(pending + offset, pending_tagged & 1, true, owner, effects);
    return effects;
  }
};

void apply_robust_cleanup(tracee_state& state, const thread_state& thread,
                          const std::vector<RobustEffect>& effects)
{
  for (const auto& effect : effects) {
    if (effect.write && !write_guest_scalar(thread.physical_tid, effect.address, effect.value))
      throw std::runtime_error("Robust futex became unwritable after exit preflight");
    if (effect.wake && state.futex_state)
      state.futex_state->futex_wake(effect.address, 1, ~0u, false);
  }
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
  write_thread_signal_mask(thread.physical_tid, 0);
  thread.signal_mask = 0;
  thread.robust_list_head = 0;
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
    constexpr uint64_t unblockable = (uint64_t(1) << (SIGKILL - 1)) | (uint64_t(1) << (SIGSTOP - 1));
    if ((thread.signal_mask & unblockable) || (!thread.alive && thread.signal_mask))
      throw std::runtime_error("Invalid logical thread signal mask");
    if (!thread.alive && thread.robust_list_head)
      throw std::runtime_error("Exited logical thread retains robust-list registration");
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
    const uint64_t mask = read_thread_signal_mask(physical_tid);
    state.add_thread(physical_tid, 0, 0, 0, true);
    state.threads.back().signal_mask = mask;
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

void adopt_thread_robust_list(int node, pid_t physical_tid)
{
  if (node < 0 || node >= NP || pools.count(node))
    throw std::runtime_error("Robust-list adoption requires initial owned thread bootstrap");
  auto& state = ptmc_state.running_state.child[node];
  if (state.threads.empty()) record_logical_clear_tid(node, physical_tid, 0);
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("Cannot adopt unowned robust-list registration");
  const uint64_t head = read_thread_robust_list(physical_tid);
  if (!head) return; // Preserve a registration already virtualized by startup.
  validate_robust_range(head, sizeof(robust_list_head));
  robust_list_head registration{};
  if (!guest_read_exact(physical_tid, reinterpret_cast<void*>(head), &registration, sizeof(registration)) ||
      reinterpret_cast<uint64_t>(registration.list.next) != head || registration.list_op_pending)
    throw std::runtime_error("Cannot adopt nonempty or unreadable native robust list with physical owner IDs");
  const auto result = static_cast<int64_t>(tracee_do_syscall(physical_tid,
      SYS_set_robust_list, 0, sizeof(robust_list_head), 0, 0, 0, 0));
  if (result != 0) throw std::runtime_error("Cannot disable carrier robust-list registration");
  found->robust_list_head = head;
}

long emulate_set_robust_list(int node, pid_t physical_tid, uint64_t head, uint64_t length)
{
  if (length != sizeof(robust_list_head)) return -EINVAL;
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("set_robust_list has no live owned thread");
  validate_robust_range(head, sizeof(robust_list_head));
  require_no_native_robust_list(physical_tid);
  found->robust_list_head = head;
  return 0;
}

long emulate_get_robust_list(int node, pid_t caller, int32_t target,
                            uint64_t head_out, uint64_t length_out)
{
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  const thread_state* found = nullptr;
  for (int index = 0; index < NP && !found; ++index)
    for (const auto& thread : ptmc_state.running_state.child[index].threads)
      if (thread.alive && ((target == 0 && index == node && thread.physical_tid == caller) ||
          (target > 0 && logical_thread_id(index, thread.tid) == target))) {
        found = &thread;
        break;
      }
  if (!found) return -ESRCH;
  validate_robust_range(length_out, sizeof(uint64_t));
  validate_robust_range(head_out, sizeof(uint64_t));
  if (!write_guest_scalar(caller, length_out, uint64_t(sizeof(robust_list_head)))) return -EFAULT;
  if (!write_guest_scalar(caller, head_out, found->robust_list_head)) return -EFAULT;
  return 0;
}

void adopt_thread_signal_mask(int node, pid_t physical_tid)
{
  if (node < 0 || node >= NP || pools.count(node))
    throw std::runtime_error("Signal mask adoption requires initial owned thread bootstrap");
  auto& state = ptmc_state.running_state.child[node];
  if (state.threads.empty()) {
    if (physical_tid != ptmc_state.pids[node])
      throw std::runtime_error("Cannot adopt unowned thread signal mask");
    require_no_pending_signals(physical_tid);
    const uint64_t mask = read_thread_signal_mask(physical_tid);
    record_logical_clear_tid(node, physical_tid, 0);
    state.threads.front().signal_mask = mask;
    return;
  }
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("Cannot adopt unowned thread signal mask");
  require_no_pending_signals(physical_tid);
  found->signal_mask = read_thread_signal_mask(physical_tid);
}

long emulate_thread_sigprocmask(int node, pid_t physical_tid, uint64_t how,
                               uint64_t new_mask, uint64_t old_mask, uint64_t size)
{
  if (size != sizeof(uint64_t)) return -EINVAL;
  if (node < 0 || node >= NP) throw std::runtime_error("Thread pool node out of range");
  auto& state = ptmc_state.running_state.child[node];
  const auto found = std::find_if(state.threads.begin(), state.threads.end(),
      [physical_tid](const thread_state& thread) { return thread.alive && thread.physical_tid == physical_tid; });
  if (found == state.threads.end()) throw std::runtime_error("sigprocmask has no live owned thread");
  for (uint64_t address : {new_mask, old_mask})
    if (address < scratch_page + 4096 && address > scratch_page - sizeof(uint64_t))
      throw std::runtime_error("Signal mask pointer overlaps reserved control memory");
  require_no_pending_signals(physical_tid);
  const uint64_t previous = found->signal_mask;
  if (read_thread_signal_mask(physical_tid) != previous)
    throw std::runtime_error("Physical thread signal mask differs from model");
  if (new_mask) {
    uint64_t value = 0;
    if (!guest_read_exact(physical_tid, reinterpret_cast<void*>(new_mask), &value, sizeof(value)))
      return -EFAULT;
    uint64_t replacement = previous;
    switch (static_cast<int32_t>(how)) {
      case SIG_BLOCK: replacement |= value; break;
      case SIG_UNBLOCK: replacement &= ~value; break;
      case SIG_SETMASK: replacement = value; break;
      default: return -EINVAL;
    }
    replacement &= ~((uint64_t(1) << (SIGKILL - 1)) | (uint64_t(1) << (SIGSTOP - 1)));
    write_thread_signal_mask(physical_tid, replacement);
    found->signal_mask = replacement;
  }
  // Linux commits the new mask before copying oldset out, including EFAULT.
  if (old_mask && !guest_write_exact(physical_tid, reinterpret_cast<void*>(old_mask),
                                    &previous, sizeof(previous))) return -EFAULT;
  return 0;
}

void restore_thread_signal_masks(int node, const tracee_state& state)
{
  const auto& pool = pool_for(node);
  for (pid_t carrier : pool.carriers) {
    uint64_t mask = 0;
    for (const auto& thread : state.threads)
      if (thread.alive && thread.physical_tid == carrier) { mask = thread.signal_mask; break; }
    write_thread_signal_mask(carrier, mask);
  }
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
  for (pid_t tid : found->second.carriers) {
    require_thread_capture_boundary(tid);
    uint64_t mask = 0;
    for (const auto& thread : state.threads)
      if (thread.alive && thread.physical_tid == tid) { mask = thread.signal_mask; break; }
    if (read_thread_signal_mask(tid) != mask)
      throw std::runtime_error("Physical thread signal mask differs from captured model");
  }
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
  const uint64_t inherited_mask = source->signal_mask;
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
  write_thread_signal_mask(carrier, inherited_mask);
  state.add_thread(carrier, flags, stack, parent, false);
  auto& child = state.threads.back();
  child.regs = regs;
  child.xstate = xstate;
  child.signal_mask = inherited_mask;
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
  RobustExitPlan robust_plan(physical_tid);
  const auto robust_effects = robust_plan.collect(*found, logical_thread_id(node, found->tid));
  if (found->clear_child_tid && std::any_of(state.threads.begin(), state.threads.end(),
      [&found](const thread_state& thread) { return &thread != &*found && thread.alive; }))
    robust_plan.store(found->clear_child_tid, 0, true);
  if (ptrace(PTRACE_GETREGS, physical_tid, nullptr, &found->regs) == -1)
    throw std::runtime_error("Cannot capture exiting thread registers");
  found->regs.rax = 0;
  found->xstate = capture_xstate(physical_tid);
  found->exit_code = exit_code & 255;
  const auto slot = static_cast<size_t>(found - state.threads.begin());
  apply_robust_cleanup(state, *found, robust_effects);
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
  // Preflight the entire group with an overlay of the stores that earlier
  // exits perform, including aliased child-settid and clear-tid addresses.
  RobustExitPlan robust_plan(caller);
  std::vector<std::vector<RobustEffect>> robust_effects(state.threads.size());
  size_t live = std::count_if(state.threads.begin(), state.threads.end(),
      [](const thread_state& thread) { return thread.alive; });
  for (size_t i = 0; i < state.threads.size(); ++i) {
    const auto& thread = state.threads[i];
    if (!thread.alive) continue;
    if (query_clear_child_tid(thread.physical_tid))
      throw std::runtime_error("Thread carrier has unadopted inherited clear_child_tid registration");
    if (thread.child_settid_pending)
      robust_plan.store(thread.pending_child_settid, logical_thread_id(node, thread.tid));
    robust_effects[i] = robust_plan.collect(thread, logical_thread_id(node, thread.tid));
    if (thread.clear_child_tid && live > 1) robust_plan.store(thread.clear_child_tid, 0, true);
    --live;
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
    apply_robust_cleanup(state, thread, robust_effects[i]);
    clear_logical_tid(node, state, thread);
    thread.alive = false;
    thread.physical_tid = 0;
    ptmc_state.thread_blocked[node][i] = false;
    ptmc_state.thread_exited[node][i] = true;
  }
  ptmc_state.status[node] = dstatus_exit(exit_code);
}
