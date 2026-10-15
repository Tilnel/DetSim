/*
 * monitor.cpp - Monitor/CLI implementation (simplified)
 *
 * This file now focuses on CLI command handling.
 * Configuration parsing has moved to config.cpp.
 */

#include "monitor.h"
#include "ui/cli/commands.h"

/* NCursesUI integration */
#include "ui/log_wrapper.h"
#include "ui/ncurses_ui.h"

#include "config.h"
#include "fsstate.h"
#include "guest.h"
#include "proc_status.h"
#include "scheduler.h"
#include "sockstate.h"
#include "state/state.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "utils.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <csignal>
#include <dirent.h>
#include <fmt/format.h>
#include <iomanip>
#include <map>
#include <readline/history.h>
#include <readline/readline.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

/* Defined in config.cpp */
extern int auto_mode;

/* Defined in expr.cpp */
long expr(const char *e, bool *success);
void expr_print(const char *e);

/* Defined in dwarf.cpp */
void dwarf_print_stack_trace(int pid);
bool dwarf_set_frame(int frame_id);
int dwarf_get_current_frame(void);
void dwarf_print_local_vars(int pid);

/* Defined in state.cpp */
extern int choose_many[450];

/* Forward declarations for command handlers */

static int execute_command(char *cmd_line);

static struct
{
  const char *name;
  const char *description;
  int (*handler)(char *);
} cmd_table[] = {
    {"help", "Display informations about all supported commands", cmd_help},
    {"bfs", "Perform breadth-first search from current state", cmd_bfs},
    {"dfs", "Perform depth-first search from current state", cmd_dfs},
    {"rand", "Perform random search from current state", cmd_rand},
    {"q", "Exit ptraceMC", cmd_q},
    {"si", "Step from current state, on focused process", cmd_si},
    {"sw", "Switch control focus on the n-th process", cmd_sw},
    {"thread", "Switch to thread N in current process (thread to list)", cmd_thread},
    {"load", "Load state of given StateHash", cmd_load},
    {"info", "Display current state history", cmd_info},
    {"batch", "Read command list from file", cmd_batch},
    {"p", "Calculate expression", cmd_p},
    {"x",
     "Examine memory: x/[count][format][size] <addr> (formats: x,d,u,o,t,c,s; "
     "sizes: b,h,w,g)",
     cmd_x},
    {"bt", "Print backtrace with arguments", cmd_bt},
    {"frame", "Select stack frame", cmd_frame},
    {"locals", "Show local variables in current frame", cmd_locals},
    {"ls", "List files in current node's filesystem", cmd_ls},
    {"stat", "Show stat of files matching pattern", cmd_stat},
    {"hexdump", "Hexdump of a file (c: continue, q: quit)", cmd_hexdump},
    {"diff", "Compare two system states by hash prefix", cmd_diff},
};

#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

static inline void welcome()
{
  detsim::ui::ui_printf("Build time: %s, %s\n", __TIME__, __DATE__);
  detsim::ui::ui_printf("Welcome to ptraceMC!\n");
  detsim::ui::ui_printf("For help, type \"help\"\n");
}

int sigint_received = 0;
static void ctrl_c(int _) { sigint_received = 1; }
static void handle_sigint() { signal(SIGINT, ctrl_c); }

void init_regex();
void init_monitor(int argc, char *argv[])
{
  /* Parse arguments. */
  parse_args(argc, argv);

  /* Open the log file. */
  init_log(log_file);

  init_regex();

  read_config(cfg_file);

  /* Initialize state store */
  StateStore::instance().init();

  handle_sigint();

  /* Display welcome message. */
  welcome();
}

/* We use the `readline' library to provide more flexibility to read from stdin.
 */
static char *line_read = NULL;

char *rl_gets()
{
  if (line_read)
  {
    free(line_read);
    line_read = NULL;
  }

  line_read = readline("(ptmc) ");

  if (line_read && *line_read)
  {
    add_history(line_read);
  }

  return line_read;
}

/* External declaration for NCursesUI */
extern "C" detsim::ui::NCursesUI *get_ncurses_ui();

static int execute_command(char *cmd_line)
{
  char lastbuf[256] = {0};
  strncpy(lastbuf, cmd_line, sizeof(lastbuf) - 1);
  lastbuf[sizeof(lastbuf) - 1] = '\0';

  char *str_end = cmd_line + strlen(cmd_line);
  /* extract the first token as the command */
  char *cmd = strtok(cmd_line, " ");
  if (cmd == NULL)
  {
    return 0; // Empty command
  }

  /* treat the remaining string as the arguments */
  char *args = cmd + strlen(cmd) + 1;
  if (args >= str_end)
    args = NULL;

  int i;
  for (i = 0; i < NR_CMD; i++)
  {
    if (strcmp(cmd, cmd_table[i].name) == 0)
    {
      return cmd_table[i].handler(args);
    }
  }

  detsim::ui::ui_printf("Unknown command '%s'\n", cmd);
  return 0;
}

