/*
 * state.cpp - State management
 */

#include "state.h"
#include "cereal/archives/binary.hpp"
#include "config.h"
#include "debug.h"
#include "../fs/fd_manager.h"
#include "../fs/fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "proc_status.h"
#include "state_store.h"
#include "syscall_fmt.h"
#include "sysstate_store.h"
#include "utils.h"
#include <assert.h>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <set>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>

/* ======================================================================
 * Section 1: Global State Variables
 * ====================================================================== */

PTMC_STATE ptmc_state;
TSS state_tree;
SSS state_set;

std::unordered_map<int, void *> rseq_struct;
std::unordered_map<int, int> rseq_len;

// Note: membuf removed - was never used, caused double free bug

/* ======================================================================
 * Section 2: State Logging and Display
 * ====================================================================== */

void show_syscall(int pid, hash_type ts_hash)
{
  char buf[1024];
  syscall_fmt::format(buf, pid, ts_hash);
  detsim::ui::ui_printf("%s\n", buf);
}

/* Format syscall for history display */
static void format_syscall_for_history(char *buf, int pid, hash_type ts_hash)
{
  syscall_fmt::format(buf, pid, ts_hash);
  buf[1023] = '\0'; /* Ensure null termination */
}

void show_syscall_history()
{
  hash_type ss = ptmc_state.running_state.ss_hash;
  std::stack<std::string> print_stack;
  int cnt = 0;

  while (state_tree.count(ss))
  {
    cnt++;
    hash_type pre = std::get<0>(state_tree[ss]);
    int which = std::get<1>(state_tree[ss]);
    int choose = std::get<2>(state_tree[ss]);

    sys_state s = sys_state(ss);

    char buf[1024];
    format_syscall_for_history(buf, ptmc_state.pids[which], s.ts_hash[which]);
    print_stack.push(std::string(buf) + "\n");

    if (choose >= 0)
      sprintf(buf,
              HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d (choose %d): ", pre,
              ss, which, choose);
    else
      sprintf(buf, HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d: ", pre, ss,
              which);
    print_stack.push(buf);
    ss = pre;
  }

  while (!print_stack.empty())
  {
    detsim::ui::ui_printf("%s", print_stack.top().c_str());
    print_stack.pop();
  }
  detsim::ui::ui_printf("%d steps in total.\n", cnt);
}

/* ======================================================================
 * Section 3: State Creation (from running processes)
 * ====================================================================== */

/* Create sys_state from running processes' syscall info */
/* will save ***ALL*** stuffs of a sys_state and its tracees */
/* Create sys_state with incremental save optimization
 * Only the active process is fully saved.
 * Other processes reuse old hashes if their state is unchanged.
 */
sys_state::sys_state(const struct syscall_info info[][MAX_THREADS_PER_PROCESS])
{
  auto old_state = ptmc_state.running_state;
  auto active_idx = ptmc_state.cursor;
  hash_type ss_hash_tmp = 0;
  error_bound = ptmc_state.error_bound;
  extern int auto_mode;  // For detecting manual mode

  if (old_state.ss_hash == 0)
  {
    for (int j = 0; j < NP; j++)
    {
      child[j] = tracee_state(j, info[j]);
      status[j] = ptmc_state.status[j];
      ts_hash[j] = child[j].save(ptmc_state.pids[j]);
      ss_hash_tmp ^= ts_hash[j];
    }
    ss_hash = ss_hash_tmp;
    goto ret;
  }

  for (int j = 0; j < NP; j++)
  {
    /* Create new tracee_state from running process */
    child[j] = tracee_state(j, info[j]);
    status[j] = ptmc_state.status[j];

    if (j == active_idx)
    {
      /* Active process: always full save */
      ts_hash[j] = child[j].save(ptmc_state.pids[j]);
      LOG_DEBUG("Active tstate %d saved: " HASH_FORMAT, j, ts_hash[j]);
    }
    else
    {
      /* Inactive process: check if state changed */
      if (auto_mode && child[j].metadata_equal(old_state.child[j]))  // Disable optimization in manual mode
      {
        /* State unchanged, reuse old hash */
        ts_hash[j] = old_state.ts_hash[j];
        LOG_DEBUG("Inactive tstate %d reused: " HASH_FORMAT, j, ts_hash[j]);
      }
      else
      {
        /* State changed, full save */
        ts_hash[j] = child[j].save(ptmc_state.pids[j]);
        LOG_DEBUG("Inactive tstate %d changed, saved: " HASH_FORMAT, j,
                  ts_hash[j]);
      }
    }

    ss_hash_tmp ^= ts_hash[j];
  }

ret:
  ss_hash = ss_hash_tmp;
  LOG_DEBUG("Create sstate (incremental) " HASH_FORMAT, ss_hash);
  save_metadata();
}

/* ======================================================================
 * Section 4: State Persistence (save/load to disk)
 * ====================================================================== */

int sys_state::save_metadata() const
{
  LOG_DEBUG("to store sstate %016lx", ss_hash);

  // Use SysStateStore for packed storage
  SysStateStore &store = SysStateStore::instance();

  if (!store.exists(ss_hash))
  {
    LOG_DEBUG("sys_state stores %016lx", ss_hash);

    // Serialize to memory buffer
    std::ostringstream oss;
    try
    {
      cereal::BinaryOutputArchive outputArchive(oss);
      outputArchive(*this);
    }
    catch (...)
    {
      LOG_CRIT("Failed to serialize sys_state %016lx", ss_hash);
      return 0;
    }

    std::string data_str = oss.str();
    std::vector<uint8_t> data(data_str.begin(), data_str.end());

    if (!store.save(ss_hash, data))
    {
      LOG_CRIT("Failed to save metadata to SysStateStore");
      return 0;
    }

    return 1;
  }
  return 0;
}

/* ======================================================================
 * Section 5: State Recovery (restore running process from snapshot)
 * ====================================================================== */

