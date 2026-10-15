/*
 * monitor.h - Monitor/CLI interface
 */

#ifndef __MONITOR_H
#define __MONITOR_H

#include "fs/fd_manager.h"
#include "net/sockstate.h"
#include "state/state.h"
#include "types.h"
#include <memory>
#include <string>
#include <unordered_set>

enum
{
  PTMC_PRELOAD,
  PTMC_LOADED,
  PTMC_STOP,
  PTMC_RUNNING,
  PTMC_END,
  PTMC_ABORT,
  PTMC_QUIT
};

struct PTMC_STATE
{
  enum
  {
    MODE_DFS,
    MODE_BFS,
    MODE_RAND
  } mode = MODE_BFS;
  int state = PTMC_PRELOAD;
  int cursor = -1;
  int n_choose = 0;
  int choose = -1;
  int batch_choice_preset =
      -1; /* Preset choice value from batch file (-1 = not set) */
  sys_state running_state = sys_state();
  hash_type toload = 0;

  int status[NP] = {};

  SockState sock_states[NP];
  std::shared_ptr<FdManager> fd_managers[NP]; /* Per-process fd allocation */
  FileSystemState fs_states[NP];
  ProcessKernelState process_states[NP];

  int pids[NP] = {};

  /* Multi-threading support */
  int current_thread_idx[NP] = {};  /* Currently selected thread index per tracee */
  bool thread_blocked[NP][64] = {}; /* Thread blocked status (waiting on futex) per tracee, max 64 threads */
  bool thread_exited[NP][64] = {};  /* Thread exit status per tracee, max 64 threads */

  /* Get current thread TID for a tracee */
  pid_t get_current_tid(int tracee_idx) const;
  void set_current_thread(int tracee_idx, int thread_idx);

  /* Get physical TID for a specific thread index in a tracee */
  pid_t get_thread_tid(int tracee_idx, int thread_idx) const;

  struct tracee_info
  {
    char *executable = nullptr;
    int argc = 0;
    char *argv[5] = {};
  } tracee[NP];

  struct timeval time[NP] = {};
  std::string addrs[NP];

  std::unordered_set<std::string> assertions;
  std::string model_identity; // Configured execution/inspection model.

  int error_bound = 5;

  // Constructor to ensure proper initialization
  PTMC_STATE() = default;
};

int is_auto_mode();
void cleanup_readline();

extern PTMC_STATE ptmc_state;

#endif /* __MONITOR_H */
