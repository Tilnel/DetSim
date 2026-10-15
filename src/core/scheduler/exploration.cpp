/*
 * exploration.cpp - State space exploration implementations
 */

#include "scheduler.h"
#include "state/state.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "state/state_transition.h"
#include "monitor.h"
#include "proc_status.h"
#include "utils/utils.h"
#include "ui/log_wrapper.h"
#include "engine/signal_handler.h"
#include "engine/thread_manager.h"
#include "engine/exec_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <optional>
#include <random>
#include <set>

/* ======================================================================
 * Global State for Exploration
 * ====================================================================== */

/* Exploration statistics - atomic for thread safety */
static std::atomic<size_t> g_states_searched{0};
static std::atomic<size_t> g_states_new{0};
static std::atomic<size_t> g_queue_size{0};
static std::atomic<bool> g_stop_requested{false};

/* Use sigint from signal handler namespace */
using sig::sigint_received;

/* Statistics accumulated during exploration */
static ExplorationStats g_stats;

/* Callbacks */
static StateExploredCallback g_state_explored_cb;
static ExplorationInterruptedCallback g_interrupted_cb;
static ExplorationCompleteCallback g_complete_cb;

/* Timing */
static double g_start_time = 0.0;

/* State set for deduplication */
static std::set<hash_type> g_state_set;

/* ======================================================================
 * Utility Functions
 * ====================================================================== */

static double gettime()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void update_stats()
{
    g_stats.states_searched = g_states_searched.load();
    g_stats.states_unique = g_state_set.size();
    g_stats.states_queued = g_queue_size.load();
    g_stats.elapsed_time = gettime() - g_start_time;
    if (g_stats.elapsed_time > 0) {
        g_stats.states_per_sec = g_stats.states_searched / g_stats.elapsed_time;
    }
}

/* ======================================================================
 * Exploration Control
 * ====================================================================== */

const ExplorationStats& get_exploration_stats()
{
    update_stats();
    return g_stats;
}

void reset_exploration_stats()
{
    g_stats.reset();
    g_states_searched = 0;
    g_states_new = 0;
    g_queue_size = 0;
}

bool should_stop_exploration()
{
    return g_stop_requested.load() || sigint_received;
}

void request_exploration_stop()
{
    g_stop_requested = true;
}

void set_state_explored_callback(StateExploredCallback cb)
{
    g_state_explored_cb = cb;
}

void set_exploration_interrupted_callback(ExplorationInterruptedCallback cb)
{
    g_interrupted_cb = cb;
}

void set_exploration_complete_callback(ExplorationCompleteCallback cb)
{
    g_complete_cb = cb;
}

/* ======================================================================
 * State Checking
 * ====================================================================== */

/* ======================================================================
 * Multi-Threaded State Transition
 * ====================================================================== */

// Structure to hold exploration choices for a state
struct ExplorationChoice {
    int process_idx;      // Which process to run
    int thread_idx;       // Which thread to run (-1 for auto-select)
};

// Get runnable threads for a process after restoring a state
static std::vector<int> get_runnable_threads_for_process(const sys_state& state, int process_idx)
{
    std::vector<int> runnable_threads;

    // Sync the thread manager with the state
    ThreadManager* tm = thread::get_manager(process_idx);
    if (!tm) {
        LOG_ERROR("No ThreadManager for process %d", process_idx);
        // Fall back to single-threaded: return main thread (index 0)
        runnable_threads.push_back(0);
        return runnable_threads;
    }

    tm->sync_from_tracee_state(state.child[process_idx]);

    // Get all runnable threads
    runnable_threads = tm->get_runnable_threads();

    // If no threads are runnable, return empty vector
    // The caller should check for this and skip the process
    LOG_DEBUG("Process %d: found %zu runnable threads", process_idx, runnable_threads.size());

    return runnable_threads;
}

// Execute a state transition with a specific thread
static TransitionResult state_transition_with_thread(const sys_state& source_state,
                                                     int process_idx,
                                                     int thread_idx)
{
    // Set the current thread in the thread manager
    ThreadManager* tm = thread::get_manager(process_idx);
    if (tm) {
        tm->set_current_thread(thread_idx);
        LOG_DEBUG("BFS: Set current thread to %d for process %d", thread_idx, process_idx);
    }

    // Also set in ptmc_state for consistency
    ptmc_state.current_thread_idx[process_idx] = thread_idx;

    // Call the normal state transition
    return state_transition(source_state, process_idx);
}

/* ======================================================================
 * BFS Implementation with Multi-Threaded Support
 * ====================================================================== */

