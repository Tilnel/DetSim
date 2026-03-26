#include "scheduler.h"
#include "debug.h"
#include "emu.h"
#include "expr_eval.hpp"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "proc_status.h"
#include "sockstate.h"
#include "state.h"
#include "state_store.h"
#include "state_store_packed.h"
#include "state_transition.h"
#include "sysstate_store.h"
#include "utils.h"

/* NCursesUI integration */
#include "ui/log_wrapper.h"
#include "ui/ncurses_ui.h"
#include <csignal>

// 外部声明：get_ncurses_ui 定义在 main.cpp
extern "C" detsim::ui::NCursesUI *get_ncurses_ui();

#include <assert.h>
#include <atomic>
#include <chrono>
#include <cmath> // for log, exp
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <random>
#include <readline/readline.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#define pids ptmc_state.pids
extern FILE *log_fp;
double start_time = 0.0;

/* Apply user choice modifications to syscall arguments */
void apply_choose(const syscall_info &info, choose_out *out)
{
  for (int i = 0; i < 6; i++)
  {
    if (out->len[i])
      memcpy_host2guest((void *)info.args[i], out->args[i], out->len[i]);
  }
}

void show_syscall_history();
/* For single step use. Should get hash from ptmc_state.sysstate_hash. *
 * So, after every run, include cmd_cont, should set *
 * ptmc_state.sysstate_hash as the ending state. */

void load(hash_type hash)
{
  if (hash == 0)
    hash = ptmc_state.running_state.ss_hash;
  sys_state s(hash);
  s.recover_running_state();
}

/* Forward declarations for status monitor */
static void stop_status_monitor();

long expr(const char *e, bool *success);
/* Judge state assertions. Return 0 if legal */
static int check_state()
{
  hash_type current_hash = ptmc_state.running_state.ss_hash;
  for (auto &assertion : ptmc_state.assertions)
  {
    bool success;
    long val = expr(assertion.c_str(), &success);
    LOG_DEBUG("Assertion \"%s\" = %ld (success=%d)", assertion.c_str(), val,
              success);
    if (!success)
      LOG_ERROR("Failed to evaluate expression \"%s\"", assertion.c_str());
    if (val == false)
    {
      detsim::ui::ui_printf("Assertion \"%s\" fail at sys_state " HASH_FORMAT
                            "!\n",
                            assertion.c_str(), current_hash);
      return 1;
    }
  }
  for (auto f : ptmc_state.user_checks)
    if (f())
      return 1;

  return 0;
}

int load_exec_store()
{
  int index = ptmc_state.cursor;
  sys_state s = ptmc_state.running_state;

  if (DISEXITED(s.status[index]))
  {
    detsim::ui::ui_printf(
        "Tracee #%d has exited with status %d. Please do something else.\n",
        index, DEXITSTATUS(s.status[index]));
    return 0;
  }
  else if (DISCRASHED(s.status[index]))
  {
    detsim::ui::ui_printf(
        "Tracee #%d stopped for signal %s. Please do something else.\n", index,
        strsignal(DSTOPSIG(s.status[index])));
    return 0;
  }

  TransitionResult result = state_transition(s, index);

  if (check_state() != 0)
    detsim::ui::ui_printf("Reach illegal state.\n");

  return 0;
}

extern LSS state_queue;
extern SSS state_set;
extern TSS state_tree;
extern int sigint_received;

#include <sys/time.h>
static double gettime()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* ======================================================================
 * Status Monitor - Real-time statistics display
 * ====================================================================== */

/* Global statistics for status monitor */
static std::atomic<size_t> g_traces_searched{0};
static std::atomic<size_t> g_states_searched{0};
static std::atomic<size_t> g_states_new{0};
static std::atomic<size_t> g_queue_size{0};
static std::atomic<double> g_start_time{0.0};
static std::atomic<bool> g_monitor_running{false};

/* Depth tracking for depth vs states analysis */
static std::atomic<size_t> g_max_depth_recorded{0}; // Maximum depth seen so far
static FILE *g_depth_stat_file = nullptr; // File for depth-statistics output
static std::mutex g_depth_stat_mutex;     // Mutex for file access

/* Data points for exponential fitting: (depth, total_states) */
static std::vector<std::pair<double, double>> g_depth_data_points;
static std::mutex g_depth_data_mutex;

/* Exponential fit parameters: states = a * exp(b * depth) */
static double g_fit_a = 0.0; // coefficient
static double g_fit_b = 0.0; // exponent
static std::mutex g_fit_mutex;

/* Perform exponential fit: y = a * exp(b * x)
 * Returns true if fit succeeds, false otherwise */
static bool
exponential_fit(const std::vector<std::pair<double, double>> &points,
                double &out_a, double &out_b)
{
  if (points.size() < 3)
    return false; // Need at least 3 points

  size_t n = points.size();
  double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;

  // Use log-linear fit: ln(y) = ln(a) + b*x
  for (const auto &p : points)
  {
    double x = p.first;
    double y_log = log(p.second);
    sum_x += x;
    sum_y += y_log;
    sum_xy += x * y_log;
    sum_x2 += x * x;
  }

  double denom = n * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-10)
    return false; // Avoid division by zero

  out_b = (n * sum_xy - sum_x * sum_y) / denom;
  double a_log = (sum_y - out_b * sum_x) / n;
  out_a = exp(a_log);

  return true;
}

