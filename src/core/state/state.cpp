/*
 * state.cpp - State management
 */

#include "state.h"
#include "xstate.h"
#include "../engine/thread_control.h"
#include "../engine/thread_pool.h"
#include "../engine/pending_syscall.h"
#include <poll.h>
#include "soft_dirty_experiment.h"
#include "page_capture.h"
#include "snapshot_memory.h"
#include "page_restore_stats.h"
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

bool syscall_history_enabled = true;

// Scalar metadata only: observation must not load snapshots or dereference guest pointers.
void show_syscall_current()
{
  const auto& state = ptmc_state.running_state;
  const auto edge = state_tree.find(state.ss_hash);
  if (edge == state_tree.end()) {
    detsim::ui::ui_printf("Current state " HASH_FORMAT " (no predecessor).\n", state.ss_hash);
    return;
  }
  const auto [predecessor, which, choice, slot] = edge->second;
  detsim::ui::ui_printf(HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d", predecessor, state.ss_hash, which);
  if (choice >= 0) detsim::ui::ui_printf(" (choose %d)", choice);
  detsim::ui::ui_printf(": ");
  const int status = state.status[which];
  if (DISCRASHED(status) || DISSIGNALED(status)) {
    detsim::ui::ui_printf("<fatal signal %d>\n", DSTOPSIG(status));
    return;
  }
  const auto& child = state.child[which];
  if (slot < 0 || slot >= MAX_THREADS_PER_PROCESS || slot >= int(child.threads.size())) {
    detsim::ui::ui_printf("<no syscall metadata>\n");
    return;
  }
  const auto& thread = child.threads[slot];
  const auto& info = child.si[slot];
  const auto* pending = thread.pending_syscall ? &*thread.pending_syscall : nullptr;
  const int nr = pending ? pending->nr : info.nr;
  extern const char* syscalls[450];
  const char* name = nr >= 0 && nr < 450 && syscalls[nr] ? syscalls[nr] : "unknown";
  if (child.threads.size() > 1) detsim::ui::ui_printf("[TID:%d] ", thread.tid);
  detsim::ui::ui_printf("%s(", name);
  for (int i = 0; i < 6; ++i)
    detsim::ui::ui_printf("%s0x%lx", i ? ", " : "", (unsigned long)(pending ? pending->args[i] : info.args[i]));
  detsim::ui::ui_printf(")");
  if (pending) {
    const char* phase = pending->phase == PendingPhase::Waiting ? "waiting" :
                        pending->phase == PendingPhase::Retry ? "retry" : "ready";
    detsim::ui::ui_printf(" <pending:%s>\n", phase);
  } else if (nr == SYS_exit || nr == SYS_exit_group) {
    detsim::ui::ui_printf(" <nonreturning>\n");
  } else {
    detsim::ui::ui_printf(" = %ld\n", (long)info.rval);
  }
}

void show_syscall_history()
{
  if (!syscall_history_enabled) return;
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

/* A transition may name its restored source as a deduplication hint. We still
 * capture every byte: a stopped task can have externally modified memory.
 * Generic callers have no source hint and use the ordinary full-save path.
 */
sys_state::sys_state(const struct syscall_info info[][MAX_THREADS_PER_PROCESS],
                     const sys_state *restored_source, bool persist)
{
  const char *mode = getenv("DETSIM_CAPTURE_MODE");
  const bool force_full = mode && !strcmp(mode, "full");
  error_bound = ptmc_state.error_bound;
  const auto& context = restored_source ? *restored_source : ptmc_state.running_state;
  property_failed = context.property_failed;
  monitor_states = context.monitor_states;
  model_identity = context.model_identity;
  // The product snapshot is one stopped cut: check every node before the
  // first injected brk query or memory read in any node.
  for (int j = 0; j < NP; ++j) {
    child[j] = tracee_state(j, info[j]);
    child[j].prepare_capture(ptmc_state.pids[j]);
  }
  for (int j = 0; j < NP; ++j) {
    status[j] = ptmc_state.status[j];
    const hash_type comparison_hash = !force_full && restored_source &&
        j != ptmc_state.cursor && DISALIVE(status[j]) &&
        DISALIVE(restored_source->status[j]) ? restored_source->ts_hash[j] : 0;
    ts_hash[j] = child[j].save_stopped(ptmc_state.pids[j], comparison_hash);
    if (!ts_hash[j]) throw std::runtime_error("Failed to capture tracee state");
  }
  ss_hash = identity_hash();
  LOG_DEBUG("Create sstate " HASH_FORMAT, ss_hash);
  if (persist) save_metadata();
}

void sys_state::commit_metadata()
{
  ss_hash = identity_hash();
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
    const uint32_t domain = 0x44535335; // DSS5: ordered machine/monitor/model product state
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

void tracee_state::validate_io_state() const
{
  validate_pending_syscalls(*this);
  auto require = [](bool condition) {
    if (!condition) throw std::runtime_error("Invalid snapshot descriptor/resource graph");
  };
  const auto &manager = fd_manager_state;
  const auto &files = fs_state.open_files;
  const auto &sockets = sock_state.sockets();
  const auto &epolls = sock_state.epoll_instances();
  const auto &pipe_fds = fs_state.pipe_fds_;
  for (const auto& thread:threads) {
    if (!thread.pending_syscall || thread.pending_syscall->reason!=PendingReason::Io ||
        thread.pending_syscall->phase==PendingPhase::Ready) continue;
    const auto& call=*thread.pending_syscall;
    const int fd=static_cast<int>(call.args[0]);
    require(manager.is_allocated(fd));
    const auto kind=manager.get_fd_type(fd);
    if(call.nr==SYS_read || call.nr==SYS_write) require(kind==FdType::PIPE || kind==FdType::SOCKET);
    else if(call.nr==SYS_recvfrom || call.nr==SYS_accept || call.nr==SYS_accept4) require(kind==FdType::SOCKET);
    else require(kind==FdType::EPOLL);
  }
  for (int fd : manager.get_allocated_fds()) {
    const auto kind = manager.get_fd_type(fd);
    const bool file = files.contains(fd), socket = sockets.contains(fd);
    const bool epoll = epolls.contains(fd), pipe = pipe_fds.contains(fd);
    require(int(file) + int(socket) + int(epoll) + int(pipe) == 1);
    require((file && (kind == FdType::FILE || kind == FdType::STDIO)) ||
            (socket && kind == FdType::SOCKET) ||
            (epoll && kind == FdType::EPOLL) || (pipe && kind == FdType::PIPE));
  }
  for (const auto &[fd, file] : files) {
    const auto kind = manager.get_fd_type(fd);
    require(manager.is_allocated(fd) && (kind == FdType::FILE || kind == FdType::STDIO));
    require(file.offset >= 0 && fs_state.filesystem.contains(file.path));
  }
  std::map<int, std::pair<unsigned, unsigned>> pipe_ends;
  for (const auto &[fd, signed_id] : pipe_fds) {
    require(manager.get_fd_type(fd) == FdType::PIPE && signed_id != 0 && signed_id != INT_MIN);
    const int id = signed_id > 0 ? signed_id : -signed_id;
    require(fs_state.pipes_.contains(id));
    const bool read_end = signed_id > 0;
    require((manager.get_status_flags(fd) & O_ACCMODE) == (read_end ? O_RDONLY : O_WRONLY));
    auto &ends = pipe_ends[id];
    if (read_end) ++ends.first;
    else ++ends.second;
  }
  for (const auto &[id, pipe] : fs_state.pipes_) {
    require(pipe.buffer.size()<=FileSystemState::pipe_capacity);
    require(id > 0 && !(pipe.read_closed && pipe.write_closed));
    const auto ends = pipe_ends[id];
    require(ends.first == unsigned(!pipe.read_closed) && ends.second == unsigned(!pipe.write_closed));
  }
  auto same_address = [](const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
  };
  for (const auto &[fd, socket] : sockets) {
    require(manager.get_fd_type(fd) == FdType::SOCKET && socket.fd == fd &&
            socket.domain == AF_INET && (socket.type == SOCK_STREAM || socket.type == SOCK_DGRAM));
    require(socket.keep_idle > 0 && socket.keep_idle <= 32767 && socket.keep_interval > 0 &&
            socket.keep_interval <= 32767 && socket.keep_count > 0 && socket.keep_count <= 127);
    const auto &connections = sock_state.tcp_connections();
    require(connections.contains(fd) == socket.connected);
    require(!socket.connected || (socket.type == SOCK_STREAM && !socket.listening));
    require(!socket.listening || (socket.type == SOCK_STREAM && socket.bound && socket.backlog >= 0));
    if (!socket.listening) require(socket.pending_connections.empty() && socket.established_connections.empty());
    std::set<uint64_t> pending;
    for (uint64_t endpoint : socket.pending_connections) {
      require(pending.insert(endpoint).second && socket.established_connections.contains(endpoint));
      const auto &connection = socket.established_connections.at(endpoint);
      require(connection.local_fd >= 0 && connection.local_addr.sin_family == AF_INET &&
              connection.peer_addr.sin_family == AF_INET &&
              endpoint == ((uint64_t(connection.local_addr.sin_addr.s_addr) << 32) | connection.local_addr.sin_port));
    }
    require(pending.size() == socket.established_connections.size());
  }
  for (const auto &[fd, connection] : sock_state.tcp_connections()) {
    const auto it = sockets.find(fd);
    require(it != sockets.end() && it->second.type == SOCK_STREAM && it->second.connected);
    require(connection.local_fd == fd && same_address(connection.local_addr, it->second.local_addr) &&
            same_address(connection.peer_addr, it->second.peer_addr));
  }
  for (const auto &[fd, queue] : sock_state.udp_recv_buffers()) {
    (void)queue;
    const auto it = sockets.find(fd);
    require(it != sockets.end() && it->second.type == SOCK_DGRAM);
  }
  for (const auto &[fd, epoll] : epolls) {
    require(manager.get_fd_type(fd) == FdType::EPOLL && epoll.epoll_fd == fd);
    for (const auto &[watched, watch] : epoll.watched_fds) {
      const auto kind = manager.get_fd_type(watched);
      require(watched != fd && (kind == FdType::SOCKET || kind == FdType::PIPE) &&
              !(watch.events & ~(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP)));
    }
  }
}

/* Load tracee_state from integrated StateStore data */
tracee_state::tracee_state(hash_type hash)
  : threads(), thread_create_records(), main_tid(0), current_thread_idx(0), futex_state()
{
  if (!hash) return;
  const auto snapshot = SnapshotView::open(hash);
  const auto metadata = snapshot->metadata();
  std::istringstream encoded(std::string(reinterpret_cast<const char*>(metadata.data()), metadata.size()));
  try {
    cereal::BinaryInputArchive archive(encoded);
    archive(*this);
    if (encoded.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("Trailing tracee metadata bytes");
    validate_io_state();
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("Failed to deserialize tracee_state snapshot: ") + error.what());
  } catch (...) {
    throw std::runtime_error("Failed to deserialize tracee_state snapshot");
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

// Public low-level restore APIs may bypass the owned-carrier barrier. Their
// caller must keep all tasks stopped; never write memory with native rseq active.
static void require_no_process_rseq(pid_t pid)
{
  const auto threads = get_thread_list(pid);
  if (threads.empty()) throw std::runtime_error("Cannot inspect restore process threads");
  for (pid_t tid : threads) require_no_rseq(tid);
}

void tracee_state::restore_memory_mappings(
    hash_type ts_hash, int pid, std::vector<maps_item> &maps_out) const
{
  require_no_process_rseq(pid);
  const auto snapshot = SnapshotView::open(ts_hash);
  std::vector<maps_item> maps_saved;
  std::istringstream encoded(std::string(snapshot->maps_text()));
  std::string line;
  while (std::getline(encoded, line)) {
    maps_item item{};
    if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %511s",
               &item.start, &item.end, item.flags, &item.offset, &item.a, &item.b,
               &item.inode, item.name) < 7 || item.end <= item.start)
      throw std::runtime_error("Invalid saved memory mapping text");
    maps_saved.push_back(item);
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
  require_no_process_rseq(pid);
  const auto snapshot = SnapshotView::open(ts_hash);
  const auto& regions = snapshot->regions();

  constexpr size_t MAX_IOV_BATCH = 256;
  iovec local_iov[MAX_IOV_BATCH], remote_iov[MAX_IOV_BATCH];
  size_t iov_count = 0, queued_bytes = 0;
  // Loading another page can evict a previous page from the store's cache.
  // Own every queued buffer until the corresponding write completes.
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> owners;
  owners.reserve(MAX_IOV_BATCH);
  auto flush = [&] {
    if (!iov_count) return;
    checked_memory_io(true, pid, local_iov, remote_iov, iov_count);
    detsim::page_restore::write_bytes.fetch_add(queued_bytes, std::memory_order_relaxed);
    detsim::page_restore::pages_written.fetch_add(iov_count, std::memory_order_relaxed);
    iov_count = queued_bytes = 0;
    owners.clear();
  };

  std::vector<maps_item> current_maps;
  auto current_maps_file = fileutils::open_cfile(fmt::format("/proc/{}/maps", pid), "r");
  if (!current_maps_file) throw std::runtime_error("Cannot inspect restore memory permissions");
  get_maps_item(current_maps, current_maps_file.get());
  std::vector<size_t> protected_regions;
  std::vector<uint8_t> comparison;

  for (size_t i = 0; i < regions.size(); ++i) {
    const auto& region = regions[i];
    const uint64_t start = region.start, end = region.end;
    const size_t size = end - start;
    bool valid = false;
    for (const auto& item : maps) {
      if (item.start <= start && end <= item.end && item.flags[3] == 'p') {
        valid = true;
        break;
      }
    }
    if (!valid) throw std::runtime_error("Snapshot private region missing during restore");

    uint64_t readable_end = start;
    bool original_permissions = true;
    for (const auto& live : current_maps) {
      if (live.end <= readable_end || live.start >= end) continue;
      if (live.start > readable_end || live.flags[0] != 'r') break;
      original_permissions &= mapping_protection(live.flags) == int(region.prot);
      readable_end = std::min(end, live.end);
      if (readable_end == end) break;
    }
    const bool comparable = readable_end == end;
    if (comparable) {
      comparison.resize(size);
      iovec local{comparison.data(), size}, remote{reinterpret_cast<void*>(start), size};
      checked_memory_io(false, pid, &local, &remote, 1);
      detsim::page_restore::read_bytes.fetch_add(size, std::memory_order_relaxed);
    }

    bool made_writable = false;
    for (size_t page_index = 0; page_index < snapshot->page_count(i); ++page_index) {
      const auto page = snapshot->page(i, page_index);
      if (comparable && !memcmp(comparison.data() + (page.address - start), page.data, page.length))
        continue;
      if (!made_writable) {
        // Applying the saved writable protection also repairs a split RO branch.
        if (!original_permissions || !comparable || !(region.prot & PROT_WRITE)) {
          restore_protection(pid, start, end, region.prot | PROT_WRITE);
          protected_regions.push_back(i);
        }
        made_writable = true;
      }
      local_iov[iov_count] = {const_cast<uint8_t*>(page.data), page.length};
      remote_iov[iov_count] = {reinterpret_cast<void*>(page.address), page.length};
      owners.push_back(page.owner);
      ++iov_count;
      queued_bytes += page.length;
      if (iov_count == MAX_IOV_BATCH) flush();
    }
    if (comparable)
      detsim::page_restore::pages_compared.fetch_add(snapshot->page_count(i), std::memory_order_relaxed);
    // Contents may be unchanged while the discarded branch changed mprotect.
    if (!made_writable && !original_permissions)
      restore_protection(pid, start, end, region.prot);
  }
  flush();

  for (size_t i : protected_regions)
    restore_protection(pid, regions[i].start, regions[i].end, regions[i].prot);

  auto regs = snapshot->main_regs();
  if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1)
    throw std::runtime_error("Cannot restore main thread registers");
}

/* Main entry: recover running state for a process */
/* Helper: Restore registers for all threads during recovery */
void tracee_state::restore_thread_registers(pid_t /*main_pid*/) const
{
  // The enclosing restore has verified the entire physical set remains stopped.
  for (const auto& thread : threads) {
    if (!thread.alive) continue;
    if (ptrace(PTRACE_SETREGS, thread.physical_tid, nullptr, &thread.regs) == -1)
      throw std::runtime_error("Cannot restore thread registers");
    restore_xstate(thread.physical_tid, thread.xstate);
  }
}

namespace {
struct ThreadBindingSlot {
  pid_t tid;
  pid_t physical_tid;
  bool alive;
};
using ThreadBindingLayout = std::vector<ThreadBindingSlot>;
struct ProcessBindingOwner {
  int fd;
  pid_t pid;
  std::vector<std::weak_ptr<const ThreadBindingLayout>> layouts;
  ~ProcessBindingOwner() { close(fd); }
};
struct SnapshotThreadBinding {
  std::shared_ptr<ProcessBindingOwner> owner;
  std::shared_ptr<const ThreadBindingLayout> threads;
};
std::map<std::pair<int, hash_type>, SnapshotThreadBinding> snapshot_thread_bindings;
std::map<pid_t, std::weak_ptr<ProcessBindingOwner>> process_owners;

bool same_layout(const ThreadBindingLayout& layout, const tracee_state& state)
{
  if (layout.size() != state.threads.size()) return false;
  for (size_t i = 0; i < layout.size(); ++i)
    if (layout[i].tid != state.threads[i].tid ||
        layout[i].alive != state.threads[i].alive) return false;
  return true;
}

bool same_runtime_bindings(const tracee_state& left, const tracee_state& right)
{
  if (left.threads.size() != right.threads.size()) return false;
  for (size_t i = 0; i < left.threads.size(); ++i) {
    const auto& a = left.threads[i];
    const auto& b = right.threads[i];
    if (a.tid != b.tid || a.alive != b.alive ||
        (a.alive && a.physical_tid != b.physical_tid)) return false;
  }
  return true;
}

std::vector<pid_t> bound_threads(const tracee_state& state)
{
  std::vector<pid_t> tids;
  for (size_t slot = 0; slot < state.threads.size(); ++slot) {
    const auto& thread = state.threads[slot];
    if (thread.tid != static_cast<pid_t>(slot + 1))
      throw std::runtime_error("Invalid logical thread slot");
    if (thread.alive) {
      if (thread.physical_tid <= 0)
        throw std::runtime_error("Logical thread has no owned physical binding");
      tids.push_back(thread.physical_tid);
    }
  }
  return tids;
}

void remember_thread_bindings(pid_t pid, hash_type hash, const tracee_state& state)
{
  int node = -1;
  for (int i = 0; i < NP; ++i) if (ptmc_state.pids[i] == pid) node = i;
  // Low-level capture may produce inspectable fixtures; only registered nodes
  // receive authority to restore into the running model.
  if (node < 0) return;
  auto owner = process_owners[pid].lock();
  if (!owner) {
    int fd = syscall(SYS_pidfd_open, pid, 0);
    if (fd == -1) throw std::runtime_error("Cannot retain snapshot process incarnation");
    owner = std::make_shared<ProcessBindingOwner>();
    owner->fd = fd;
    owner->pid = pid;
    process_owners[pid] = owner;
  }
  pollfd descriptor{owner->fd, POLLIN, 0};
  if (poll(&descriptor, 1, 0) != 0)
    throw std::runtime_error("Snapshot capture owner has exited; reset process bindings first");
  const auto key = std::make_pair(node, hash);
  const auto existing = snapshot_thread_bindings.find(key);
  if (existing != snapshot_thread_bindings.end()) {
    if (existing->second.owner != owner)
      throw std::runtime_error("Snapshot identity belongs to another process incarnation");
    if (!same_layout(*existing->second.threads, state))
      throw std::runtime_error("Snapshot identity has conflicting thread bindings");
    return;
  }
  std::shared_ptr<const ThreadBindingLayout> layout;
  for (auto it = owner->layouts.begin(); it != owner->layouts.end();) {
    auto candidate = it->lock();
    if (!candidate) { it = owner->layouts.erase(it); continue; }
    if (same_layout(*candidate, state)) { layout = std::move(candidate); break; }
    ++it;
  }
  if (!layout) {
    auto created = std::make_shared<ThreadBindingLayout>();
    created->reserve(state.threads.size());
    for (const auto& thread : state.threads)
      created->push_back({thread.tid, thread.physical_tid, thread.alive});
    layout = std::move(created);
    owner->layouts.push_back(layout);
  }
  snapshot_thread_bindings.emplace(key, SnapshotThreadBinding{owner, std::move(layout)});
}
}

void clear_snapshot_thread_bindings()
{
  snapshot_thread_bindings.clear();
  process_owners.clear();
  clear_thread_pools();
}

tracee_state tracee_state::resolve_thread_bindings(int index, hash_type hash) const
{
  tracee_state resolved = *this;
  resolved.bind_threads_for_restore(index, hash);
  return resolved;
}

void tracee_state::bind_threads_for_restore(int index, hash_type hash)
{
  const auto found = snapshot_thread_bindings.find({index, hash});
  if (index < 0 || index >= NP || found == snapshot_thread_bindings.end())
    throw std::runtime_error("Snapshot has no local thread binding provenance");
  const auto& binding = found->second;
  pollfd descriptor{binding.owner->fd, POLLIN, 0};
  if (binding.owner->pid != ptmc_state.pids[index] || poll(&descriptor, 1, 0) != 0)
    throw std::runtime_error("Snapshot belongs to a different or dead process incarnation");
  const auto& layout = *binding.threads;
  if (threads.size() != layout.size())
    throw std::runtime_error("Unsupported snapshot restore across thread creation or exit");
  for (size_t i = 0; i < threads.size(); ++i) {
    if (threads[i].tid != layout[i].tid || threads[i].alive != layout[i].alive)
      throw std::runtime_error("Snapshot logical thread lifecycle differs from its capture");

  }
  for (auto& record : thread_create_records) record.physical_tid = 0;
  verify_thread_pool(index);
  bind_thread_carriers(index, *this);
  validate_io_state();
}

void tracee_state::recover_running_state(int index, hash_type ts_hash) const
{
  const auto resolved = resolve_thread_bindings(index, ts_hash);
  ptmc_state.tracee_image_dirty[index] = true;
  restore_thread_signal_masks(index, resolved);
  normalize_thread_pool_for_restore(index);
  resolved.recover_stopped(index, ts_hash);
  resolved.restore_runtime_state(index);
}

void tracee_state::recover_stopped(int index, hash_type ts_hash) const
{
  const pid_t main_pid = ptmc_state.pids[index];
  /* Restore the kernel heap extent before reconciling VMAs. */
  if (tracee_do_syscall(main_pid, SYS_brk, brk, 0, 0, 0, 0, 0) != brk)
    throw std::runtime_error("Cannot restore snapshot brk");
  std::vector<maps_item> maps;
  restore_memory_mappings(ts_hash, main_pid, maps);
  recover_mem_reg_snapshot(maps, ts_hash, main_pid);

  restore_thread_registers(main_pid);
}

void tracee_state::restore_runtime_state(int index) const
{
  validate_io_state();
  auto& runtime = ptmc_state.running_state.child[index];
  runtime.threads = threads;
  runtime.thread_create_records = thread_create_records;
  runtime.main_tid = main_tid;
  runtime.current_thread_idx = current_thread_idx;
  memmove(runtime.si, si, sizeof(si));
  runtime.futex_state = futex_state;
  restore_resources(index);
}

void tracee_state::restore_resources(int index) const
{
  for (int t = 0; t < MAX_THREADS_PER_PROCESS; ++t) {
    ptmc_state.thread_blocked[index][t] =
        t < static_cast<int>(threads.size()) && threads[t].alive &&
        threads[t].pending_syscall && threads[t].pending_syscall->phase == PendingPhase::Waiting;
    ptmc_state.thread_exited[index][t] =
        t < static_cast<int>(threads.size()) && !threads[t].alive;
  }

  // Restore current thread index
  ptmc_state.current_thread_idx[index] = current_thread_idx;

  /* Restore subsystems */
  ptmc_state.sock_states[index] = sock_state;
  ptmc_state.fs_states[index] = fs_state;
  ptmc_state.process_states[index] = process_state;

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
        std::tuple<hash_type, int, int, int>(s.ss_hash, which, choose,
                                           t.child[which].current_thread_idx);
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
hash_type tracee_state::save_full_state_to_state_store(int pid, hash_type comparison_hash)
{
  prepare_capture(pid);
  return capture_stopped(pid, comparison_hash);
}

hash_type tracee_state::capture_stopped(pid_t pid, hash_type comparison_hash)
{
  validate_io_state();
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
    const bool anonymous_guard = !strcmp(item.flags, "---p") &&
                                 !item.inode && !item.name[0];
    if (item.flags[3] != 'p' || (item.flags[0] != 'r' && !anonymous_guard))
      throw std::runtime_error(fmt::format("Unsupported snapshot mapping {:x}-{:x} {} {}: requires private readable or anonymous PROT_NONE memory",
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

  // prepare_capture preserved and verified the complete logical slot layout.
  for (auto& thread : threads) {
    if (!thread.alive) continue;
    if (ptrace(PTRACE_GETREGS, thread.physical_tid, nullptr, &thread.regs) == -1)
      throw std::runtime_error("Cannot capture thread registers");
    thread.xstate = capture_xstate(thread.physical_tid);
  }
  if (current_thread_idx < 0 || current_thread_idx >= static_cast<int>(threads.size()))
    throw std::runtime_error("Invalid selected thread in snapshot");

  LOG_INFO("Saved state for %zu threads in process %d", threads.size(), pid);

  // Get main thread registers (for backward compatibility)
  const auto main = std::find_if(threads.begin(), threads.end(),
      [](const thread_state& thread) { return thread.is_main; });
  if (main == threads.end()) throw std::runtime_error("Snapshot has no logical main thread");
  const auto regs = main->regs;

  // Serialize tracee_state AFTER threads have been populated
  std::vector<uint8_t> tstate_data;
  {
    std::ostringstream oss;
    {
      FileContentSnapshotScope file_content_scope;
      cereal::BinaryOutputArchive ar(oss);
      ar(*this);
    }
    std::string str = oss.str();
    tstate_data.assign(str.begin(), str.end());
  }

  if (page_capture_enabled())
  {
    const auto hash = save_paged_snapshot(pid, regions, tstate_data, maps_text, regs,
                              comparison_hash);
    remember_thread_bindings(pid, hash, *this);
    return hash;
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
  header->version = FLAT_SNAPSHOT_VERSION;
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

  auto* soft_dirty = detsim::experiment::configured_capture();
  if (soft_dirty) {
    for (const auto& region : regions)
      if (region.prot == PROT_NONE)
        throw std::runtime_error("Soft-dirty capture does not support PROT_NONE mappings");
    std::vector<detsim::experiment::Region> raw_regions;
    raw_regions.reserve(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
      region_infos[i] = regions[i];
      region_infos[i].offset = region_data_offset + regions[i].offset;
      raw_regions.push_back({uintptr_t(regions_to_capture[i].start),
                             regions_to_capture[i].size,
                             all_data + region_infos[i].offset});
    }
    // The helper keeps raw bytes and deliberately tracks restore writes.
    soft_dirty->capture(pid, raw_regions, maps_text);
  }

  detsim::GuardMemoryReader guard_memory(pid);
  constexpr int MAX_IOV_BATCH = 256;
  for (size_t batch_start = 0; batch_start < regions.size();
       batch_start += MAX_IOV_BATCH)
  {
    size_t batch_end = std::min(batch_start + MAX_IOV_BATCH, regions.size());
    size_t batch_count = batch_end - batch_start;

    struct iovec local_iov[MAX_IOV_BATCH];
    struct iovec remote_iov[MAX_IOV_BATCH];
    size_t readable_count = 0;

    for (size_t i = 0; i < batch_count; i++)
    {
      size_t idx = batch_start + i;
      auto &src = regions_to_capture[idx];
      // Include protection and flags too: every byte enters the state hash.
      region_infos[idx] = regions[idx];
      region_infos[idx].offset = region_data_offset + regions[idx].offset;

      uint8_t *dest = all_data + region_infos[idx].offset;
      if (regions[idx].prot == PROT_NONE) {
        if (!soft_dirty) guard_memory.read(src.start, dest, src.size);
      } else {
        local_iov[readable_count] = {dest, src.size};
        remote_iov[readable_count] = {reinterpret_cast<void*>(src.start), src.size};
        ++readable_count;
      }
    }

    if (!soft_dirty)
      checked_memory_io(false, pid, local_iov, remote_iov, readable_count);
  }

  memcpy(all_data + header->tstate_offset, tstate_data.data(), tstate_size);

  if (maps_size > 0)
  {
    memcpy(all_data + header->maps_offset, maps_data.data(), maps_size);
    LOG_TRACE("Copied maps data: offset=%lu, size=%zu", header->maps_offset,
              maps_size);
  }

  memcpy(all_data + header->regs_offset, &regs, sizeof(regs));

  // The store may avoid hashing only after equality of all captured bytes.
  // It preserves write validation and never loads a cold comparison candidate.
  hash_type ts_hash = StateStore::instance().save(all_data, total_size, comparison_hash);
  const char *mode = getenv("DETSIM_CAPTURE_MODE");
  if (comparison_hash && mode && !strcmp(mode, "verify") &&
      StateStore::instance().save(all_data, total_size) != ts_hash)
    throw std::runtime_error("Capture deduplication disagrees with full save");


  LOG_DEBUG("StateStore saved hash %016lx, size=%zu, regions=%zu, tstate=%zu, "
            "maps=%zu",
            ts_hash, total_size, regions.size(), tstate_size, maps_size);
  remember_thread_bindings(pid, ts_hash, *this);
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
  process_state = ptmc_state.process_states[which];

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
void tracee_state::prepare_capture(pid_t pid)
{
  if (threads.empty()) {
    for (pid_t tid : get_thread_list(pid)) {
      add_thread(tid, 0, 0, 0, tid == pid);
      threads.back().signal_mask = read_thread_signal_mask(tid);
    }
  }
  if (threads.size() > MAX_THREADS_PER_PROCESS)
    throw std::runtime_error("Snapshot exceeds supported thread count");
  int node = -1;
  for (int i = 0; i < NP; ++i) if (ptmc_state.pids[i] == pid) node = i;
  if (node >= 0) prepare_thread_pool(node, *this);
  else {
    const auto expected = bound_threads(*this);
    verify_stopped_thread_set(pid, expected);
    for (pid_t tid : expected) require_thread_capture_boundary(tid);
  }
}

hash_type tracee_state::save(int pid, hash_type comparison_hash)
{
  prepare_capture(pid);
  return save_stopped(pid, comparison_hash);
}

hash_type tracee_state::save_stopped(pid_t pid, hash_type comparison_hash)
{
  /* Record brk */
  brk = tracee_do_syscall(pid, SYS_brk, 0, 0, 0, 0, 0, 0);
  LOG_DEBUG("recorded brk = 0x%x", brk);

  /* Capture memory state (includes tstate) */
  return capture_stopped(pid, comparison_hash);
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
  ts.physical_tid = physical_tid;  // Transient runtime binding
  ts.alive = true;
  ts.clone_flags = clone_flags;
  ts.stack_addr = stack;
  const auto parent = std::find_if(threads.begin(), threads.end(),
      [ptid](const thread_state& thread) { return thread.physical_tid == ptid; });
  if (ptid && parent == threads.end())
    throw std::runtime_error("Thread creation parent has no owned logical binding");
  const pid_t logical_parent = !ptid || parent == threads.end() ? 0 : parent->tid;
  ts.ptid = logical_parent;
  ts.is_main = is_main;

  // Record creation info with both virtual and physical TID
  thread_create_info record;
  record.virtual_tid = virtual_tid;
  record.physical_tid = physical_tid;
  record.clone_flags = clone_flags;
  record.stack_addr = stack;
  record.ptid = logical_parent;
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
  // Retain the logical slot and record its lifecycle independently of binding.
  // We keep the entry in the vector to maintain stable virtual TID mapping
  for (auto& ts : threads) {
    if (ts.tid == tid) {
      LOG_INFO("Marked thread %d (physical=%d) as exited", tid, ts.physical_tid);
      ts.alive = false;
      ts.pending_syscall.reset();
      ts.physical_tid = 0;
      return;
    }
  }
}

thread_state* tracee_state::find_thread(pid_t tid)
{
  for (auto& ts : threads) {
    if (ts.tid == tid && ts.alive) {
      return &ts;
    }
  }
  return nullptr;
}

const thread_state* tracee_state::find_thread(pid_t tid) const
{
  for (const auto& ts : threads) {
    if (ts.tid == tid && ts.alive) {
      return &ts;
    }
  }
  return nullptr;
}

int tracee_state::find_thread_index(pid_t tid) const
{
  for (size_t i = 0; i < threads.size(); i++) {
    if (threads[i].tid == tid && threads[i].alive) {
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
  auto result = std::unique_ptr<bytes, decltype(&free)>(
      static_cast<bytes*>(malloc(size_t(size) + 1)), &free);
  if (!result) throw std::bad_alloc();
  SnapshotView::open(ts_hash)->read_into(addr, result.get(), size_t(size));
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
  if (!ptmc_state.model_identity.empty() && model_identity != ptmc_state.model_identity)
    throw std::runtime_error("Snapshot execution model differs from the configured model");
  // Resolve and verify every node before changing any live or modeled state.
  sys_state resolved = *this;
  for (int i = 0; i < NP; ++i)
    resolved.child[i].bind_threads_for_restore(i, ts_hash[i]);
  bool restore_image[NP];
  for (int i = 0; i < NP; ++i) {
    restore_image[i] = ptmc_state.tracee_image_dirty[i] || ts_hash[i] != ptmc_state.running_state.ts_hash[i];
    // A failed restore must not leave its partial writes labelled as the old
    // running snapshot. Retain dirty flags until the whole operation commits.
    ptmc_state.tracee_image_dirty[i] = true;
  }
  for (int i = 0; i < NP; ++i) {
    restore_thread_signal_masks(i, resolved.child[i]);
    if (restore_image[i]) normalize_thread_pool_for_restore(i);
  }
  ptmc_state.error_bound = error_bound;
  for (int i = 0; i < NP; ++i) {
    ptmc_state.status[i] = status[i];
    if (restore_image[i])
      resolved.child[i].recover_stopped(i, ts_hash[i]);
    else if (!same_runtime_bindings(resolved.child[i], ptmc_state.running_state.child[i]))
      resolved.child[i].restore_thread_registers(ptmc_state.pids[i]);
    resolved.child[i].restore_resources(i);
  }
  ptmc_state.running_state = std::move(resolved);
  for (int i = 0; i < NP; ++i) ptmc_state.tracee_image_dirty[i] = false;
}

/* ======================================================================
 * Section 13: Global Cleanup
 * ====================================================================== */

void cleanup_all()
{
  clear_snapshot_thread_bindings();
  for (int i = 0; i < NP; ++i) ptmc_state.tracee_image_dirty[i] = false;
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
    ptmc_state.process_states[i] = ProcessKernelState();
    ptmc_state.fd_managers[i].reset();
  }

  /* 4. Clear global containers */
  detsim::ui::ui_printf("[DEBUG] Clearing global containers...\n");
  state_tree.clear();
  state_set.clear();

  /* 6. Cleanup readline resources */
  detsim::ui::ui_printf("[DEBUG] Cleaning up readline...\n");
  cleanup_readline();

  /* 7. Cleanup config allocated memory */
  detsim::ui::ui_printf("[DEBUG] Cleaning up config...\n");
  cleanup_config();

  detsim::ui::ui_printf("[DEBUG] Global cleanup completed\n");
}
