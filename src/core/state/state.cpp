/*
 * state.cpp - State management
 */

#include "state.h"
#include "xstate.h"
#include <stdexcept>
#include <climits>
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

// Check before interpreting an archive: v3 thread records lack XSTATE.
static void validate_state_header(const std::vector<uint8_t>& data)
{
  if (data.size() < sizeof(StateDataHeader))
    throw std::runtime_error("Truncated state snapshot header");
  StateDataHeader header;
  memcpy(&header, data.data(), sizeof(header));
  if (header.magic != 0x4453544D || header.version != 5)
    throw std::runtime_error("Incompatible state snapshot: requires format version 5 (thread/futex state)");
  auto in_bounds = [&](uint64_t offset, uint64_t size) {
    return offset <= data.size() && size <= data.size() - offset;
  };
  if (header.num_regions > (data.size() - sizeof(header)) / sizeof(RegionInfo) ||
      !header.tstate_size || !in_bounds(header.tstate_offset, header.tstate_size) ||
      !in_bounds(header.maps_offset, header.maps_size) ||
      !in_bounds(header.regs_offset, sizeof(user_regs_struct)))
    throw std::runtime_error("Invalid state snapshot section bounds");
  const uint64_t table_end = sizeof(header) + uint64_t(header.num_regions) * sizeof(RegionInfo);
  if (header.tstate_offset != table_end ||
      header.maps_offset != header.tstate_offset + header.tstate_size ||
      header.regs_offset < header.maps_offset + header.maps_size ||
      header.regs_offset + sizeof(user_regs_struct) != data.size())
    throw std::runtime_error("Invalid state snapshot section ordering");
  uint64_t next_data = header.maps_offset + header.maps_size;
  uint64_t previous_end = 0;
  for (uint32_t i = 0; i < header.num_regions; ++i) {
    RegionInfo region;
    memcpy(&region, data.data() + sizeof(header) + i * sizeof(region), sizeof(region));
    const uint32_t prot = (region.flags[0] == 'r' ? PROT_READ : 0) |
                          (region.flags[1] == 'w' ? PROT_WRITE : 0) |
                          (region.flags[2] == 'x' ? PROT_EXEC : 0);
    if (region.flags[0] != 'r' || (region.flags[1] != 'w' && region.flags[1] != '-') ||
        (region.flags[2] != 'x' && region.flags[2] != '-') || region.flags[3] != 'p' ||
        region.flags[4] != 0 || region.prot != prot)
      throw std::runtime_error("Invalid snapshot memory region protection");
    if (region.end <= region.start || region.start < previous_end ||
        region.offset != next_data || region.offset > header.regs_offset ||
        region.end - region.start > header.regs_offset - region.offset)
      throw std::runtime_error("Invalid state snapshot memory region");
    previous_end = region.end;
    next_data += region.end - region.start;
  }
  if (next_data != header.regs_offset)
    throw std::runtime_error("Invalid state snapshot memory extent");
}

static void checked_memory_io(bool write, pid_t pid, const iovec* local,
                              const iovec* remote, size_t count)
{
  size_t expected = 0;
  for (size_t i = 0; i < count; ++i) {
    if (local[i].iov_len != remote[i].iov_len ||
        local[i].iov_len > size_t(SSIZE_MAX) - expected)
      throw std::runtime_error("Invalid snapshot memory transfer size");
    expected += local[i].iov_len;
  }
  if (!expected) return;
  const ssize_t n = write ? process_vm_writev(pid, local, count, remote, count, 0)
                          : process_vm_readv(pid, local, count, remote, count, 0);
  if (n < 0 || size_t(n) != expected)
    throw std::runtime_error(std::string(write ? "Snapshot memory write" : "Snapshot memory read") +
                             " incomplete: " + std::to_string(n) + "/" + std::to_string(expected));
}

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
  error_bound = ptmc_state.error_bound;
  extern int auto_mode;  // For detecting manual mode

  if (old_state.ss_hash == 0)
  {
    for (int j = 0; j < NP; j++)
    {
      child[j] = tracee_state(j, info[j]);
      status[j] = ptmc_state.status[j];
      ts_hash[j] = child[j].save(ptmc_state.pids[j]);
    }
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

  }

ret:
  for (hash_type hash : ts_hash)
    if (!hash) throw std::runtime_error("Failed to capture tracee state");
  ss_hash = identity_hash();
  LOG_DEBUG("Create sstate (incremental) " HASH_FORMAT, ss_hash);
  save_metadata();
}

