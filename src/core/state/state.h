/*
 * state.h - State management
 */

#ifndef __STATE_H
#define __STATE_H

#include "../engine/thread_manager.h"  // For MAX_THREADS_PER_PROCESS
#include "../fs/fsstate.h"
#include "../net/sockstate.h"
#include <sys/user.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include "futexstate.h"
#include "process/process_state.h"


/* ======================================================================
 * State Data Format (shared with StateStore)
 * ====================================================================== */

inline constexpr uint32_t FLAT_SNAPSHOT_VERSION = 11;
inline constexpr uint32_t PAGE_SNAPSHOT_VERSION = 12;

/* Packed to avoid alignment padding */
struct __attribute__((packed)) StateDataHeader
{
  uint32_t magic;         // 'DSTM' = 0x4453544D
  uint32_t version;       // 11: flat; 12: pages (includes modeled kernel process state)
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
  uint64_t offset; // flat: byte offset in blob; pages: first PageRef index
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
 * Thread State (Single Thread State for Multi-threading Support)
 * ====================================================================== */

/**
 * @brief Per-thread state for multi-threading support
 *
 * Each thread in a tracee process has a thread_state entry. The tid field
 * is a virtual TID (stable index + 1) that remains constant across state
 * saves/loads. The physical_tid is the actual kernel thread ID which may
 * change when states are restored.
 *
 * A thread is considered "exited" when physical_tid == 0. This allows us
 * to maintain stable TID mappings even after threads exit.
 */
struct thread_state
{
  pid_t tid;                      // Virtual TID (stable thread index + 1)
  pid_t physical_tid;             // Physical TID (actual kernel thread ID)
  struct user_regs_struct regs{}; // General-purpose register context
  std::vector<uint8_t> xstate;     // Complete Linux NT_X86_XSTATE register set
  uint64_t clone_flags;           // Clone flags used to create this thread
  uint64_t stack_addr;            // Stack address (for clone)
  pid_t ptid;                     // Parent thread TID
  bool is_main;                   // Whether this is the main thread

  thread_state() : tid(0), physical_tid(0), clone_flags(0), stack_addr(0), ptid(0), is_main(false)
  {
    // regs will be zero-initialized by the default member initializer
  }

  template <class Archive>
  void serialize(Archive &ar)
  {
    ar(tid, physical_tid, regs.r15, regs.r14, regs.r13, regs.r12, regs.rbp, regs.rbx,
       regs.r11, regs.r10, regs.r9, regs.r8, regs.rax, regs.rcx, regs.rdx,
       regs.rsi, regs.rdi, regs.orig_rax, regs.rip, regs.cs, regs.eflags,
       regs.rsp, regs.ss, regs.fs_base, regs.gs_base, regs.ds, regs.es,
       regs.fs, regs.gs, clone_flags, stack_addr, ptid, is_main, xstate);
  }
};

/* Thread creation record for tracking clone parameters */
struct thread_create_info
{
  pid_t virtual_tid;     // Virtual TID (stable thread index + 1)
  pid_t physical_tid;    // Physical TID (actual kernel thread ID)
  uint64_t clone_flags;
  uint64_t stack_addr;
  pid_t ptid;