/* Update fit with new data point */
static void update_depth_fit(size_t depth, size_t total_states)
{
  std::lock_guard<std::mutex> lock(g_depth_data_mutex);
  g_depth_data_points.push_back(
      {static_cast<double>(depth), static_cast<double>(total_states)});

  // Keep only last 50 points for local fitting
  if (g_depth_data_points.size() > 50)
  {
    g_depth_data_points.erase(g_depth_data_points.begin());
  }

  double a, b;
  if (exponential_fit(g_depth_data_points, a, b))
  {
    std::lock_guard<std::mutex> fit_lock(g_fit_mutex);
    g_fit_a = a;
    g_fit_b = b;
  }
}

/* Performance diagnostics */
static std::atomic<uint64_t> g_save_time_us{0};    // Total state save time
static std::atomic<uint64_t> g_restore_time_us{0}; // Total state restore time
static std::atomic<uint64_t> g_syscall_time_us{
    0};                                        // Total syscall execution time
static std::atomic<size_t> g_save_count{0};    // Number of saves
static std::atomic<size_t> g_restore_count{0}; // Number of restores
static std::chrono::high_resolution_clock::time_point g_last_stat_time;
static std::thread g_monitor_thread;

/* Calculate real depth from state_tree by backtracking to root
 * This is the actual path length from current state to initial state */
static size_t calculate_state_depth(hash_type current_hash)
{
  if (current_hash == 0)
    return 0;

  size_t depth = 0;
  hash_type hash = current_hash;

  /* Use a simple visited set to prevent infinite loops (shouldn't happen in
   * tree) */
  std::unordered_set<hash_type> visited;

  while (state_tree.count(hash) && visited.insert(hash).second)
  {
    auto &parent_info = state_tree[hash];
    hash = std::get<0>(parent_info);
    if (hash == 0)
      break; // Reached root
    depth++;
  }

  return depth;
}

/* Get terminal size */
static void get_terminal_size(int &rows, int &cols)
{
  struct winsize w;
  rows = 24;
  cols = 80; /* defaults */
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
  {
    rows = w.ws_row;
    cols = w.ws_col;
  }
}