/* Load tracee_state from integrated StateStore data */
tracee_state::tracee_state(hash_type hash)
  : threads(), thread_create_records(), main_tid(0), current_thread_idx(0), futex_state(nullptr)
{
  // Handle invalid hash (0 means uninitialized/invalid)
  if (hash == 0)
  {
    return;
  }

  // Load integrated data from StateStore
  LOG_DEBUG("tracee_state: loading hash %016lx", hash);
  auto data_shared_ptr = StateStore::instance().load_shared(hash);

  if (!data_shared_ptr)
  {
    LOG_CRIT("Cannot load tracee_state for hash %016lx", hash);
    return;
  }

  const std::vector<uint8_t> &data = *data_shared_ptr;

  // Parse header
  if (data.size() < sizeof(StateDataHeader))
  {
    LOG_CRIT("Invalid state data for hash %016lx: too small", hash);
    return;
  }

  StateDataHeader *header = (StateDataHeader *)data.data();
  if (header->magic != 0x4453544D)
  {
    LOG_CRIT("Invalid state data for hash %016lx: bad magic", hash);
    return;
  }

  // Deserialize tracee_state from integrated data
  if (header->version >= 2 && header->tstate_size > 0)
  {
    if (header->tstate_offset + header->tstate_size <= data.size())
    {
      std::istringstream iss(std::string(
          (char *)data.data() + header->tstate_offset, header->tstate_size));
      try
      {
        cereal::BinaryInputArchive ar(iss);
        ar(*this);
      }
      catch (...)
      {
        LOG_ERROR("Failed to deserialize tracee_state for hash %016lx", hash);
      }
    }
  }
  else
  {
    LOG_WARN("StateStore data for hash %016lx has no embedded tracee_state",
             hash);
  }
}

/* Load sys_state from state_store by hash */
sys_state::sys_state(hash_type hash)
{
  ss_hash = hash;

  // Initialize ts_hash to 0 to avoid garbage values if loading fails
  for (int i = 0; i < NP; i++)
  {
    ts_hash[i] = 0;
  }

  // Try SysStateStore first (new format)
  SysStateStore &store = SysStateStore::instance();
  std::vector<uint8_t> data;

  LOG_DEBUG("sys_state: loading hash %016lx from SysStateStore", hash);

  if (store.load(hash, data))
  {
    LOG_DEBUG("sys_state: loaded hash %016lx, data size=%zu", hash,
              data.size());
    // Loaded from packed storage
    try
    {
      std::istringstream iss(std::string((char *)data.data(), data.size()));
      cereal::BinaryInputArchive inputArchive(iss);
      inputArchive(*this);
    }
    catch (...)
    {
      LOG_CRIT("Failed to deserialize sys_state %016lx from packed store",
               hash);
    }
  }
  else
  {
    LOG_CRIT("Failed to load sys_state %016lx from SysStateStore", hash);
  }

  // Load child states from integrated data (only for valid hashes)
  for (int i = 0; i < NP; i++)
  {
    if (DISALIVE(status[i]) && ts_hash[i] != 0)
    {
      child[i] = tracee_state(ts_hash[i]);
    }
  }
}

void tracee_state::restore_memory_mappings(
    hash_type ts_hash, int pid, std::vector<maps_item> &maps_out) const
{
  /* Load saved mappings from StateStore */
  std::vector<maps_item> maps_saved;
  {
    std::shared_ptr<const std::vector<uint8_t>> data_shared_ptr =
        StateStore::instance().load_shared(ts_hash);
    if (data_shared_ptr == nullptr)
    {
      LOG_CRIT("Cannot load state for hash %016lx", ts_hash);
      return;
    }

    const std::vector<uint8_t> &data = *data_shared_ptr;

    if (data.size() >= sizeof(StateDataHeader))
    {
      StateDataHeader *header = (StateDataHeader *)data.data();
      /* Try to read from integrated maps text (version 3+) */
      if (header->magic == 0x4453544D && header->version >= 3 &&
          header->maps_size > 0)
      {
        if (header->maps_offset + header->maps_size <= data.size())
        {
          const char *maps_text =
              (const char *)data.data() + header->maps_offset;
          std::istringstream iss(std::string(maps_text, header->maps_size));
          std::string line;
          while (std::getline(iss, line))
          {
            maps_item item = {};
            uint32_t offset, a, b, inode;
            if (sscanf(line.c_str(), "%lx-%lx %4s %x %d:%d %d %511s",
                       &item.start, &item.end, item.flags, &offset, &a, &b,
                       &inode, item.name) >= 7)
            {
              maps_saved.push_back(item);
            }
          }
        }
      }
    }
  }

  if (maps_saved.empty())
  {
    /* Fallback: load from separate .maps file */
    LOG_WARN("StateStore includes no mappings for tshash = %016lx", ts_hash);
    std::string maps_path =
        fileutils::format_hash_filename("mappings", ".maps", ts_hash);
    auto maps_fp = fileutils::open_cfile(maps_path, "r");
    if (maps_fp)
    {
      get_maps_item(maps_saved, maps_fp.get());
    }
  }

  /* Get current mappings */
  std::string maps_path = fmt::format("/proc/{}/maps", pid);
  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  std::vector<maps_item> maps_old;
  if (maps_fp)
  {
    get_maps_item(maps_old, maps_fp.get());
  }

  LOG_DEBUG("new maps has %d and old maps has %d items", maps_saved.size(),
            maps_old.size());

  /* Unmap regions that exist in current but not in saved */
  for (auto &item_old : maps_old)
  {
    LOG_TRACE("old item: [%lx-%lx] %s", item_old.start, item_old.end,
              item_old.name);
    if (!mapping_exists(item_old, maps_saved))
    {
      tracee_do_munmap(pid, item_old.start, item_old.end);
    }
  }

  /* Map regions that exist in saved but not in current */
  for (auto &item_new : maps_saved)
  {
    LOG_TRACE("new item: [%lx-%lx] %s", item_new.start, item_new.end,
              item_new.name);
    if (!mapping_exists(item_new, maps_old))
    {
      tracee_do_mmap(pid, item_new.start, item_new.end);
    }
  }

  /* Return saved mappings for further processing */
  maps_out = maps_saved;
}