int scheduler_exec_bfs()
{
    ptmc_state.mode = PTMC_STATE::MODE_BFS;
    g_start_time = gettime();
    srand(time(NULL));

    std::optional<sys_state> state_fetched = std::nullopt;
    syscall_info syscall_info[NP];

    size_t states_searched_this_run = 0;
    size_t states_new_this_run = 0;

    g_stop_requested = false;
    g_state_set.clear();

    StateStore::instance().reset_stats();
    StateStore::instance().enable_prefetch(100);

    g_states_searched = 0;
    g_states_new = 0;
    g_queue_size = 0;

    sys_state s = ptmc_state.running_state;
    StateStore::instance().queue_clear();
    state_queue_append(ptmc_state.running_state);

    if (!g_state_set.count(s.ss_hash))
    {
        states_new_this_run++;
    }
    g_state_set.emplace(s.ss_hash);
    states_searched_this_run++;

    g_states_searched = states_searched_this_run;
    g_states_new = states_new_this_run;

    ptmc_state.n_choose = 0;
    ptmc_state.choose = 0;

    int loop_count = 0;
    while (!should_stop_exploration())
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

        LOG_INFO("BFS loop %d: fetched state %016lx, queue_size=%zu", loop_count,
                  state_fetched.value().ss_hash, StateStore::instance().queue_size());

        std::vector<int> indexes;
        for (int k = 0; k < NP; k++)
            indexes.push_back(k);
        shuffle(indexes.begin(), indexes.end(), std::default_random_engine(rand()));

        for (auto &i : indexes)
        {
            s = state_fetched.value();

            if (DISDEAD(s.status[i])) {
                LOG_INFO("Process %d is dead, skipping", i);
                continue;
            }

            LOG_INFO("Processing process %d, state hash=%016lx", i, s.ss_hash);

            for (int j = 0; j < NP; j++)
                ptmc_state.status[j] = s.status[j];

            for (int j = 0; j < NP; j++)
                if (s.ts_hash[j] == 0)
                    LOG_CRIT("Child state hash is 0 for tracee %d. This should not "
                             "happen if the state is properly saved.", j);

            // Get all runnable threads for this process in this state
            std::vector<int> runnable_threads = get_runnable_threads_for_process(s, i);

            LOG_INFO("Process %d has %zu runnable threads", i, runnable_threads.size());

            // Skip if no runnable threads (all blocked or exited)
            if (runnable_threads.empty()) {
                LOG_INFO("Process %d has no runnable threads, skipping", i);
                continue;
            }

            // For multi-threaded BFS, we need to explore each runnable thread
            // as a separate branch in the state space
            for (size_t t = 0; t < runnable_threads.size(); t++) {
                int thread_idx = runnable_threads[t];

                // Restore the state for each thread exploration (except the first)
                if (t > 0) {
                    s.recover_running_state();
                    ptmc_state.running_state = s;
                    LOG_DEBUG("Restored state for thread %d exploration", thread_idx);
                }

                // Execute with the specific thread
                TransitionResult result = state_transition_with_thread(s, i, thread_idx);
                LOG_INFO("BFS: Process %d, Thread %d: result code=%d, new_state_hash=%016lx",
                         i, thread_idx, result.code, result.new_state.ss_hash);
                states_searched_this_run++;
                g_states_searched = states_searched_this_run;

                int check_result = result.property_violation;
                if (check_result != 0)
                {
                    update_stats();
                    if (g_interrupted_cb)
                        g_interrupted_cb(g_stats, "illegal state detected");
                    state_fetched = std::nullopt;
                    g_state_set.clear();
                    return 1;
                }

                if (result.code != CKPT_DISCARD)
                {
                    bool is_new = !g_state_set.count(result.new_state.ss_hash);
                    if (is_new)
                    {
                        if (check_result == 0)
                            state_queue_append(result.new_state);
                        g_state_set.emplace(result.new_state.ss_hash);
                        states_new_this_run++;
                        g_states_new = states_new_this_run;
                    }
                }

                if (g_state_explored_cb)
                {
                    update_stats();
                    g_state_explored_cb(result.new_state.ss_hash, g_stats);
                }

                // Handle multi-choice syscalls (like recvfrom with multiple messages)
                if (ptmc_state.n_choose)
                {
                    if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
                    {
                        // For now, handle choices within the same thread
                        // This is a simplified approach - we explore all choices
                        // sequentially for the same thread
                        ptmc_state.choose++;
                        if (ptmc_state.choose < ptmc_state.n_choose) {
                            // We need to restore and try the next choice
                            // This is handled by going back to the same thread
                            t--;  // Stay on the same thread index
                            continue;
                        }
                        ptmc_state.choose = 0;
                    }
                }
            }
        }

        g_states_searched = states_searched_this_run;
        g_states_new = states_new_this_run;
        g_queue_size = StateStore::instance().queue_size();
    }

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        g_state_set.clear();
        return 0;
    }

    if (g_complete_cb)
        g_complete_cb(g_stats);

    g_state_set.clear();
    return 0;
}