/* Format bytes to human readable */
static std::string format_bytes(size_t bytes)
{
  const char *units[] = {"B", "KB", "MB", "GB"};
  int unit = 0;
  double size = bytes;
  while (size >= 1024 && unit < 3)
  {
    size /= 1024;
    unit++;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f%s", size, units[unit]);
  return std::string(buf);
}

/* ANSI escape codes */
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_CLEAR_LINE "\033[2K"
#define ANSI_MOVE_UP "\033[A"
#define ANSI_MOVE_DOWN "\033[B"
#define ANSI_SAVE_CURSOR "\033[s"
#define ANSI_RESTORE_CURSOR "\033[u"
#define ANSI_MOVE_TO_ROW(r) "\033[" #r ";1H"

/* Status monitor thread - updates display every second */
static void status_monitor_thread()
{
  /* Wait a bit for initial states to be processed */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  /* Get NCursesUI instance */
  detsim::ui::NCursesUI *ui = get_ncurses_ui();

  int rows, cols;
  get_terminal_size(rows, cols);

  /* Hide cursor (only if not using UI) */
  // Skip ANSI cursor hide in no-ui mode to avoid terminal issues
  // if (!ui)
  // {
  //   detsim::ui::ui_printf(ANSI_HIDE_CURSOR);
  //   fflush(stdout);
  // }

  while (g_monitor_running.load())
  {
    auto &store = StateStore::instance();
    auto stats = store.get_stats();

    size_t l1_usage = store.get_l1_usage();
    size_t l1_cap = store.get_l1_capacity();
    size_t l2_usage = store.get_l2_usage();
    size_t l2_cap = store.get_l2_capacity();
    size_t l1_entries = store.get_l1_entry_count();
    size_t l2_entries = store.get_l2_entry_count();
    size_t io_queue_size = store.get_io_queue_size();
    size_t io_queue_cap = store.get_io_queue_capacity();
    size_t prefetch_queue_size = store.get_prefetch_queue_size();

    double hit_rate = stats.hit_rate();
    double elapsed = gettime() - g_start_time.load();
    double states_per_sec =
        elapsed > 0 ? g_states_searched.load() / elapsed : 0;

    size_t total_sysstates = state_set.size();
    size_t unique_states = g_states_new.load();
    size_t queue_size = g_queue_size.load();
    size_t traces_searched = g_traces_searched.load();
    /* Calculate real depth from state tree */
    size_t depth = calculate_state_depth(ptmc_state.running_state.ss_hash);

    /* NEW: Track depth vs states relationship - log when we see a new max depth
     */
    size_t current_max_depth = g_max_depth_recorded.load();
    size_t total_searched = g_states_searched.load();
    if (depth > current_max_depth && total_searched > 0)
    {
      // Update max depth (may fail if another thread updated, that's OK)
      if (g_max_depth_recorded.compare_exchange_weak(current_max_depth, depth))
      {
        std::lock_guard<std::mutex> lock(g_depth_stat_mutex);
        if (g_depth_stat_file)
        {
          fprintf(g_depth_stat_file, "%zu %zu\n", depth, total_searched);
          fflush(g_depth_stat_file);
        }
        // Update exponential fit with new data point
        update_depth_fit(depth, total_searched);
      }
    }

    /* NEW: Track global container sizes (potential memory hogs) */
    size_t state_tree_size = state_tree.size();
    size_t state_queue_internal = StateStore::instance().queue_size();
    size_t sock_state_total = 0;
    size_t fs_state_total = 0;
    for (int i = 0; i < NP; i++)
    {
      sock_state_total += ptmc_state.sock_states[i].sockets().size();
      sock_state_total += ptmc_state.sock_states[i].udp_recv_buffers().size();
    }

    /* Update status display */
    if (ui)
    {
      /* Use NCursesUI status window - compact layout */
      char buf[256];

      double l1_pct = l1_cap > 0 ? 100.0 * l1_usage / l1_cap : 0;
      double l2_pct = l2_cap > 0 ? 100.0 * l2_usage / l2_cap : 0;
      int hours = (int)(elapsed / 3600);
      int minutes = ((int)elapsed % 3600) / 60;
      int seconds = (int)elapsed % 60;

      /* Line 1: Progress stats */
      snprintf(buf, sizeof(buf),
               "Searched: %zu | Unique: %zu | Depth: %zu | Queued: %zu | "
               "%.1f/s | %02d:%02d:%02d",
               g_states_searched.load(), unique_states, depth, queue_size,
               states_per_sec, hours, minutes, seconds);
      ui->set_status_line(1, buf);

      /* Line 2: Worker queues & prefetch stats */
      snprintf(
          buf, sizeof(buf),
          "IO: %zu/%zu | Prefetch: %zu Q(%lu/%lu H) | Tree: %zu | Sys: %zu",
          io_queue_size, io_queue_cap, prefetch_queue_size,
          stats.prefetch_issued, stats.prefetch_hits, state_tree_size,
          total_sysstates);
      ui->set_status_line(2, buf);

      /* Line 3: Cache L1 & L2 with hit rate */
      snprintf(
          buf, sizeof(buf),
          "L1: %s/%s (%.0f%%) [%zu] | L2: %s/%s (%.0f%%) [%zu] | Hit: %.1f%%",
          format_bytes(l1_usage).c_str(), format_bytes(l1_cap).c_str(), l1_pct,
          l1_entries, format_bytes(l2_usage).c_str(),
          format_bytes(l2_cap).c_str(), l2_pct, l2_entries, hit_rate);
      ui->set_status_line(3, buf);

      /* Line 4: Eviction & subsystem stats */
      snprintf(buf, sizeof(buf),
               "Evict: %lu (%s) | Sock: %zu | FS: %zu | SQueue: %zu",
               stats.evict_calls, format_bytes(stats.evict_bytes).c_str(),
               sock_state_total, fs_state_total, state_queue_internal);
      ui->set_status_line(4, buf);

      /* Line 5: State stats */
      snprintf(buf, sizeof(buf), "Error Bound: %d", ptmc_state.error_bound);
      ui->set_status_line(5, buf);
    }
    else
    {
      /* No UI mode: use simple text output without ANSI codes */
      /* Only print every 10 seconds to avoid spamming logs */
      static time_t last_status_time = 0;
      time_t current_time = time(NULL);
      if (current_time - last_status_time >= 10)
      {
        last_status_time = current_time;

        /* Line 1: Basic stats */
        printf("[STATUS] States: %zu searched | %zu unique | %zu queued | "
               "Depth: %zu | %.1f states/s\n",
               g_states_searched.load(), unique_states, queue_size, depth,
               states_per_sec);

        /* Line 2: Worker queues */
        printf("[STATUS] Queues: IO=[%zu/%zu] | Prefetch=[%zu]\n",
               io_queue_size, io_queue_cap, prefetch_queue_size);

        /* Line 3: Cache L1 */
        double l1_pct = l1_cap > 0 ? 100.0 * l1_usage / l1_cap : 0;
        printf("[STATUS] L1 Hot: %s / %s (%.1f%%) | %zu entries\n",
               format_bytes(l1_usage).c_str(), format_bytes(l1_cap).c_str(),
               l1_pct, l1_entries);

        /* Line 4: Cache L2 */
        double l2_pct = l2_cap > 0 ? 100.0 * l2_usage / l2_cap : 0;
        printf("[STATUS] L2 Warm: %s / %s (%.1f%%) | %zu entries\n",
               format_bytes(l2_usage).c_str(), format_bytes(l2_cap).c_str(),
               l2_pct, l2_entries);

        /* Line 5: Time */
        int hours = (int)(elapsed / 3600);
        int minutes = ((int)elapsed % 3600) / 60;
        int seconds = (int)elapsed % 60;
        printf("[STATUS] Elapsed: %02d:%02d:%02d | Total SysStates: %zu\n",
               hours, minutes, seconds, total_sysstates);

        /* Line 6: Mode */
        const char *mode_str = "DFS";
        if (ptmc_state.mode == PTMC_STATE::MODE_RAND)
          mode_str = "RAND";
        else if (ptmc_state.mode == PTMC_STATE::MODE_DFS)
          mode_str = "DFS";
        else if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
          mode_str = "BFS";
        printf("[STATUS] Mode: %s | Auto: %s | SIGINT: %s\n", mode_str,
               is_auto_mode() ? "ON" : "OFF", sigint_received ? "YES" : "NO");

        /* Line 7: Process status */
        int running = 0, exited = 0;
        for (int i = 0; i < NP; i++)
        {
          if (DISDEAD(ptmc_state.status[i]))
            exited++;
          else
            running++;
        }
        printf("[STATUS] Processes: %d running | %d exited | Current: %d\n",
               running, exited, ptmc_state.cursor);

        /* Line 8: Current hash */
        printf("[STATUS] Current: %016lx\n", ptmc_state.running_state.ss_hash);
        fflush(stdout);
      }
    }

    /* Sleep 1 second */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  /* Show cursor again (only if not using UI) */
  // Skip ANSI cursor show in no-ui mode to avoid terminal issues
  // if (!ui)
  // {
  //   detsim::ui::ui_printf(ANSI_SHOW_CURSOR);
  //   fflush(stdout);
  // }
}

static void start_status_monitor()
{
  g_monitor_running = true;
  g_start_time = gettime();
  g_last_stat_time = std::chrono::high_resolution_clock::now();
  g_max_depth_recorded = 0;

  /* Open depth statistics file for append */
  std::lock_guard<std::mutex> lock(g_depth_stat_mutex);
  g_monitor_thread = std::thread(status_monitor_thread);
  pthread_setname_np(g_monitor_thread.native_handle(), "status_monitor");
}

static void stop_status_monitor()
{
  g_monitor_running = false;
  if (g_monitor_thread.joinable())
  {
    g_monitor_thread.join();
  }

  /* Clear depth data points for next run */
  {
    std::lock_guard<std::mutex> data_lock(g_depth_data_mutex);
    g_depth_data_points.clear();
  }
  {
    std::lock_guard<std::mutex> fit_lock(g_fit_mutex);
    g_fit_a = 0.0;
    g_fit_b = 0.0;
  }
  g_max_depth_recorded = 0;

  /* If not using UI, clear status area and show cursor */
  detsim::ui::NCursesUI *ui = get_ncurses_ui();
}

bool state_to_be_discarded(int index, syscall_info *infos)
{
  if (ptmc_state.error_bound <= 0)
    return true;

  if (index == 0)
  {
    uintptr_t raft = eval<uintptr_t>("raft", 0);
    if (raft == 0)
      return false; // If we can't evaluate, don't discard
    int state = eval<int>("((raft_server*)raft)->state", 0);
    if (state == 2 || state == 3)
    {
      return true;
    }
  }

  return false;
}

/* auto mode: */
int exec_bfs()
{
  ptmc_state.mode = PTMC_STATE::MODE_BFS;
  start_time = gettime();
  srand(time(NULL));
  std::optional<sys_state> state_fetched = std::nullopt;
  syscall_info syscall_info[NP];

  /* Statistics for this execution only */
  size_t states_searched_this_run = 0;
  size_t states_new_this_run = 0;

  /* Reset StateStore statistics for this run */
  StateStore::instance().reset_stats();
  StateStore::instance().enable_prefetch(100);

  /* Initialize global statistics */
  g_states_searched = 0;
  g_states_new = 0;
  g_queue_size = 0;

  sys_state s = ptmc_state.running_state;
  StateStore::instance().queue_clear();
  state_queue_append(ptmc_state.running_state);

  /* Check if this is a new state or was already in state_set */
  if (!state_set.count(s.ss_hash))
  {
    states_new_this_run++;
  }
  state_set.emplace(s.ss_hash);
  states_searched_this_run++;

  /* Update global stats */
  g_states_searched = states_searched_this_run;
  g_states_new = states_new_this_run;

  ptmc_state.n_choose = 0;
  ptmc_state.choose = 0;

  /* Start status monitor in auto mode */
  start_status_monitor();

  /* until no state in queue */
  int loop_count = 0;
  while (true)
  {
    loop_count++;
    if (loop_count % 100 == 0)
    {
      LOG_TRACE("BFS loop %d, queue_size=%zu, searched=%zu", loop_count,
                StateStore::instance().queue_size(), states_searched_this_run);
    }

    state_fetched = state_queue_extract();
    if (!state_fetched.has_value())
    {
      LOG_TRACE("BFS: queue empty, exiting loop. Total searched: %zu",
                states_searched_this_run);
      break;
    }
    std::vector<int> indexes;
    for (int k = 0; k < NP; k++)
      indexes.push_back(k);
    shuffle(indexes.begin(), indexes.end(), std::default_random_engine(rand()));

    for (auto &i : indexes)
    {
    again:
      s = state_fetched.value();

      if (DISDEAD(s.status[i]))
        continue;

      for (int j = 0; j < NP; j++)
        ptmc_state.status[j] = s.status[j];

      for (int j = 0; j < NP; j++)
        if (s.ts_hash[j] == 0)
          LOG_CRIT("Child state hash is 0 for tracee %d. This should not "
                   "happen if the state is properly saved.",
                   j);

      TransitionResult result = state_transition(s, i);
      g_save_time_us += result.save_time.count();
      g_restore_time_us += result.restore_time.count();
      g_save_count++;
      states_searched_this_run++;
      g_states_searched = states_searched_this_run;

      int check_result = check_state();
      if (check_result != 0)
      {
        detsim::ui::ui_printf("Stopped for illegal state. Searched for %zu "
                              "sys_states (new: %zu)\n",
                              states_searched_this_run, states_new_this_run);
        show_syscall_history();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                              time_used, states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        state_fetched->clear();
      }

      if (result.code != CKPT_DISCARD &&
          !state_to_be_discarded(i, syscall_info))
      {
        bool is_new = !state_set.count(result.new_state.ss_hash);
        if (is_new)
        {
          if (check_result == 0)
            state_queue_append(result.new_state);
          state_set.emplace(result.new_state.ss_hash);
          states_new_this_run++;
        }
      }

      if (ptmc_state.n_choose)
      {
        if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
        {
          ptmc_state.choose++;
          if (ptmc_state.choose < ptmc_state.n_choose)
            goto again;
          ptmc_state.choose = 0;
        }
      }
    }

    /* Update global statistics for monitor */
    g_states_searched = states_searched_this_run;
    g_states_new = states_new_this_run;
    g_queue_size = StateStore::instance().queue_size();

    if (sigint_received)
    {
      stop_status_monitor();
      detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
      /* Wait for pending StateStore writes to complete to ensure data
       * consistency */
      detsim::ui::ui_printf("Flushing pending state writes...\n");
      StateStore::instance().wait_for_completion();
      /* Flush StateStorePacked index */
      StateStorePacked::instance().flush_index();
      /* Flush SysStateStore index to merge incremental entries */
      SysStateStore::instance().flush_index();
      detsim::ui::ui_printf(
          "Searched for %zu sys_states (new: %zu), total unique: %zu\n",
          states_searched_this_run, states_new_this_run, state_set.size());
      if (ptmc_state.running_state.ss_hash == 0)
        LOG_CRIT("Current state hash is 0. This should not happen if states "
                 "are properly saved.");
      show_syscall_history();
      stop_status_monitor();
      double time_used = gettime() - start_time;
      detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                            time_used, states_searched_this_run / time_used);
      StateStore::instance().print_stats();
      sigint_received = 0;
      state_set.clear();
      return 0;
    }
  }
  stop_status_monitor();
  detsim::ui::ui_printf(
      "Complete explore all %zu sys_states (new: %zu), total unique: %zu\n",
      states_searched_this_run, states_new_this_run, state_set.size());
  show_syscall_history();
  double time_used = gettime() - start_time;
  detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
                        states_searched_this_run / time_used);
  StateStore::instance().print_stats();
  return 0;
}