/* ======================================================================
 * Section 4: State Persistence (save/load to disk)
 * ====================================================================== */

hash_type sys_state::identity_hash() const
{
  std::ostringstream encoded;
  {
    cereal::BinaryOutputArchive archive(encoded);
    const uint32_t domain = 0x44535334; // DSS4: ordered system metadata
    const uint32_t nodes = NP;
    archive(domain, nodes, *this);
  }
  const std::string bytes = encoded.str();
  const hash_type result = XXHash64::hash(bytes.data(), bytes.size());
  if (result == 0)
    throw std::runtime_error("Reserved zero system-state identity");
  return result;
}

int sys_state::save_metadata() const
{
  if (ss_hash != identity_hash())
    throw std::runtime_error("System-state metadata does not match its identity");
  std::ostringstream encoded;
  {
    cereal::BinaryOutputArchive archive(encoded);
    archive(*this);
  }
  const std::string bytes = encoded.str();
  const std::vector<uint8_t> data(bytes.begin(), bytes.end());
  auto &store = SysStateStore::instance();
  if (store.exists(ss_hash))
  {
    std::vector<uint8_t> previous;
    if (!store.load(ss_hash, previous) || previous != data)
      throw std::runtime_error("System-state identity collision or corrupt metadata");
    return 0;
  }
  if (!store.save(ss_hash, data))
    throw std::runtime_error("Failed to persist system-state metadata");
  return 1;
}

/* ======================================================================
 * Section 5: State Recovery (restore running process from snapshot)
 * ====================================================================== */

/* Load tracee_state from integrated StateStore data */
tracee_state::tracee_state(hash_type hash)
  : threads(), thread_create_records(), main_tid(0), current_thread_idx(0), futex_state()
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
    throw std::runtime_error("Cannot load tracee_state snapshot");
  }

  const std::vector<uint8_t> &data = *data_shared_ptr;
  validate_state_header(data);

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
        throw std::runtime_error("Failed to deserialize tracee_state snapshot");
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
sys_state::sys_state(hash_type hash) : sys_state()
{
  if (!hash)
    throw std::runtime_error("Cannot load zero system-state identity");
  std::vector<uint8_t> data;
  if (!SysStateStore::instance().load(hash, data))
    throw std::runtime_error("Cannot load system-state metadata");
  std::istringstream encoded(std::string(data.begin(), data.end()));
  {
    cereal::BinaryInputArchive archive(encoded);
    archive(*this);
  }
  if (encoded.peek() != std::char_traits<char>::eof() || identity_hash() != hash)
    throw std::runtime_error("Incompatible or corrupt system-state metadata identity");
  ss_hash = hash;
  for (int i = 0; i < NP; ++i)
  {
    if (!ts_hash[i])
      throw std::runtime_error("Missing tracee identity in system-state metadata");
    if (DISALIVE(status[i]))
      child[i] = tracee_state(ts_hash[i]);
  }
}

static bool excluded_mapping(const maps_item& item)
{
  if (item.start < scratch_page + PAGE_SIZE && scratch_page < item.end) {
    if (item.start != scratch_page || item.end != scratch_page + PAGE_SIZE)
      throw std::runtime_error("Unsupported mapping overlaps reserved scratch page");
    return true;
  }
  return !strncmp(item.name, "[vvar", 5) || !strcmp(item.name, "[vdso]") ||
         !strcmp(item.name, "[vsyscall]");
}

static int mapping_protection(const char* flags)
{
  return (flags[0] == 'r' ? PROT_READ : 0) |
         (flags[1] == 'w' ? PROT_WRITE : 0) |
         (flags[2] == 'x' ? PROT_EXEC : 0);
}

static void restore_protection(pid_t pid, uint64_t start, uint64_t end, int prot)
{
  if (tracee_do_syscall(pid, SYS_mprotect, start, end - start, prot, 0, 0, 0) != 0)
    throw std::runtime_error("Cannot restore snapshot memory protection");
}