/*
 * Optimized memory recovery with:
 * 1. Aggregated IO: Use process_vm_writev with multiple iovecs
 */
void tracee_state::recover_mem_reg_snapshot(std::vector<maps_item> &maps,
                                            hash_type ts_hash, int pid) const
{
  // Load integrated data from StateStore
  std::shared_ptr<const std::vector<uint8_t>> data_shared_ptr =
      StateStore::instance().load_shared(ts_hash);

  if (!data_shared_ptr)
  {
    LOG_CRIT("Cannot load state for hash %016lx", ts_hash);
    return;
  }

  const std::vector<uint8_t> &data = *data_shared_ptr;
  // Parse header
  if (data.size() < sizeof(StateDataHeader))
  {
    LOG_CRIT("Invalid state data for hash %016lx: too small", ts_hash);
    return;
  }

  StateDataHeader *header = (StateDataHeader *)data.data();
  if (header->magic != 0x4453544D)
  {
    LOG_CRIT("Invalid state data format for hash %016lx", ts_hash);
    return;
  }

  RegionInfo *regions = (RegionInfo *)(data.data() + sizeof(StateDataHeader));

  /* Read current rseq structure to preserve it */
  void *rseq = malloc(rseq_len[pid]);
  tracee_read_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);

  // ========================================================================
  // Optimization 1: Aggregated IO - Batch multiple regions in single syscall
  // process_vm_writev supports up to IOV_MAX (1024) iovecs
  // This reduces syscall overhead from N syscalls to ceil(N/IOV_MAX)
  // ========================================================================

  // Maximum iovecs per syscall (conservative to avoid stack overflow)
  constexpr int MAX_IOV_BATCH = 256;

  struct iovec local_iov[MAX_IOV_BATCH];
  struct iovec remote_iov[MAX_IOV_BATCH];
  int iov_count = 0;

  for (uint32_t i = 0; i < header->num_regions; i++)
  {
    uint64_t start = regions[i].start;
    uint64_t end = regions[i].end;
    uint64_t offset = regions[i].offset;
    ssize_t size = end - start;

    if (size <= 0)
      continue;

    // Check if this region is still valid in current maps
    bool valid = false;
    for (auto &item : maps)
    {
      if (item.start <= start && end <= item.end && item.flags[1] == 'w')
      {
        valid = true;
        break;
      }
    }
    if (!valid)
    {
      LOG_WARN("Region %lx-%lx not found in current maps, skipping", start,
               end);
      continue;
    }

    // Check data bounds
    if (offset + size > data.size())
    {
      LOG_WARN("Region %lx-%lx data out of bounds", start, end);
      continue;
    }

    // Fill iovec for this region
    local_iov[iov_count].iov_base = (bytes *)data.data() + offset;
    local_iov[iov_count].iov_len = size;
    remote_iov[iov_count].iov_base = (void *)start;
    remote_iov[iov_count].iov_len = size;
    iov_count++;

    LOG_TRACE("Queue restore mem %p-%p (batch %d/%d)", (void *)start,
              (void *)end, iov_count, MAX_IOV_BATCH);

    // Flush batch when full
    if (iov_count >= MAX_IOV_BATCH)
    {
      ssize_t n = syscall(SYS_process_vm_writev, pid, local_iov, iov_count,
                          remote_iov, iov_count, 0);
      if (n < 0)
      {
        LOG_ERROR("Batch process_vm_writev failed: %s", strerror(errno));
        // Fallback: try individual writes
        for (int j = 0; j < iov_count; j++)
        {
          tracee_write_mem(pid, remote_iov[j].iov_base, local_iov[j].iov_base,
                           local_iov[j].iov_len);
        }
      }
      iov_count = 0;
    }
  }

  // Flush remaining regions
  if (iov_count > 0)
  {
    ssize_t n = syscall(SYS_process_vm_writev, pid, local_iov, iov_count,
                        remote_iov, iov_count, 0);
    if (n < 0)
    {
      LOG_ERROR("Final batch process_vm_writev failed: %s", strerror(errno));
      // Fallback: try individual writes
      for (int j = 0; j < iov_count; j++)
      {
        tracee_write_mem(pid, remote_iov[j].iov_base, local_iov[j].iov_base,
                         local_iov[j].iov_len);
      }
    }
  }

  /* Restore the preserved rseq structure */
  tracee_write_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);
  free(rseq);

  // Read registers from data
  if (header->regs_offset + sizeof(struct user_regs_struct) <= data.size())
  {
    struct user_regs_struct regs;
    memcpy(&regs, data.data() + header->regs_offset, sizeof(regs));
    ptrace(PTRACE_SETREGS, pid, NULL, &regs);
    LOG_TRACE("Restored registers for hash %016lx", ts_hash);
  }
  else
  {
    LOG_ERROR("Invalid register offset in state data for hash %016lx", ts_hash);
  }
}