static int states_searched_this_run = 0;
static int states_new_this_run = 0;

struct DFSFrame
{
  hash_type ss_hash;
  int depth;
  int i;
  int state;
  sys_state s;
  std::vector<int> indexes;
  size_t index_pos;
  std::vector<int> choices;
  int n_choose;
  int choosed;
  int ptmc_n_choose;
  int ptmc_choose;
  struct syscall_info si_arr[NP];

  DFSFrame()
      : ss_hash(0), depth(0), i(0), state(0), index_pos(0), n_choose(-1),
        choosed(0), ptmc_n_choose(0), ptmc_choose(-1)
  {
  }
};

int do_dfs(hash_type ss_hash, int depth)
{
  states_searched_this_run++;
  bool is_new = !state_set.count(ss_hash);
  if (is_new)
  {
    state_set.emplace(ss_hash);
    states_new_this_run++;
  }
  else
    return 0;

  if (depth <= 0)
    return 0;

  /* already at this state */
  sys_state s = sys_state(ss_hash);
  syscall_info syscall_info[NP];

  std::vector<int> indexes;
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.status[i] = s.status[i];
    indexes.push_back(i);
    if (s.ts_hash[i] == 0)
    {
      LOG_CRIT("Child state hash is 0 for tracee %d. This should not happen if "
               "the state is properly saved.",
               i);
      print_call_stack();
    }
  }
  shuffle(indexes.begin(), indexes.end(), std::default_random_engine(rand()));

  for (auto &i : indexes)
  {
    if (DISDEAD(s.status[i]))
      continue;

    std::vector<int> choices;
    ptmc_state.n_choose = 0;
    ptmc_state.choose = -1;
    int choosed = 0;
    int n_choose = -1;

    do
    {
      for (int j = 0; j < NP; j++)
      {
        ptmc_state.status[j] = s.status[j];
      }
      g_restore_count++;
      g_states_searched = states_searched_this_run;
      g_states_new = states_new_this_run;
      g_queue_size = 0;

      if (choosed)
      {
        ptmc_state.n_choose = n_choose;
        ptmc_state.choose = choices.back();
        choices.pop_back();
      }

      TransitionResult result = state_transition(s, i);
      g_save_time_us += result.save_time.count();
      g_restore_time_us += result.restore_time.count();

      n_choose = ptmc_state.n_choose;
      if (!choosed && choices.empty() && n_choose > 1)
      {
        choosed = 1;
        for (int c = 0; c < ptmc_state.n_choose; c++)
          if (c != ptmc_state.choose)
            choices.push_back(c);
        shuffle(choices.begin(), choices.end(),
                std::default_random_engine(rand()));
      }

      g_save_count++;
      if (check_state() != 0)
      {
        stop_status_monitor();
        detsim::ui::ui_printf("Stopped for illegal state. Searched for %zu "
                              "sys_states (new: %zu)\n",
                              states_searched_this_run, states_new_this_run);
        show_syscall_history();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                              time_used, states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        return 1;
      }

      if (result.code != CKPT_DISCARD &&
          !state_to_be_discarded(i, syscall_info))
      {
        LOG_DEBUG("DFS depth %d, tracee %d, choose %d, hash %016lx", depth, i,
                  ptmc_state.choose, result.new_state.ss_hash);
        if (do_dfs(result.new_state.ss_hash, depth - 1) == 1)
          return 1;
      }
    } while (!choices.empty());
  }

  if (sigint_received)
  {
    stop_status_monitor();
    detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
    /* Wait for pending StateStore writes to complete to ensure data consistency
     */
    detsim::ui::ui_printf("Flushing pending state writes...\n");
    StateStore::instance().wait_for_completion();
    /* Flush StateStorePacked index */
    StateStorePacked::instance().flush_index();
    /* Flush SysStateStore index to merge incremental entries */
    SysStateStore::instance().flush_index();
    detsim::ui::ui_printf(
        "Searched for %zu sys_states (new: %zu), total unique: %zu\n",
        states_searched_this_run, states_new_this_run, state_set.size());
    if (ptmc_state.running_state.ss_hash == 0)
      LOG_CRIT("Current state hash is 0. This should not happen if states are "
               "properly saved.");
    show_syscall_history();
    stop_status_monitor();
    double time_used = gettime() - start_time;
    detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                          time_used, states_searched_this_run / time_used);
    StateStore::instance().print_stats();
    sigint_received = 0;
    state_set.clear();
    return 1;
  }

  return 0;
}