static bool same_backing(const maps_item& saved, const maps_item& live, uint64_t address)
{
  if (saved.flags[3] != live.flags[3] || saved.inode != live.inode ||
      saved.a != live.a || saved.b != live.b)
    return false;
  // Anonymous splits need no file-offset adjustment. File mappings do.
  return !saved.inode || uint64_t(saved.offset) + address - saved.start ==
                          uint64_t(live.offset) + address - live.start;
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
      throw std::runtime_error("Cannot load snapshot mappings");
    }

    const std::vector<uint8_t> &data = *data_shared_ptr;
    validate_state_header(data);

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
            if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %511s",
                       &item.start, &item.end, item.flags, &item.offset, &item.a, &item.b,
                       &item.inode, item.name) >= 7)
            {
              if (item.end <= item.start)
                throw std::runtime_error("Invalid saved memory mapping extent");
              maps_saved.push_back(item);
            }
            else
              throw std::runtime_error("Invalid saved memory mapping text");
          }
        }
      }
    }
  }

  if (maps_saved.empty())
    throw std::runtime_error("Snapshot contains no memory mappings");

  /* Get current mappings */
  std::string maps_path = fmt::format("/proc/{}/maps", pid);
  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  std::vector<maps_item> maps_old;
  if (!maps_fp)
    throw std::runtime_error("Cannot read current memory mappings for restore");
  get_maps_item(maps_old, maps_fp.get());
  if (ferror(maps_fp.get()) || maps_old.empty())
    throw std::runtime_error("Incomplete current memory mappings for restore");

  LOG_DEBUG("new maps has %d and old maps has %d items", maps_saved.size(),
            maps_old.size());

  // Restore ranges rather than matching VMA endpoints: mprotect may split or
  // merge VMAs while leaving the same underlying private mapping intact.
  for (const auto& saved : maps_saved) {
    if (excluded_mapping(saved) || !strcmp(saved.name, "[heap]")) continue;
    if (saved.flags[3] != 'p')
      throw std::runtime_error("Shared mappings are outside snapshot restore support");
    uint64_t cursor = saved.start;
    for (const auto& live : maps_old) {
      if (live.end <= cursor || live.start >= saved.end) continue;
      if (live.start > cursor) break;
      if (!same_backing(saved, live, cursor)) break;
      cursor = std::min(saved.end, live.end);
      if (cursor == saved.end) break;
    }
    if (cursor != saved.end) {
      if (saved.inode || saved.name[0])
        throw std::runtime_error("Cannot recreate missing or replaced file-backed/named snapshot mapping");
      void* result = tracee_do_mmap(pid, saved.start, saved.end, mapping_protection(saved.flags));
      if (reinterpret_cast<uint64_t>(result) != saved.start)
        throw std::runtime_error("Cannot recreate anonymous snapshot mapping");
    }
  }

  // Remove only portions absent from the saved map, preserving split/merged
  // neighbors and the tracer's injection page. Heap extent is restored by brk.
  for (const auto& live : maps_old) {
    if (excluded_mapping(live) || !strcmp(live.name, "[heap]")) continue;
    uint64_t cursor = live.start;
    for (const auto& saved : maps_saved) {
      if (saved.end <= cursor || saved.start >= live.end) continue;
      if (saved.start > cursor &&
          tracee_do_syscall(pid, SYS_munmap, cursor, saved.start - cursor, 0, 0, 0, 0) != 0)
        throw std::runtime_error("Cannot remove extra snapshot mapping range");
      cursor = std::min(live.end, saved.end);
      if (cursor == live.end) break;
    }
    if (cursor < live.end &&
        tracee_do_syscall(pid, SYS_munmap, cursor, live.end - cursor, 0, 0, 0, 0) != 0)
      throw std::runtime_error("Cannot remove extra snapshot mapping range");
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
    throw std::runtime_error("Cannot load snapshot memory");
  }

  const std::vector<uint8_t> &data = *data_shared_ptr;
  validate_state_header(data);
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
  if (rseq_len[pid] < 0)
    throw std::runtime_error("Invalid rseq size");
  std::vector<uint8_t> rseq(rseq_len[pid]);
  iovec rseq_local{rseq.data(), rseq.size()};
  iovec rseq_remote{rseq_struct[pid], rseq.size()};
  checked_memory_io(false, pid, &rseq_local, &rseq_remote, 1);

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
  std::vector<maps_item> current_maps;
  auto current_maps_file = fileutils::open_cfile(fmt::format("/proc/{}/maps", pid), "r");
  if (!current_maps_file) throw std::runtime_error("Cannot inspect restore memory permissions");
  get_maps_item(current_maps, current_maps_file.get());
  std::vector<uint32_t> protected_regions;
  std::vector<uint8_t> comparison;

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
      if (item.start <= start && end <= item.end && item.flags[3] == 'p')
      {
        valid = true;
        break;
      }
    }
    if (!valid)
    {
      throw std::runtime_error("Snapshot private region missing during restore");
    }

    // Check data bounds
    if (offset + size > data.size())
    {
      LOG_WARN("Region %lx-%lx data out of bounds", start, end);
      continue;
    }

    uint64_t readable_end = start;
    bool original_permissions = true;
    for (const auto& live : current_maps) {
      if (live.end <= readable_end || live.start >= end) continue;
      if (live.start > readable_end || live.flags[0] != 'r') break;
      original_permissions &= mapping_protection(live.flags) == int(regions[i].prot);
      readable_end = std::min(end, live.end);
      if (readable_end == end) break;
    }
    if (readable_end == end) {
      comparison.resize(size);
      iovec local{comparison.data(), size_t(size)}, remote{reinterpret_cast<void*>(start), size_t(size)};
      checked_memory_io(false, pid, &local, &remote, 1);
      if (!memcmp(comparison.data(), data.data() + offset, size)) {
        if (!original_permissions) restore_protection(pid, start, end, regions[i].prot);
        continue;
      }
    }

    uint64_t cursor = start;
    bool already_writable = (regions[i].prot & PROT_WRITE) != 0;
    for (const auto& live : current_maps) {
      if (live.end <= cursor || live.start >= end) continue;
      if (live.start > cursor || mapping_protection(live.flags) != int(regions[i].prot)) {
        already_writable = false;
        break;
      }
      cursor = std::min(end, live.end);
      if (cursor == end) break;
    }
    if (!already_writable || cursor != end) {
      restore_protection(pid, start, end, regions[i].prot | PROT_WRITE);
      protected_regions.push_back(i);
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
      checked_memory_io(true, pid, local_iov, remote_iov, iov_count);
      iov_count = 0;
    }
  }

  // Flush remaining regions
  if (iov_count > 0)
  {
    checked_memory_io(true, pid, local_iov, remote_iov, iov_count);
  }

  /* Restore the preserved rseq structure */
  checked_memory_io(true, pid, &rseq_local, &rseq_remote, 1);

  // Restore every saved permission, including pages that were writable in S
  // but read-only in the branch being discarded.
  for (uint32_t i : protected_regions)
    restore_protection(pid, regions[i].start, regions[i].end, regions[i].prot);

  // Read registers from data
  if (header->regs_offset + sizeof(struct user_regs_struct) <= data.size())
  {
    struct user_regs_struct regs;
    memcpy(&regs, data.data() + header->regs_offset, sizeof(regs));
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1)
      throw std::runtime_error("Cannot restore main thread registers");
    LOG_TRACE("Restored registers for hash %016lx", ts_hash);
  }
  else
  {
    LOG_ERROR("Invalid register offset in state data for hash %016lx", ts_hash);
  }
}

