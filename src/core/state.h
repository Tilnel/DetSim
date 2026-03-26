/*
 * state.h - State management
 */

#ifndef __STATE_H
#define __STATE_H

#include "fsstate.h"
#include "sockstate.h"
#include <sys/user.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <optional>

/* ======================================================================
 * State Data Format (shared with StateStore)
 * ====================================================================== */

/* Packed to avoid alignment padding */
struct __attribute__((packed)) StateDataHeader
{
  uint32_t magic;         // 'DSTM' = 0x4453544D
  uint32_t version;       // 3 (updated for integrated mappings)
  uint32_t num_regions;   // number of memory regions
  uint64_t tstate_offset; // offset to serialized tracee_state
  uint64_t tstate_size;   // size of serialized tracee_state
  uint64_t maps_offset;   // offset to mappings data (text format)
  uint64_t maps_size;     // size of mappings data
  uint64_t regs_offset;   // offset to register data
};
static_assert(sizeof(StateDataHeader) == 52, "StateDataHeader size must be 52 bytes");

/* Packed to avoid alignment padding */
struct __attribute__((packed)) RegionInfo
{
  uint64_t start;
  uint64_t end;
  uint64_t offset; // offset in data buffer
  uint32_t prot;   // protection flags (PROT_READ/WRITE/EXEC)
  char flags[5];   // original flags string from /proc/pid/maps (e.g., "rwxp")
};
static_assert(sizeof(RegionInfo) == 33, "RegionInfo size must be 33 bytes");

/* ======================================================================
 * Syscall Info Structure
 * ====================================================================== */

struct syscall_info
{
  int nr;
  /* only modified parameters will have value */
  uintptr_t rval;
  uintptr_t args[6];

  template <class Archive>
  void serialize(Archive &ar);
};

/* ======================================================================
 * Tracee State (Single Process State)
 * ====================================================================== */

/* Raft-specific state stored per tracee for safety checking */
struct raft_check_state
{
  // Equality comparison for incremental save optimization
  long current_term = 0;
  int is_leader = 0;
  long last_log_term = 0;

  /* Extended state for comprehensive bug detection */
  long commit_idx = 0;
  long applied_idx = 0;
  long last_idx = 0;

  /* Vote integrity checking */
  long voted_for = -1;

  /* Leader state tracking */
  long match_idx[3] = {0, 0, 0};
  long next_idx[3] = {0, 0, 0};

  template <class Archive>
  void serialize(Archive &ar)
  {
    ar(current_term, is_leader, last_log_term, commit_idx, applied_idx,
       last_idx, voted_for, match_idx[0], match_idx[1], match_idx[2],
       next_idx[0], next_idx[1], next_idx[2]);
  }
};

typedef struct tracee_state
{
  /* about syscall_info: here the syscall indicates the last DONE syscall */
  syscall_info si;

  uintptr_t brk;
  struct timeval tv;
  FileSystemState fs_state;
  SockState sock_state;

  /* Raft consensus checking state - stored per state for BFS correctness */
  raft_check_state raft_state;

  /* FdManager state - saved to preserve fd allocation state */
  FdManager fd_manager_state;

  /* ---------------------------------------------------------
   * Constructors
   * --------------------------------------------------------- */

  /* From running process */
  tracee_state(int which, const struct syscall_info &info);

  /* From saved state (hash) */
  tracee_state(hash_type hash);

  // Default constructor - value-initialize all members
  tracee_state()
      : si{}, brk(0), tv{0, 0}, fs_state(), sock_state(),
        raft_state()
  {
  }

  ~tracee_state() = default;

  // Copy control
  tracee_state(const tracee_state &other) = default;
  tracee_state &operator=(const tracee_state &other) = default;

  // Move control - explicitly default
  tracee_state(tracee_state &&other) noexcept = default;
  tracee_state &operator=(tracee_state &&other) noexcept = default;

  // Explicit memory cleanup - call after copying to ptmc_state to free source
  void clear();

  // Equality comparison (for incremental save optimization)
  // Compares all metadata fields to detect state changes
  bool metadata_equal(const tracee_state &other) const;

  /* ---------------------------------------------------------
   * State Capture (running -> struct -> disk)
   * --------------------------------------------------------- */

  /* Full state save: memory, mappings, serialization */
  hash_type save(int pid);

  /* Capture memory and registers */
  hash_type save_full_state_to_state_store(int pid);

  /* ---------------------------------------------------------
   * State Recovery (disk -> struct -> running)
   * --------------------------------------------------------- */

  /* Main recovery entry */
  void recover_running_state(int index, hash_type ts_hash) const;

  /* Restore memory mappings for brk */
  void restore_memory_mappings(hash_type ts_hash, int pid, std::vector<maps_item> &maps_out) const;

  /* Recover memory and registers */
  void recover_mem_reg_snapshot(std::vector<maps_item> &maps, hash_type ts_hash, int pid) const;


  /* ---------------------------------------------------------
   * Serialization
   * --------------------------------------------------------- */
  template <class Archive>
  void serialize(Archive &ar);

  void show_syscall() const;

} tracee_state;