int do_dfs_iterative(hash_type initial_ss_hash, int initial_depth)
{
  std::vector<DFSFrame> stack;
  stack.reserve(1024);

  DFSFrame root;
  root.ss_hash = initial_ss_hash;
  root.depth = initial_depth;
  root.i = 0;
  root.state = 0;
  root.index_pos = 0;
  stack.push_back(std::move(root));

  while (!stack.empty())
  {
    DFSFrame &frame = stack.back();

    if (frame.state == 0)
    {
      states_searched_this_run++;
      bool is_new = !state_set.count(frame.ss_hash);
      if (!is_new)
      {
        stack.pop_back();
        continue;
      }
      state_set.emplace(frame.ss_hash);
      states_new_this_run++;

      if (frame.depth <= 0)
      {
        stack.pop_back();
        continue;
      }

      frame.s = sys_state(frame.ss_hash);
      for (int j = 0; j < NP; j++)
      {
        ptmc_state.status[j] = frame.s.status[j];
        frame.indexes.push_back(j);
        if (frame.s.ts_hash[j] == 0)
        {
          LOG_CRIT(
              "Child state hash is 0 for tracee %d. This should not happen if "
              "the state is properly saved.",
              j);
          print_call_stack();
        }
      }
      shuffle(frame.indexes.begin(), frame.indexes.end(),
              std::default_random_engine(rand()));

      frame.state = 1;
      frame.index_pos = 0;
    }

    if (frame.state == 1)
    {
      if (frame.index_pos >= frame.indexes.size())
      {
        stack.pop_back();
        continue;
      }

      int i = frame.indexes[frame.index_pos];
      frame.i = i;

      if (DISDEAD(frame.s.status[i]))
      {
        frame.index_pos++;
        continue;
      }

      frame.choices.clear();
      frame.n_choose = -1;
      frame.choosed = 0;
      ptmc_state.n_choose = 0;
      ptmc_state.choose = -1;
      frame.ptmc_n_choose = 0;
      frame.ptmc_choose = -1;

      frame.state = 2;
    }

    if (frame.state == 2)
    {
      int i = frame.i;

      for (int j = 0; j < NP; j++)
      {
        ptmc_state.status[j] = frame.s.status[j];
      }
      g_restore_count++;
      g_states_searched = states_searched_this_run;
      g_states_new = states_new_this_run;
      g_queue_size = 0;

      if (frame.choosed)
      {
        ptmc_state.n_choose = frame.ptmc_n_choose;
        ptmc_state.choose = frame.choices.back();
        frame.choices.pop_back();
      }

      TransitionResult result = state_transition(frame.s, i);
      g_save_time_us += result.save_time.count();
      g_restore_time_us += result.restore_time.count();

      frame.ptmc_n_choose = ptmc_state.n_choose;

      if (!frame.choosed && frame.choices.empty() && frame.ptmc_n_choose > 1)
      {
        frame.choosed = 1;
        for (int c = 0; c < ptmc_state.n_choose; c++)
          if (c != ptmc_state.choose)
            frame.choices.push_back(c);
        shuffle(frame.choices.begin(), frame.choices.end(),
                std::default_random_engine(rand()));
      }

      g_save_count++;
      if (check_state() != 0)
      {
        stop_status_monitor();
        detsim::ui::ui_printf("Stopped for illegal state. Searched for %zu "
                              "sys_states (new: %zu)\n",
                              states_searched_this_run, states_new_this_run);
        show_syscall_history();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                              time_used, states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        return 1;
      }

      if (result.code != CKPT_DISCARD &&
          !state_to_be_discarded(i, frame.si_arr))
      {
        LOG_DEBUG("DFS depth %d, tracee %d, choose %d, hash %016lx",
                  frame.depth, i, ptmc_state.choose, result.new_state.ss_hash);

        if (!frame.choices.empty())
        {
          frame.state = 3;
        }
        else
        {
          frame.index_pos++;
          frame.state = 1;
        }

        DFSFrame new_frame;
        new_frame.ss_hash = result.new_state.ss_hash;
        new_frame.depth = frame.depth - 1;
        new_frame.i = 0;
        new_frame.state = 0;
        new_frame.index_pos = 0;
        stack.push_back(std::move(new_frame));
        continue;
      }

      if (frame.choices.empty())
      {
        frame.index_pos++;
        frame.state = 1;
      }
    }

    if (frame.state == 3)
    {
      if (sigint_received)
      {
        stop_status_monitor();
        detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
        detsim::ui::ui_printf("Flushing pending state writes...\n");
        StateStore::instance().wait_for_completion();
        StateStorePacked::instance().flush_index();
        SysStateStore::instance().flush_index();
        detsim::ui::ui_printf(
            "Searched for %zu sys_states (new: %zu), total unique: %zu\n",
            states_searched_this_run, states_new_this_run, state_set.size());
        if (ptmc_state.running_state.ss_hash == 0)
          LOG_CRIT(
              "Current state hash is 0. This should not happen if states are "
              "properly saved.");
        show_syscall_history();
        stop_status_monitor();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                              time_used, states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        sigint_received = 0;
        state_set.clear();
        return 1;
      }

      if (frame.choices.empty())
      {
        frame.index_pos++;
        frame.state = 1;
      }
      else
      {
        frame.state = 2;
      }
    }
  }

  return 0;
}