/* Main entry: recover running state for a process */
/* Helper: Terminate threads that shouldn't exist during recovery */
void tracee_state::terminate_extra_threads(pid_t main_pid, int threads_to_terminate) const
{
  auto current_threads = get_thread_list(main_pid);

  for (pid_t current_tid : current_threads) {
    if (threads_to_terminate <= 0) break;

    // Don't terminate the main thread
    if (current_tid == main_pid) {
      continue;
    }

    // Check if this physical TID is in the saved state as a live thread
    bool should_exist = false;
    for (const auto& ts : threads) {
      if (ts.physical_tid == current_tid) {
        should_exist = true;
        break;
      }
    }

    if (should_exist) {
      continue;  // This thread should exist, skip
    }

    LOG_INFO("Terminating thread %d", current_tid);

    // Use tracee_do_syscall to make the thread call exit
    int status;
    pid_t result = waitpid(current_tid, &status, WNOHANG);
    if (result == 0) {
      // Thread is running, we need to stop it first
      LOG_WARN("Thread %d is running, cannot terminate cleanly", current_tid);
    } else if (result == current_tid) {
      // Thread has stopped or exited
      if (WIFSTOPPED(status)) {
        // Thread is stopped, we can make it exit
        tracee_do_syscall(current_tid, SYS_exit, 0, 0, 0, 0, 0, 0);
        LOG_INFO("Terminated thread %d via exit syscall", current_tid);
        threads_to_terminate--;
      }
    }
  }
}

/* Helper: Create threads that don't exist during recovery */
void tracee_state::create_missing_threads(pid_t main_pid) const
{
  auto current_threads = get_thread_list(main_pid);
  std::set<pid_t> current_tids(current_threads.begin(), current_threads.end());

  for (const auto& target_ts : threads) {
    // Skip exited threads (physical_tid == 0)
    if (target_ts.physical_tid == 0) {
      LOG_INFO("Skipping exited thread with virtual TID %d", target_ts.tid);
      continue;
    }

    // Skip main thread (virtual TID 1)
    if (target_ts.tid == 1) continue;

    // Check if this saved thread already exists in current threads
    if (current_tids.count(target_ts.physical_tid) > 0) {
      LOG_DEBUG("Thread with virtual TID %d already exists (physical=%d)",
                target_ts.tid, target_ts.physical_tid);
      continue;
    }

    LOG_INFO("Creating thread with virtual TID %d (clone_flags=0x%lx)",
             target_ts.tid, target_ts.clone_flags);

    // Only create if we have clone flags
    if (target_ts.clone_flags != 0) {
      // Prepare clone parameters
      uint64_t flags = target_ts.clone_flags | SIGCHLD | CLONE_PTRACE;

      // Execute clone in the main thread
      pid_t new_tid = (pid_t)tracee_do_syscall((int)main_pid, SYS_clone,
                                                flags,
                                                target_ts.stack_addr,
                                                (uint64_t)target_ts.ptid,
                                                0, 0, 0);

      if (new_tid > 0) {
        LOG_INFO("Created new thread %d (requested virtual TID %d)", new_tid, target_ts.tid);

        // Wait for the new thread to stop
        int wstatus;
        pid_t waited = waitpid(new_tid, &wstatus, 0);
        if (waited == new_tid && WIFSTOPPED(wstatus)) {
          // Restore registers for the new thread
          struct user_regs_struct new_regs = target_ts.regs;
          // RAX contains return value from clone (0 for child)
          new_regs.rax = 0;
          ptrace(PTRACE_SETREGS, new_tid, NULL, &new_regs);
          LOG_INFO("Restored registers for new thread %d", new_tid);
        }
      } else {
        LOG_ERROR("Failed to create thread: %ld", (long)new_tid);
      }
    } else {
      LOG_WARN("No clone flags for thread %d, cannot recreate", target_ts.tid);
    }
  }
}

/* Helper: Restore registers for all threads during recovery */
void tracee_state::restore_thread_registers(pid_t main_pid) const
{
  auto current_threads = get_thread_list(main_pid);

  for (const auto& target_ts : threads) {
    // Skip exited threads
    if (target_ts.physical_tid == 0) {
      continue;
    }

    // Find the current physical thread that corresponds to this saved thread
    pid_t physical_tid = -1;

    // First try to match by stored physical_tid
    for (pid_t current_tid : current_threads) {
      if (current_tid == target_ts.physical_tid) {
        physical_tid = current_tid;
        break;
      }
    }

    // If exact match not found, use position-based fallback
    // This handles the case where threads were recreated
    if (physical_tid == -1) {
      int virtual_idx = target_ts.tid - 1;  // Convert to 0-based index
      if (virtual_idx >= 0 && virtual_idx < (int)current_threads.size()) {
        physical_tid = current_threads[virtual_idx];
      }
    }

    if (physical_tid > 0) {
      // Restore registers for this thread
      if (ptrace(PTRACE_SETREGS, physical_tid, NULL, (void*)&target_ts.regs) == 0) {
        LOG_TRACE("Restored registers for virtual thread %d (physical %d)",
                  target_ts.tid, physical_tid);
      } else {
        LOG_ERROR("Failed to restore registers for virtual thread %d (physical %d): %s",
                  target_ts.tid, physical_tid, strerror(errno));
      }
    } else {
      LOG_WARN("Could not find physical thread for virtual thread %d", target_ts.tid);
    }
  }
}