/* Main entry: recover running state for a process */
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

    if (physical_tid > 0) {
      // Restore registers for this thread
      if (ptrace(PTRACE_SETREGS, physical_tid, NULL, (void*)&target_ts.regs) == 0) {
        restore_xstate(physical_tid, target_ts.xstate);
        LOG_TRACE("Restored registers for virtual thread %d (physical %d)",
                  target_ts.tid, physical_tid);
      } else {
        throw std::runtime_error("Cannot restore thread registers");
      }
    } else {
      throw std::runtime_error("Cannot find physical thread for register restore");
    }
  }
}

void tracee_state::recover_running_state(int index, hash_type ts_hash) const
{
  pid_t main_pid = ptmc_state.pids[index];
  // Lifecycle restoration needs a real virtual-to-physical remapping contract.
  // Refuse changed thread sets before touching memory instead of guessing by position.
  const auto current_threads = get_thread_list(main_pid);
  std::set<pid_t> live(current_threads.begin(), current_threads.end());
  std::set<pid_t> saved;
  for (const auto& thread : threads)
    if (thread.physical_tid) saved.insert(thread.physical_tid);
  if (live != saved)
    throw std::runtime_error("Unsupported snapshot restore across thread creation or exit");
  /* Restore the kernel heap extent before reconciling VMAs. */
  if (tracee_do_syscall(main_pid, SYS_brk, brk, 0, 0, 0, 0, 0) != brk)
    throw std::runtime_error("Cannot restore snapshot brk");
  std::vector<maps_item> maps;
  restore_memory_mappings(ts_hash, main_pid, maps);
  recover_mem_reg_snapshot(maps, ts_hash, main_pid);

  restore_thread_registers(main_pid);

  restore_runtime_state(index);
}