int exec_dfs(int depth)
{
  srand(time(NULL));
  ptmc_state.mode = PTMC_STATE::MODE_DFS;
  StateStore::instance().reset_stats();
  StateStore::instance().disable_prefetch();
  g_states_searched = 1;
  g_states_new = 1;
  states_searched_this_run = 1;
  states_new_this_run = 1;
  start_status_monitor();

  StateStore::instance().queue_clear();
  start_time = gettime();

  do_dfs_iterative(ptmc_state.running_state.ss_hash, depth);

  stop_status_monitor();
  detsim::ui::ui_printf(
      "Complete explore all %zu sys_states (new: %zu), total unique: %zu\n",
      states_searched_this_run, states_new_this_run, state_set.size());
  show_syscall_history();
  double time_used = gettime() - start_time;
  detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
                        states_searched_this_run / time_used);
  StateStore::instance().print_stats();

  uint64_t total_save_us = g_save_time_us.load();
  uint64_t total_restore_us = g_restore_time_us.load();
  detsim::ui::ui_printf("\nTransition Timing:\n");
  detsim::ui::ui_printf("  Total save time:    %lu ms (%.1f%%)\n",
                        total_save_us / 1000,
                        100.0 * total_save_us / (time_used * 1000000));
  detsim::ui::ui_printf("  Total restore time: %lu ms (%.1f%%)\n",
                        total_restore_us / 1000,
                        100.0 * total_restore_us / (time_used * 1000000));
  detsim::ui::ui_printf("  Save+Restore overhead: %.1f%%\n",
                        100.0 * (total_save_us + total_restore_us) /
                            (time_used * 1000000));

  return 0;
}