/* ======================================================================
 * DFS Implementation
 * ====================================================================== */

static int dfs_recursive(int depth, int max_depth, std::set<hash_type> &visited)
{
    if (should_stop_exploration())
        return 0;

    if (depth >= max_depth)
        return 0;

    sys_state s = ptmc_state.running_state;

    for (int i = 0; i < NP; i++)
    {
        if (DISDEAD(s.status[i]))
            continue;

    again:
        for (int j = 0; j < NP; j++)
            ptmc_state.status[j] = s.status[j];

        TransitionResult result = state_transition(s, i);
        g_states_searched++;

        int check_result = result.property_violation;
        if (check_result != 0)
        {
            update_stats();
            if (g_interrupted_cb)
                g_interrupted_cb(g_stats, "illegal state detected");
            return 1;
        }

        if (result.code != CKPT_DISCARD)
        {
            bool is_new = !visited.count(result.new_state.ss_hash);
            if (is_new)
            {
                visited.emplace(result.new_state.ss_hash);
                g_states_new++;

                if (g_state_explored_cb)
                {
                    update_stats();
                    g_state_explored_cb(result.new_state.ss_hash, g_stats);
                }

                int ret = dfs_recursive(depth + 1, max_depth, visited);
                if (ret != 0)
                    return ret;

                s.recover_running_state();
                ptmc_state.running_state = s;
            }
        }

        if (ptmc_state.n_choose)
        {
            if (ptmc_state.mode == PTMC_STATE::MODE_DFS)
            {
                ptmc_state.choose++;
                if (ptmc_state.choose < ptmc_state.n_choose)
                    goto again;
                ptmc_state.choose = 0;
            }
        }
    }

    return 0;
}

int scheduler_exec_dfs(int depth)
{
    ptmc_state.mode = PTMC_STATE::MODE_DFS;
    g_start_time = gettime();

    g_stop_requested = false;
    g_states_searched = 0;
    g_states_new = 0;

    std::set<hash_type> visited;
    visited.emplace(ptmc_state.running_state.ss_hash);

    int ret = dfs_recursive(0, depth, visited);

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        return 0;
    }

    if (ret == 0 && g_complete_cb)
        g_complete_cb(g_stats);

    return ret;
}

/* ======================================================================
 * Random Exploration Implementation
 * ====================================================================== */

int scheduler_exec_rand(int depth)
{
    ptmc_state.mode = PTMC_STATE::MODE_RAND;
    g_start_time = gettime();
    srand(time(NULL));

    g_stop_requested = false;
    g_states_searched = 0;
    g_states_new = 0;

    std::set<hash_type> visited;
    visited.emplace(ptmc_state.running_state.ss_hash);

    for (int step = 0; step < depth && !should_stop_exploration(); step++)
    {
        sys_state s = ptmc_state.running_state;

        std::vector<int> live_procs;
        for (int i = 0; i < NP; i++)
        {
            if (!DISDEAD(s.status[i]))
                live_procs.push_back(i);
        }

        if (live_procs.empty())
            break;

        int proc = live_procs[rand() % live_procs.size()];

        for (int j = 0; j < NP; j++)
            ptmc_state.status[j] = s.status[j];

        TransitionResult result = state_transition(s, proc);
        g_states_searched++;

        int check_result = result.property_violation;
        if (check_result != 0)
        {
            update_stats();
            if (g_interrupted_cb)
                g_interrupted_cb(g_stats, "illegal state detected");
            return 1;
        }

        if (result.code != CKPT_DISCARD)
        {
            bool is_new = !visited.count(result.new_state.ss_hash);
            if (is_new)
            {
                visited.emplace(result.new_state.ss_hash);
                g_states_new++;
            }

            if (g_state_explored_cb)
            {
                update_stats();
                g_state_explored_cb(result.new_state.ss_hash, g_stats);
            }
        }
    }

    update_stats();

    if (sigint_received)
    {
        sigint_received = 0;
        if (g_interrupted_cb)
            g_interrupted_cb(g_stats, "SIGINT received");
        return 0;
    }

    if (g_complete_cb)
        g_complete_cb(g_stats);

    return 0;
}