/* External batch file from config */
extern char *batch_file;

void ui_mainloop()
{
  detsim::ui::ui_printf("[DEBUG] ui_mainloop started, auto_mode=%d\n",
                        is_auto_mode());

  /* Handle batch mode first */
  if (batch_file != NULL)
  {
    char *current_batch = batch_file;
    batch_file = NULL; // Clear to prevent recursive batch processing
    detsim::ui::ui_printf("[DEBUG] Running batch mode from file: %s\n",
                          current_batch);
    cmd_batch(current_batch);
    return;
  }

  detsim::ui::NCursesUI *ui = get_ncurses_ui();

  /* Handle auto mode, default to bfs */
  if (is_auto_mode())
  {
    detsim::ui::ui_printf("[DEBUG] Running cmd_bfs\n");
    cmd_bfs(NULL);
    detsim::ui::ui_printf("[DEBUG] cmd_bfs returned\n");
    return;
  }

  /* If UI is available, use it */
  if (ui)
  {
    /* Set up command callback */
    ui->set_on_command(
        [](const std::string &cmd_line)
        {
          // Create modifiable copy for strtok
          char *cmd_copy = strdup(cmd_line.c_str());
          int result = execute_command(cmd_copy);
          free(cmd_copy);
          if (result < 0)
          {
            // Command wants to exit
            // Note: In NCursesUI mode, we need a way to signal exit
            // For now, just handle 'q' command specially
          }
        });

    /* Set up selection callback */
    ui->set_on_selection([](const std::string &text) {});

    /* Run NCursesUI main loop */
    ui->run();
    return;
  }

  /* Fallback to readline mode */
  char lastbuf[256] = {0};
  char lastcmd[256] = {0};

  for (char *str; (str = rl_gets()) != NULL;)
  {
    if (str[0] != 0)
      strncpy(lastbuf, str, sizeof(lastbuf) - 1);
    lastbuf[sizeof(lastbuf) - 1] = '\0';

    char *str_end = str + strlen(str);
    /* extract the first token as the command */
    char *cmd = strtok(str, " ");
    if (cmd == NULL)
    {
      // Copy lastcmd to str buffer safely for strtok
      if (strlen(lastcmd) < 256)
      {
        strcpy(str, lastcmd);
        cmd = strtok(str, " ");
      }
      if (cmd == NULL)
      {
        lastbuf[0] = lastcmd[0] = '\0';
        continue;
      }
    }

    /* treat the remaining string as the arguments */
    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end)
      args = NULL;

    int i;
    for (i = 0; i < NR_CMD; i++)
    {
      if (strcmp(cmd, cmd_table[i].name) == 0)
      {
        if (cmd_table[i].handler(args) < 0)
          return;
        break;
      }
    }

    if (i == NR_CMD)
      detsim::ui::ui_printf("Unknown command '%s'\n", cmd);
    strncpy(lastcmd, lastbuf, sizeof(lastcmd) - 1);
    lastcmd[sizeof(lastcmd) - 1] = '\0';
  }
}

/* ======================================================================
 * PTMC_STATE Multi-threading Methods
 * ====================================================================== */

pid_t PTMC_STATE::get_current_tid(int tracee_idx) const
{
  if (tracee_idx < 0 || tracee_idx >= NP) return -1;

  const auto& threads = running_state.child[tracee_idx].threads;
  int tidx = current_thread_idx[tracee_idx];

  if (tidx >= 0 && tidx < (int)threads.size()) {
    // Use the stored physical_tid from thread_state
    pid_t physical_tid = threads[tidx].physical_tid;
    if (threads[tidx].alive && physical_tid > 0) {
      return physical_tid;
    }
  }

  // Bootstrap has no logical slots yet; loaded/unbound slots must never fall back.
  return threads.empty() ? pids[tracee_idx] : -1;
}

pid_t PTMC_STATE::get_thread_tid(int tracee_idx, int thread_idx) const
{
  if (tracee_idx < 0 || tracee_idx >= NP) return -1;

  const auto& threads = running_state.child[tracee_idx].threads;
  if (thread_idx < 0 || thread_idx >= (int)threads.size()) {
    return -1;
  }

  // Use the stored physical_tid from thread_state
  pid_t physical_tid = threads[thread_idx].physical_tid;
  if (threads[thread_idx].alive && physical_tid > 0) {
    return physical_tid;
  }

  return -1;
}

void PTMC_STATE::set_current_thread(int tracee_idx, int thread_idx)
{
  if (tracee_idx < 0 || tracee_idx >= NP) return;

  const auto& threads = running_state.child[tracee_idx].threads;
  if (thread_idx >= 0 && thread_idx < (int)threads.size()) {
    current_thread_idx[tracee_idx] = thread_idx;
  }
}
