/*
 * monitor.h - Monitor/CLI interface
 */

#ifndef __MONITOR_H
#define __MONITOR_H

#include "fd_manager.h"
#include "sockstate.h"
#include "state.h"
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

  int pids[NP] = {};

  struct tracee_info
  {
    char *executable = nullptr;
    int argc = 0;
    char *argv[5] = {};
  } tracee[NP];

  struct timeval time[NP] = {};
  std::string addrs[NP];

  std::unordered_set<std::string> assertions;
  std::vector<int (*)()> user_checks;

  /* Raft consensus checking state - stored per-process */
  raft_check_state raft_states[NP];

  int error_bound = 5;

  // Constructor to ensure proper initialization
  PTMC_STATE() = default;
};

int is_auto_mode();
void cleanup_readline();

extern PTMC_STATE ptmc_state;

#endif /* __MONITOR_H */