void tracee_state::recover_running_state(int index, hash_type ts_hash) const
{
  pid_t main_pid = ptmc_state.pids[index];
  std::vector<maps_item> maps;
  restore_memory_mappings(ts_hash, main_pid, maps);

  /* Restore brk (heap) - must be done before restoring memory content */
  tracee_do_syscall(main_pid, SYS_brk, brk, 0, 0, 0, 0, 0);
  LOG_DEBUG("restored brk = 0x%x", brk);

  recover_mem_reg_snapshot(maps, ts_hash, main_pid);

  /* ======================================================================
   * Multi-threading support: restore thread state
   * ====================================================================== */

  // Count non-exited threads in saved state
  size_t saved_live_thread_count = 0;
  for (const auto& ts : threads) {
    if (ts.physical_tid != 0) {
      saved_live_thread_count++;
    }
  }

  // Get current thread list
  auto current_threads = get_thread_list(main_pid);

  LOG_INFO("Recovering threads for process %d: current=%zu, saved_live=%zu, saved_total=%zu",
           index, current_threads.size(), saved_live_thread_count, threads.size());

  // Step 1: Terminate threads that shouldn't exist
  if (current_threads.size() > saved_live_thread_count) {
    int threads_to_terminate = current_threads.size() - saved_live_thread_count;
    LOG_INFO("Need to terminate %d threads", threads_to_terminate);
    terminate_extra_threads(main_pid, threads_to_terminate);
  }

  // Step 2: Create threads that don't exist
  create_missing_threads(main_pid);

  // Step 3: Restore registers for all threads
  restore_thread_registers(main_pid);

  // Restore current thread index
  ptmc_state.current_thread_idx[index] = current_thread_idx;

  /* Restore subsystems */
  ptmc_state.sock_states[index] = sock_state;
  ptmc_state.fs_states[index] = fs_state;
  ptmc_state.raft_states[index] = raft_state;

  /* Restore FdManager state */
  *ptmc_state.fd_managers[index] = fd_manager_state;

  /* Re-link FdManager after deserialization */
  ptmc_state.sock_states[index].set_fd_manager(ptmc_state.fd_managers[index]);
  ptmc_state.fs_states[index].set_fd_manager(ptmc_state.fd_managers[index]);

  /* Restore time */
  ptmc_state.time[index] = tv;
}

/* ======================================================================
 * Section 6: State Queue & Tree Management
 * ====================================================================== */

void state_queue_append(sys_state &s)
{
  StateStore::instance().queue_push_back(s.ss_hash);
}

std::optional<sys_state> state_queue_extract()
{
  hash_type hash = StateStore::instance().queue_try_pop_front();
  if (hash == 0)
    return std::nullopt;

  sys_state s(hash);
  for (int i = 0; i < NP; i++)
    if (s.ts_hash[i] == 0)
      LOG_CRIT("Child state hash is 0 for tracee %d. This should not happen if "
               "the state is properly saved.",
               i);
  return s;
}

void state_tree_add(const sys_state &s, sys_state &t, int which, int choose)
{
  if (state_tree.count(t.ss_hash) == 0)
    state_tree[t.ss_hash] =
        std::tuple<hash_type, int, int>(s.ss_hash, which, choose);
}

/* ======================================================================
 * Explicit Memory Cleanup
 * ====================================================================== */

void tracee_state::clear()
{
  // Reset to default state to free all allocated memory
  *this = tracee_state();
}

void sys_state::clear()
{
  // Reset to default state to free all allocated memory
  *this = sys_state();
}

/* ======================================================================
 * Section 7: Memory Capture
 * ====================================================================== */


/*
 * Capture memory, registers, and tracee_state, store via StateStore
 * Optimized with batched process_vm_readv
 */