void tracee_state::restore_runtime_state(int index) const
{
  auto& runtime = ptmc_state.running_state.child[index];
  runtime.threads = threads;
  runtime.thread_create_records = thread_create_records;
  runtime.main_tid = main_tid;
  runtime.current_thread_idx = current_thread_idx;
  memmove(runtime.si, si, sizeof(si));
  runtime.futex_state = futex_state;
  for (int t = 0; t < MAX_THREADS_PER_PROCESS; ++t) {
    ptmc_state.thread_blocked[index][t] =
        t < static_cast<int>(threads.size()) && threads[t].physical_tid &&
        futex_state && futex_state->is_thread_waiting(threads[t].tid);
    ptmc_state.thread_exited[index][t] =
        t < static_cast<int>(threads.size()) && !threads[t].physical_tid;
  }

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
    throw std::runtime_error("Cannot open tracee memory mappings for capture");
  }

  std::vector<maps_item> parsed_maps;
  get_maps_item(parsed_maps, maps_fp.get());
  if (ferror(maps_fp.get()) || parsed_maps.empty())
    throw std::runtime_error("Failed to read tracee memory mappings");

  // Kernel VMA partitions can change after mprotect without changing memory
  // semantics. Store one canonical partition for adjacent equivalent ranges.
  std::vector<maps_item> maps;
  for (const auto& item : parsed_maps) {
    bool merge = false;
    if (!maps.empty()) {
      const auto& previous = maps.back();
      merge = previous.end == item.start && !excluded_mapping(previous) &&
              !excluded_mapping(item) && !strcmp(previous.flags, item.flags) &&
              !strcmp(previous.name, item.name) && same_backing(previous, item, item.start);
    }
    if (merge) maps.back().end = item.end;
    else maps.push_back(item);
  }

  struct RegionToCapture { uint64_t start, end; size_t size; };
  std::vector<RegionToCapture> regions_to_capture;
  std::vector<RegionInfo> regions;
  size_t regions_data_size = 0;
  std::string maps_text;
  for (const auto& item : maps) {
    maps_text += fmt::format("{:x}-{:x} {} {:x} {:x}:{:x} {} {}\n", item.start,
                            item.end, item.flags, item.offset, item.a, item.b,
                            item.inode, item.name);
    if (excluded_mapping(item)) continue;
    if (item.flags[3] != 'p' || item.flags[0] != 'r')
      throw std::runtime_error(fmt::format("Unsupported snapshot mapping {:x}-{:x} {} {}: requires private readable memory",
                                         item.start, item.end, item.flags, item.name));
    RegionInfo info{};
    info.start = item.start; info.end = item.end;
    info.prot = mapping_protection(item.flags);
    memcpy(info.flags, item.flags, 5);
    info.offset = regions_data_size;
    regions.push_back(info);
    regions_to_capture.push_back({item.start, item.end, size_t(item.end - item.start)});
    regions_data_size += item.end - item.start;
  }
  std::vector<uint8_t> maps_data(maps_text.begin(), maps_text.end());

  // Keep virtual slots stable, including exited slots: futex queues refer to them.
  const auto thread_list = get_thread_list(pid);
  if (thread_list.empty())
    throw std::runtime_error("Cannot capture an empty thread register set");
  if (threads.empty()) {
    for (pid_t physical_tid : thread_list)
      add_thread(physical_tid, 0, 0, 0, physical_tid == pid);
  }
  if (threads.size() > MAX_THREADS_PER_PROCESS)
    throw std::runtime_error("Snapshot exceeds supported thread count");
  std::set<pid_t> expected;
  for (size_t i = 0; i < threads.size(); ++i) {
    auto& thread = threads[i];
    if (thread.tid != static_cast<pid_t>(i + 1))
      throw std::runtime_error("Invalid virtual thread slot in snapshot");
    if (!thread.physical_tid) continue;
    if (!expected.insert(thread.physical_tid).second)
      throw std::runtime_error("Duplicate physical thread in snapshot");
  }
  if (expected != std::set<pid_t>(thread_list.begin(), thread_list.end()))
    throw std::runtime_error("Cannot capture untracked thread creation or exit");
  for (auto& thread : threads) {
    if (!thread.physical_tid) continue;
    if (ptrace(PTRACE_GETREGS, thread.physical_tid, nullptr, &thread.regs) == -1)
      throw std::runtime_error("Cannot capture thread registers");
    thread.xstate = capture_xstate(thread.physical_tid);
  }
  if (current_thread_idx < 0 || current_thread_idx >= static_cast<int>(threads.size()))
    throw std::runtime_error("Invalid selected thread in snapshot");

  LOG_INFO("Saved state for %zu threads in process %d", threads.size(), pid);

  // Get main thread registers (for backward compatibility)
  struct user_regs_struct regs;
  if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == -1)
    throw std::runtime_error("Cannot capture main thread registers");

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
  std::vector<uint8_t> buffer(total_size);
  uint8_t *all_data = buffer.data();

  StateDataHeader *header = (StateDataHeader *)all_data;
  header->magic = 0x4453544D;
  header->version = 5;
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

    checked_memory_io(false, pid, local_iov, remote_iov, batch_count);

    for (size_t i = 0; i < batch_count; i++)
    {
      size_t idx = batch_start + i;
      auto &src = regions_to_capture[idx];
      if ((uintptr_t)rseq_struct[pid] >= src.start &&
          (uintptr_t)rseq_struct[pid] < src.end)
      {
        uintptr_t rseq_offset = (uintptr_t)rseq_struct[pid] - src.start;
        uint8_t *dest = all_data + region_infos[idx].offset;
        if (rseq_len[pid] < 0 || size_t(rseq_len[pid]) > src.size - rseq_offset)
          throw std::runtime_error("rseq extends beyond captured memory region");
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
  : threads(), thread_create_records(), main_tid(0), current_thread_idx(0), futex_state()
{
  const auto& runtime = ptmc_state.running_state.child[which];
  threads = runtime.threads;
  thread_create_records = runtime.thread_create_records;
  main_tid = runtime.main_tid;
  current_thread_idx = ptmc_state.current_thread_idx[which];
  futex_state = runtime.futex_state;

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
void *read_mem(int /*pid*/, hash_type ts_hash, uint64_t addr, long size)
{
  if (size < 0 || uint64_t(size) > UINT64_MAX - addr)
    throw std::runtime_error("Invalid snapshot memory read range");
  auto saved = StateStore::instance().load_shared(ts_hash);
  if (!saved)
    throw std::runtime_error("Cannot read unavailable snapshot");
  validate_state_header(*saved);
  const auto* header = reinterpret_cast<const StateDataHeader*>(saved->data());
  const auto* regions = reinterpret_cast<const RegionInfo*>(saved->data() + sizeof(StateDataHeader));
  auto result = std::unique_ptr<bytes, decltype(&free)>(static_cast<bytes*>(malloc(size_t(size) + 1)), &free);
  if (!result) throw std::bad_alloc();
  size_t copied = 0;
  for (uint32_t i = 0; i < header->num_regions && copied < size_t(size); ++i) {
    const uint64_t current = addr + copied;
    if (regions[i].start <= current && current < regions[i].end) {
      const size_t length = std::min(uint64_t(size - copied), regions[i].end - current);
      memcpy(result.get() + copied, saved->data() + regions[i].offset + current - regions[i].start, length);
      copied += length;
    }
  }
  if (copied != size_t(size))
    throw std::runtime_error("Requested memory range is absent from snapshot");
  result.get()[size] = 0;
  return result.release();
}

/* ======================================================================
 * Section 10: Helper Functions
 * ====================================================================== */

void get_maps_item(std::vector<maps_item> &items, FILE *maps)
{
  char line[1024];
  while (fgets(line, sizeof(line), maps) != NULL)
  {
    maps_item item{};
    /* Use width-limited format specifiers to prevent buffer overflow */
    if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %511s", &item.start, &item.end,
               item.flags, &item.offset, &item.a, &item.b, &item.inode,
               item.name) >= 7)
    {
      if (item.end <= item.start)
        throw std::runtime_error("Invalid memory mapping extent");
      items.emplace_back(item);
    }
    else
      throw std::runtime_error("Invalid memory mapping text");
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
    ptmc_state.status[i] = status[i];
    if (ts_hash[i] == ptmc_state.running_state.ts_hash[i]) {
      // The register/memory image is unchanged, but runtime runnability and
      // selection can have changed independently of its stored hash.
      child[i].restore_runtime_state(i);
      continue;
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