int exec_rand(int depth)
{
  ptmc_state.mode = PTMC_STATE::MODE_RAND;
  start_time = gettime();
  srand(time(NULL));
  hash_type current_hash = ptmc_state.running_state.ss_hash;

  /* Statistics for this execution only */
  size_t states_searched_this_run = 0;
  size_t states_new_this_run = 0;

  /* Reset StateStore statistics for this run */
  StateStore::instance().reset_stats();
  StateStore::instance().disable_prefetch();

  /* Initialize global statistics */
  g_queue_size = 0;
  g_traces_searched = 0;
  g_states_searched = states_searched_this_run;
  g_states_new = states_new_this_run;

  start_status_monitor();

  while (true)
  {
    sys_state s(current_hash);

    for (int k = 0; k < depth; k++)
    {
      int i = rand() % NP;
      if (DISDEAD(s.status[i]))
        continue;

      for (int j = 0; j < NP; j++)
      {
        ptmc_state.status[j] = s.status[j];
        if (s.ts_hash[j] == 0)
        {
          LOG_CRIT("Child state hash is 0 for tracee %d. This should not "
                   "happen if the state is properly saved.",
                   j);
          print_call_stack();
        }
      }

      TransitionResult result = state_transition(s, i);
      g_save_count++;
      g_save_time_us += result.save_time.count();
      g_restore_time_us += result.restore_time.count();

      bool is_new = !state_set.count(result.new_state.ss_hash);
      if (is_new)
      {
        state_set.emplace(result.new_state.ss_hash);
        states_new_this_run++;
      }
      states_searched_this_run++;

      if (check_state() != 0)
      {
        detsim::ui::ui_printf("Stopped for illegal state. Searched for %zu "
                              "sys_states (new: %zu)\n",
                              states_searched_this_run, states_new_this_run);
        show_syscall_history();
        double time_used = gettime() - start_time;
        detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                              time_used, states_searched_this_run / time_used);
        StateStore::instance().print_stats();
        break;
      }
      g_states_searched = states_searched_this_run;
      g_states_new = states_new_this_run;

      s = std::move(result.new_state);
    }

    if (sigint_received)
    {
      stop_status_monitor();
      detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
      /* Wait for pending StateStore writes to complete to ensure data
       * consistency */
      detsim::ui::ui_printf("Flushing pending state writes...\n");
      StateStore::instance().wait_for_completion();
      /* Flush StateStorePacked index */
      StateStorePacked::instance().flush_index();
      /* Flush SysStateStore index to merge incremental entries */
      SysStateStore::instance().flush_index();
      detsim::ui::ui_printf(
          "Searched for %zu sys_states (new: %zu), total unique: %zu\n",
          states_searched_this_run, states_new_this_run, state_set.size());
      if (ptmc_state.running_state.ss_hash == 0)
        LOG_CRIT("Current state hash is 0. This should not happen if states "
                 "are properly saved.");
      show_syscall_history();
      stop_status_monitor();
      double time_used = gettime() - start_time;
      detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                            time_used, states_searched_this_run / time_used);
      StateStore::instance().print_stats();

      uint64_t total_save_us = g_save_time_us.load();
      uint64_t total_restore_us = g_restore_time_us.load();
      detsim::ui::ui_printf("\nTransition Timing:\n");
      detsim::ui::ui_printf("  Total save time:    %lu ms (%.1f%%)\n",
                            total_save_us / 1000,
                            100.0 * total_save_us / (time_used * 1000000));
      detsim::ui::ui_printf("  Total restore time: %lu ms (%.1f%%)\n",
                            total_restore_us / 1000,
                            100.0 * total_restore_us / (time_used * 1000000));
      detsim::ui::ui_printf("  Save+Restore overhead: %.1f%%\n",
                            100.0 * (total_save_us + total_restore_us) /
                                (time_used * 1000000));

      sigint_received = 0;
      state_set.clear();
      return 0;
    }
    g_traces_searched++;
  }
  stop_status_monitor();
  detsim::ui::ui_printf(
      "Complete explore all %zu sys_states (new: %zu), total unique: %zu\n",
      states_searched_this_run, states_new_this_run, state_set.size());
  show_syscall_history();
  double time_used = gettime() - start_time;
  detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n", time_used,
                        states_searched_this_run / time_used);
  StateStore::instance().print_stats();

  uint64_t total_save_us = g_save_time_us.load();
  uint64_t total_restore_us = g_restore_time_us.load();
  detsim::ui::ui_printf("\nTransition Timing:\n");
  detsim::ui::ui_printf("  Total save time:    %lu ms (%.1f%%)\n",
                        total_save_us / 1000,
                        100.0 * total_save_us / (time_used * 1000000));
  detsim::ui::ui_printf("  Total restore time: %lu ms (%.1f%%)\n",
                        total_restore_us / 1000,
                        100.0 * total_restore_us / (time_used * 1000000));
  detsim::ui::ui_printf("  Save+Restore overhead: %.1f%%\n",
                        100.0 * (total_save_us + total_restore_us) /
                            (time_used * 1000000));

  return 0;
}