hash_type tracee_state::save_full_state_to_state_store(int pid)
{
  std::string maps_path = fmt::format("/proc/{}/maps", pid);

  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  if (!maps_fp)
  {
    LOG_CRIT("Failed to open /proc/{}/maps", pid);
    exit(1);
  }

  // ========================================================================
  // First pass: collect all writable regions
  // ========================================================================
  struct RegionToCapture
  {
    uint64_t start;
    uint64_t end;
    size_t size;
    char flags[5];
    char name[64];
    int prot;
  };
  std::vector<RegionToCapture> regions_to_capture;

  char line[1024];
  while (fgets(line, sizeof(line), maps_fp.get()))
  {
    uint64_t start, end;
    char flags[5];
    uint32_t offset;
    int a, b, inode;
    char name[64] = {};
    sscanf(line, "%lx-%lx %4s %x %d:%d %d %63s", &start, &end, flags, &offset,
           &a, &b, &inode, name);

    /* Only save writable regions */
    if (flags[1] != 'w')
      continue;
    if (start == scratch_page)
      continue;

    RegionToCapture region;
    region.start = start;
    region.end = end;
    region.size = end - start;
    memcpy(region.flags, flags, 5);
    strncpy(region.name, name, 63);
    region.prot = 0;
    if (flags[0] == 'r')
      region.prot |= PROT_READ;
    if (flags[1] == 'w')
      region.prot |= PROT_WRITE;
    if (flags[2] == 'x')
      region.prot |= PROT_EXEC;
    regions_to_capture.push_back(region);
  }

  // ========================================================================
  std::vector<RegionInfo> regions;
  regions.reserve(regions_to_capture.size());

  size_t regions_data_size = 0;
  for (auto &src : regions_to_capture)
  {
    RegionInfo info;
    info.start = src.start;
    info.end = src.end;
    info.prot = src.prot;
    memcpy(info.flags, src.flags, 5);
    info.offset = regions_data_size;
    regions.push_back(info);
    regions_data_size += src.size;

    LOG_TRACE("dump %lx-%lx (prot=%x), %s", src.start, src.end, src.prot,
              src.name);
  }

  // Read mappings data from /proc/PID/maps
  // Note: procfs files don't support fseek/ftell, must read until EOF
  std::vector<uint8_t> maps_data;
  {
    std::string maps_path = fmt::format("/proc/{}/maps", pid);
    LOG_TRACE("Reading maps from %s", maps_path.c_str());
    FILE *maps_fp = fopen(maps_path.c_str(), "r");
    if (maps_fp)
    {
      char buffer[4096];
      size_t total_read = 0;
      while (!feof(maps_fp))
      {
        size_t n = fread(buffer, 1, sizeof(buffer), maps_fp);
        if (n > 0)
        {
          maps_data.insert(maps_data.end(), buffer, buffer + n);
          total_read += n;
        }
        if (ferror(maps_fp) && !feof(maps_fp))
        {
          LOG_ERROR("Error reading maps file");
          break;
        }
      }
      fclose(maps_fp);
      LOG_TRACE("Maps data read: %zu bytes, data size: %zu", total_read,
                maps_data.size());
    }
    else
    {
      LOG_ERROR("Failed to open %s", maps_path.c_str());
    }
  }

  // ========================================================================
  // Multi-threading support: save all thread registers
  // Use virtual TIDs (thread indices + 1) for consistency across save/restore
  // ========================================================================
  threads.clear();
  auto thread_list = get_thread_list(pid);

  int virtual_tid = 1;
  for (pid_t physical_tid : thread_list) {
    struct user_regs_struct thread_regs;
    if (ptrace(PTRACE_GETREGS, physical_tid, NULL, &thread_regs) == 0) {
      thread_state ts;
      ts.tid = virtual_tid;  // Use virtual TID (index + 1)
      ts.regs = thread_regs;

      // Store physical TID for stable mapping
      ts.physical_tid = physical_tid;

      // Find clone flags from creation records by virtual TID
      ts.clone_flags = 0;
      for (const auto& record : thread_create_records) {
        if ((int)record.virtual_tid == ts.tid) {
          ts.clone_flags = record.clone_flags;
          ts.stack_addr = record.stack_addr;
          ts.ptid = record.ptid;
          break;
        }
      }

      ts.is_main = (physical_tid == pid);
      threads.push_back(ts);

      LOG_TRACE("Saved thread physical=%d virtual=%d (main=%d) registers",
                physical_tid, ts.tid, ts.is_main);
    } else {
      LOG_WARN("Failed to get registers for thread %d", physical_tid);
    }
    virtual_tid++;
  }

  if (!threads.empty()) {
    main_tid = 1;  // Main thread is always virtual TID 1
    // Use the currently selected thread index from ptmc_state
    int tracee_idx = -1;
    for (int i = 0; i < NP; i++) {
      if (ptmc_state.pids[i] == pid) {
        tracee_idx = i;
        break;
      }
    }
    if (tracee_idx >= 0) {
      current_thread_idx = ptmc_state.current_thread_idx[tracee_idx];
    } else {
      current_thread_idx = 0;
    }
  }

  LOG_INFO("Saved state for %zu threads in process %d", threads.size(), pid);

  // Get main thread registers (for backward compatibility)
  struct user_regs_struct regs;
  ptrace(PTRACE_GETREGS, pid, NULL, &regs);

  // Serialize tracee_state AFTER threads have been populated
  std::vector<uint8_t> tstate_data;
  {
    std::ostringstream oss;
    {
      cereal::BinaryOutputArchive ar(oss);
      ar(*this);
    }
    std::string str = oss.str();
    tstate_data.assign(str.begin(), str.end());
  }

  // Allocate final buffer
  size_t header_size = sizeof(StateDataHeader);
  size_t regions_size = regions.size() * sizeof(RegionInfo);
  size_t tstate_size = tstate_data.size();
  size_t maps_size = maps_data.size();

  size_t total_size = header_size + regions_size + tstate_size + maps_size +
                      regions_data_size + sizeof(regs);
  uint8_t *all_data = new uint8_t[total_size];

  StateDataHeader *header = (StateDataHeader *)all_data;
  header->magic = 0x4453544D;
  header->version = 3;
  header->num_regions = regions.size();
  header->tstate_offset = header_size + regions_size;
  header->tstate_size = tstate_size;
  header->maps_offset = header_size + regions_size + tstate_size;
  header->maps_size = maps_size;
  header->regs_offset =
      header_size + regions_size + tstate_size + maps_size + regions_data_size;

  // Fill region info and read data directly into all_data
  RegionInfo *region_infos = (RegionInfo *)(all_data + header_size);
  size_t region_data_offset =
      header_size + regions_size + tstate_size + maps_size;

  constexpr int MAX_IOV_BATCH = 256;
  for (size_t batch_start = 0; batch_start < regions.size();
       batch_start += MAX_IOV_BATCH)
  {
    size_t batch_end = std::min(batch_start + MAX_IOV_BATCH, regions.size());
    size_t batch_count = batch_end - batch_start;

    struct iovec local_iov[MAX_IOV_BATCH];
    struct iovec remote_iov[MAX_IOV_BATCH];

    for (size_t i = 0; i < batch_count; i++)
    {
      size_t idx = batch_start + i;
      auto &src = regions_to_capture[idx];
      // Include protection and flags too: every byte enters the state hash.
      region_infos[idx] = regions[idx];
      region_infos[idx].offset = region_data_offset + regions[idx].offset;

      uint8_t *dest = all_data + region_infos[idx].offset;
      local_iov[i].iov_base = dest;
      local_iov[i].iov_len = src.size;
      remote_iov[i].iov_base = (void *)src.start;
      remote_iov[i].iov_len = src.size;
    }

    ssize_t n = syscall(SYS_process_vm_readv, pid, local_iov, batch_count,
                        remote_iov, batch_count, 0);
    if (n < 0)
    {
      LOG_ERROR("Batch process_vm_readv failed: %s", strerror(errno));
      for (size_t i = 0; i < batch_count; i++)
      {
        size_t idx = batch_start + i;
        auto &src = regions_to_capture[idx];
        uint8_t *dest = all_data + region_infos[idx].offset;
        struct iovec local = {dest, src.size};
        struct iovec remote = {(void *)src.start, src.size};
        ssize_t nr =
            syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0);
        if ((size_t)nr != src.size)
        {
          LOG_ERROR("Fallback read failed for %p-%p", (void *)src.start,
                    (void *)(src.start + src.size));
          memset(dest, 0, src.size);
        }
      }
    }

    for (size_t i = 0; i < batch_count; i++)
    {
      size_t idx = batch_start + i;
      auto &src = regions_to_capture[idx];
      if ((uintptr_t)rseq_struct[pid] >= src.start &&
          (uintptr_t)rseq_struct[pid] < src.end)
      {
        uintptr_t rseq_offset = (uintptr_t)rseq_struct[pid] - src.start;
        uint8_t *dest = all_data + region_infos[idx].offset;
        memset(dest + rseq_offset, 0x55, rseq_len[pid]);
      }
    }
  }

  memcpy(all_data + header->tstate_offset, tstate_data.data(), tstate_size);

  if (maps_size > 0)
  {
    memcpy(all_data + header->maps_offset, maps_data.data(), maps_size);
    LOG_TRACE("Copied maps data: offset=%lu, size=%zu", header->maps_offset,
              maps_size);
  }

  memcpy(all_data + header->regs_offset, &regs, sizeof(regs));

  hash_type ts_hash = StateStore::instance().save(all_data, total_size);

  delete[] all_data;

  LOG_DEBUG("StateStore saved hash %016lx, size=%zu, regions=%zu, tstate=%zu, "
            "maps=%zu",
            ts_hash, total_size, regions.size(), tstate_size, maps_size);
  return ts_hash;
}