/* ======================================================================
 * System State (Global State for All Processes)
 * ====================================================================== */

typedef struct sys_state
{
  hash_type ss_hash;
  hash_type ts_hash[NP];
  tracee_state child[NP];
  int status[NP];
  int error_bound = 5;

  // Default constructor - value-initialize all members
  sys_state() : ss_hash(0), ts_hash{}, child{}, status{}, error_bound(4) {}

  // Copy control
  sys_state(const sys_state &other) = default;
  sys_state &operator=(const sys_state &other) = default;

  // Move control - explicitly default
  sys_state(sys_state &&other) noexcept = default;
  sys_state &operator=(sys_state &&other) noexcept = default;

  /* From running processes - save */
  sys_state(const struct syscall_info info[]);

  /* From saved state (hash) */
  sys_state(hash_type hash);

  ~sys_state(){};

  // Explicit memory cleanup - call after copying to ptmc_state to free source
  void clear();

  /* ---------------------------------------------------------
   * Persistence
   * --------------------------------------------------------- */

  int save_metadata() const;

  /* ---------------------------------------------------------
   * Recovery
   * --------------------------------------------------------- */

  void recover_running_state() const;

  /* ---------------------------------------------------------
   * Serialization
   * --------------------------------------------------------- */
  template <class Archive>
  void serialize(Archive &ar);

} sys_state;

/* ======================================================================
 * State Collections
 * ====================================================================== */

/* State tree: child -> (parent, which, choose) */
typedef std::unordered_map<hash_type, std::tuple<hash_type, int, int>> TSS;

/* State queue (BFS exploration) */
typedef std::deque<hash_type> LSS;

/* State set (deduplication) */
typedef std::unordered_set<hash_type> SSS;

/* ======================================================================
 * Queue Management
 * ====================================================================== */

void state_queue_append(sys_state &s);
std::optional<sys_state> state_queue_extract();

/* ======================================================================
 * Tree Management
 * ====================================================================== */

void state_tree_add(const sys_state &s, sys_state &t, int which, int choose);

/* ======================================================================
 * History Display
 * ====================================================================== */

void show_syscall_history();
void show_syscall(int pid, hash_type ts_hash);

/* ======================================================================
 * Memory Operations
 * ====================================================================== */

/* this would malloc a buffer for the memory. free after use */
void *read_mem(int pid, hash_type ts_hash, uint64_t addr, long size);

/* ======================================================================
 * Helper Functions
 * ====================================================================== */

void get_maps_item(std::vector<maps_item> &items, FILE *maps);
bool mapping_exists(maps_item &a, std::vector<maps_item> &array);

/* ======================================================================
 * Statistics
 * ====================================================================== */

void state_stats_print();

/* ======================================================================
 * Global Cleanup
 * ====================================================================== */

void cleanup_all();

#endif /* __STATE_H */