  template <class Archive>
  void serialize(Archive &ar)
  {
    ar(virtual_tid, physical_tid, clone_flags, stack_addr, ptid);
  }
};

/* ======================================================================
 * Tracee State (Single Process State)
 * ====================================================================== */

/**
 * @brief Per-process state for a single tracee (traced process)
 *
 * tracee_state captures all state associated with a single traced process:
 * - Register context for all threads
 * - Memory mappings and heap state
 * - File system state (VFS)
 * - Socket/network state
 * - Thread management information
 *
 * This structure is serialized/deserialized for state save/load operations.
 * The threads vector maintains stable virtual TIDs even after threads exit.
 */
typedef struct tracee_state
{
  /* Last completed syscall information */
  syscall_info si[MAX_THREADS_PER_PROCESS];  // Per-thread syscall info

  uintptr_t brk;
  struct timeval tv;
  FileSystemState fs_state;
  SockState sock_state;
  ProcessKernelState process_state;

  /* FdManager state - saved to preserve fd allocation state */
  FdManager fd_manager_state;

  /* Multi-threading support */
  std::vector<thread_state> threads;        // All thread states
  std::vector<thread_create_info> thread_create_records;  // Thread creation history
  pid_t main_tid;                           // Main thread TID
  int current_thread_idx;                   // Currently selected thread index

  /* Futex state for thread synchronization */
  std::optional<FutexState> futex_state;   // Branch-owned queues, keyed by virtual TID

  /* ---------------------------------------------------------
   * Thread Management Methods
   * --------------------------------------------------------- */

  // Add a new thread to this tracee's state
  void add_thread(pid_t tid, uint64_t clone_flags, uint64_t stack, pid_t ptid, bool is_main = false);

  // Remove a thread from this tracee's state
  void remove_thread(pid_t tid);

  // Find a thread by TID
  thread_state* find_thread(pid_t tid);
  const thread_state* find_thread(pid_t tid) const;

  // Get the number of threads
  size_t thread_count() const { return threads.size(); }

  // Find thread index by TID
  int find_thread_index(pid_t tid) const;

  /* ---------------------------------------------------------
   * Constructors
   * --------------------------------------------------------- */

  /* From running process */
  tracee_state(int which, const struct syscall_info info[]);  // Per-thread syscall info

  /* From saved state (hash) */
  tracee_state(hash_type hash);

  // Default constructor - value-initialize all members
  tracee_state()
      : si{}, brk(0), tv{0, 0}, fs_state(), sock_state(),
        threads(), thread_create_records(),
        main_tid(0), current_thread_idx(0), futex_state()
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

  // Reject inconsistent captured descriptor/resource graphs before use.
  void validate_io_state() const;

  // Equality comparison (for incremental save optimization)
  // Compares all metadata fields to detect state changes
  bool metadata_equal(const tracee_state &other) const;

  /* ---------------------------------------------------------
   * State Capture (running -> struct -> disk)
   * --------------------------------------------------------- */

  /* Full state save: memory, mappings, serialization */
  hash_type save(int pid, hash_type comparison_hash = 0);

  /* Capture memory and registers */
  hash_type save_full_state_to_state_store(int pid, hash_type comparison_hash = 0);

  /* ---------------------------------------------------------
   * State Recovery (disk -> struct -> running)
   * --------------------------------------------------------- */

  /* Main recovery entry */
  void recover_running_state(int index, hash_type ts_hash) const;
  void restore_runtime_state(int index) const;

  /* Restore memory mappings for brk */
  void restore_memory_mappings(hash_type ts_hash, int pid, std::vector<maps_item> &maps_out) const;

  /* Recover memory and registers */
  void recover_mem_reg_snapshot(std::vector<maps_item> &maps, hash_type ts_hash, int pid) const;

  /* ---------------------------------------------------------
   * Thread Recovery Helpers
   * --------------------------------------------------------- */
private:
  void restore_thread_registers(pid_t main_pid) const;

public:
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

/**
 * @brief Global system state for all tracees
 *
 * sys_state represents a complete snapshot of the entire system being traced,
 * including all processes and their states. This is the primary unit of
 * state saving/loading during state space exploration.
 *
 * The ss_hash uniquely identifies this global state, while ts_hash[]
 * identifies the individual tracee states that compose it.
 */
typedef struct sys_state
{
  hash_type ss_hash;        // Ordered global semantic metadata identity
  hash_type ts_hash[NP];    // Per-process state hashes
  tracee_state child[NP];   // Per-process detailed state
  int status[NP];           // Per-process status (running/exited/etc)
  int error_bound = 5;      // Model budget; affects allowed successors.
  bool property_failed = false; // Absorbing property-monitor failure state.
  std::map<std::string, std::vector<uint8_t>> monitor_states;
  std::string model_identity; // Plugin/config/environment compatibility identity.

  // Default constructor - value-initialize all members
  sys_state() : ss_hash(0), ts_hash{}, child{}, status{}, error_bound(4) {}

  // Copy control
  sys_state(const sys_state &other) = default;
  sys_state &operator=(const sys_state &other) = default;

  // Move control - explicitly default
  sys_state(sys_state &&other) noexcept = default;
  sys_state &operator=(sys_state &&other) noexcept = default;

  /* From running processes - save */
  sys_state(const struct syscall_info info[][MAX_THREADS_PER_PROCESS],
            const sys_state *restored_source = nullptr, bool persist = true);  // Per-thread syscall info

  /* From saved state (hash) */
  sys_state(hash_type hash);

  ~sys_state(){};

  // Explicit memory cleanup - call after copying to ptmc_state to free source
  void clear();


  /* ---------------------------------------------------------
   * Persistence
   * --------------------------------------------------------- */

  int save_metadata() const;
  void commit_metadata();
  hash_type identity_hash() const;

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

/* ======================================================================
 * Global Cleanup
 * ====================================================================== */

void cleanup_all();

#endif /* __STATE_H */