/* ======================================================================
 * Section 8: Tracee State Lifecycle
 * ====================================================================== */

/* Create tracee_state from running process (capture) */
tracee_state::tracee_state(int which, const struct syscall_info info[])
  : threads(), thread_create_records(), main_tid(0), current_thread_idx(0), futex_state(nullptr)
{
  /* Save syscall info */
  memcpy(si, info, sizeof(syscall_info) * MAX_THREADS_PER_PROCESS);

  /* Save subsystems */
  sock_state = ptmc_state.sock_states[which];
  fs_state = ptmc_state.fs_states[which];
  raft_state = ptmc_state.raft_states[which];

  /* Save FdManager state */
  if (ptmc_state.fd_managers[which])
  {
    fd_manager_state = *ptmc_state.fd_managers[which];
  }

  /* Save time */
  tv = ptmc_state.time[which];

  /* Initialize brk */
  brk = 0;
}

/* Full save: capture memory, mappings, and serialize */
hash_type tracee_state::save(int pid)
{
  /* Record brk */
  brk = tracee_do_syscall(pid, SYS_brk, 0, 0, 0, 0, 0, 0);
  LOG_DEBUG("recorded brk = 0x%x", brk);

  /* Capture memory state (includes tstate) */
  return save_full_state_to_state_store(pid);
}

/* Equality comparison for incremental save optimization
 * Uses serialization to compare complex nested state structures.
 * This is slower than direct field comparison but doesn't require
 * defining operator== for all nested types.
 */
bool tracee_state::metadata_equal(const tracee_state &other) const
{
  // Compare complex state using serialization
  auto serialize_to_string = [](const tracee_state &ts) -> std::string
  {
    std::ostringstream oss;
    {
      cereal::BinaryOutputArchive ar(oss);
      ar(ts);
    }
    return oss.str();
  };

  return serialize_to_string(*this) == serialize_to_string(other);
}

/* ======================================================================
 * Section 8.5: Thread Management Methods
 * ====================================================================== */

void tracee_state::add_thread(pid_t physical_tid, uint64_t clone_flags, uint64_t stack, pid_t ptid, bool is_main)
{
  // Assign virtual TID (thread index + 1) for consistency across save/restore
  int virtual_tid = threads.size() + 1;

  thread_state ts;
  ts.tid = virtual_tid;  // Use virtual TID
  ts.physical_tid = physical_tid;  // Store physical TID
  ts.clone_flags = clone_flags;
  ts.stack_addr = stack;
  ts.ptid = ptid;
  ts.is_main = is_main;

  // Record creation info with both virtual and physical TID
  thread_create_info record;
  record.virtual_tid = virtual_tid;
  record.physical_tid = physical_tid;
  record.clone_flags = clone_flags;
  record.stack_addr = stack;
  record.ptid = ptid;
  thread_create_records.push_back(record);

  threads.push_back(ts);

  if (is_main) {
    main_tid = virtual_tid;
  }

  LOG_INFO("Added thread physical=%d virtual=%d (main=%d, flags=0x%lx)",
           physical_tid, virtual_tid, is_main, clone_flags);
}

void tracee_state::remove_thread(pid_t tid)
{
  // Find and mark thread as exited by setting physical_tid to 0
  // We keep the entry in the vector to maintain stable virtual TID mapping
  for (auto& ts : threads) {
    if (ts.tid == tid) {
      LOG_INFO("Marked thread %d (physical=%d) as exited", tid, ts.physical_tid);
      ts.physical_tid = 0;  // Mark as exited
      return;
    }
  }
}

thread_state* tracee_state::find_thread(pid_t tid)
{
  for (auto& ts : threads) {
    if (ts.tid == tid && ts.physical_tid != 0) {
      return &ts;
    }
  }
  return nullptr;
}

const thread_state* tracee_state::find_thread(pid_t tid) const
{
  for (const auto& ts : threads) {
    if (ts.tid == tid && ts.physical_tid != 0) {
      return &ts;
    }
  }
  return nullptr;
}

int tracee_state::find_thread_index(pid_t tid) const
{
  for (size_t i = 0; i < threads.size(); i++) {
    if (threads[i].tid == tid && threads[i].physical_tid != 0) {
      return i;
    }
  }
  return -1;
}

/* ======================================================================
 * Section 9: Memory Operations
 * ====================================================================== */

/* Read memory from snapshot by hash using StateStore */
void *read_mem(int pid, hash_type ts_hash, uint64_t addr, long size)
{
  bytes *ret = (bytes *)malloc(size + 1);
  ret[size] = 0;

  // Load state data from StateStore
  auto data_shared_ptr = StateStore::instance().load_shared(ts_hash);

  if (!data_shared_ptr)
  {
    LOG_CRIT("Cannot load state for hash %016lx in read_mem", ts_hash);
    // Fallback: try reading from live process
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }

  const std::vector<uint8_t> &data = *data_shared_ptr;

  // Parse header
  if (data.size() < sizeof(StateDataHeader))
  {
    LOG_CRIT("Invalid state data for hash %016lx", ts_hash);
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }

  StateDataHeader *header = (StateDataHeader *)data.data();
  if (header->magic != 0x4453544D)
  {
    LOG_CRIT("Invalid state data format for hash %016lx", ts_hash);
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }

  RegionInfo *regions = (RegionInfo *)(data.data() + sizeof(StateDataHeader));

  // Find region containing addr
  for (uint32_t i = 0; i < header->num_regions; i++)
  {
    if (regions[i].start <= addr && addr < regions[i].end)
    {
      uint64_t offset_in_region = addr - regions[i].start;
      uint64_t data_offset = regions[i].offset + offset_in_region;
      size_t bytes_available = regions[i].end - addr;
      size_t bytes_to_copy = std::min((size_t)size, bytes_available);

      if (data_offset + bytes_to_copy <= data.size())
      {
        memcpy(ret, data.data() + data_offset, bytes_to_copy);
        LOG_TRACE("read addr %p from state data (region %u)", (void *)addr, i);
      }
      else
      {
        LOG_ERROR("Data offset out of bounds for addr %p", (void *)addr);
      }
      return ret;
    }
  }

  // Region not found in saved state, try live process
  LOG_TRACE("addr %p not in saved state, reading from live process",
            (void *)addr);
  tracee_read_mem(pid, (void *)addr, ret, size);
  return ret;
}

/* ======================================================================
 * Section 10: Helper Functions
 * ====================================================================== */

void get_maps_item(std::vector<maps_item> &items, FILE *maps)
{
  maps_item item;
  char line[1024];
  while (fgets(line, sizeof(line), maps) != NULL)
  {
    /* Use width-limited format specifiers to prevent buffer overflow */
    if (sscanf(line, "%lx-%lx %4s %x %d:%d %d %511s", &item.start, &item.end,
               item.flags, &item.offset, &item.a, &item.b, &item.inode,
               item.name) >= 7)
    {
      items.emplace_back(item);
    }
  }
}

static bool maps_item_eq(maps_item &a, maps_item &b)
{
  return a.start == b.start && a.end == b.end;
}

bool mapping_exists(maps_item &a, std::vector<maps_item> &array)
{
  /* heap is managed by SYS_brk */
  if (!strcmp(a.name, "[heap]"))
    return true;

  for (auto &item : array)
  {
    if (maps_item_eq(a, item))
      return true;
  }
  return false;
}

/* ======================================================================
 * Section 12: Statistics
 * ====================================================================== */

void show_state_stats()
{
  detsim::ui::ui_printf("State set size: %zu\n", state_set.size());
  detsim::ui::ui_printf("Queue size:     %zu\n",
                        StateStore::instance().queue_size());
}

/* ======================================================================
 * Section 11: sys_state Recovery
 * ====================================================================== */

void sys_state::recover_running_state() const
{
  ptmc_state.error_bound = error_bound;
  for (int i = 0; i < NP; i++)
  {
    if (ts_hash[i] == ptmc_state.running_state.ts_hash[i])
    {
      LOG_DEBUG("tracee %d already in running state, skipping recovery", i);
      continue;
    }

    // Update ptmc_state.status from saved state
    // This ensures loaded states have correct process status
    ptmc_state.status[i] = status[i];

    // Also reset thread_exited array based on saved thread state
    // This is important for multi-threading: when loading a state,
    // we need to restore which threads were marked as exited
    const auto& saved_threads = child[i].threads;
    for (int t = 0; t < 64; t++) {
      if (t < (int)saved_threads.size()) {
        ptmc_state.thread_exited[i][t] = (saved_threads[t].physical_tid == 0);
      } else {
        ptmc_state.thread_exited[i][t] = false;
      }
    }

    if (DISALIVE(status[i]))
    {
      child[i].recover_running_state(i, ts_hash[i]);
    }
  }
}

/* ======================================================================
 * Section 13: Global Cleanup
 * ====================================================================== */

void cleanup_all()
{
  detsim::ui::ui_printf("[DEBUG] Starting global cleanup...\n");

  /* 0. Cleanup DWARF resources */
  detsim::ui::ui_printf("[DEBUG] Cleaning up DWARF...\n");
  cleanup_dwarf();

  /* 1. Shutdown StateStore - stops threads and clears caches */
  detsim::ui::ui_printf("[DEBUG] Shutting down StateStore...\n");
  StateStore::instance().shutdown();

  /* 2. Shutdown SysStateStore */
  detsim::ui::ui_printf("[DEBUG] Shutting down SysStateStore...\n");
  SysStateStore::instance().shutdown();

  /* 3. Clear ptmc_state to free all memory */
  detsim::ui::ui_printf("[DEBUG] Clearing ptmc_state...\n");
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.sock_states[i] = SockState();
    ptmc_state.fs_states[i] = FileSystemState();
    ptmc_state.fd_managers[i].reset();
    ptmc_state.raft_states[i] = raft_check_state();
  }

  /* 4. Clear global containers */
  detsim::ui::ui_printf("[DEBUG] Clearing global containers...\n");
  state_tree.clear();
  state_set.clear();

  /* 5. Free rseq structures */
  /*
  detsim::ui::ui_printf("[DEBUG] Freeing rseq structures...\n");
  for (auto& pair : rseq_struct) {
    free(pair.second);
  }
  rseq_struct.clear();
  rseq_len.clear();
  */

  /* 6. Cleanup readline resources */
  detsim::ui::ui_printf("[DEBUG] Cleaning up readline...\n");
  cleanup_readline();

  /* 7. Cleanup config allocated memory */
  detsim::ui::ui_printf("[DEBUG] Cleaning up config...\n");
  cleanup_config();

  detsim::ui::ui_printf("[DEBUG] Global cleanup completed\n");
}
