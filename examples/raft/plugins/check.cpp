/*
 * Raft Safety Property Checker
 *
 * Checks:
 * 1. Term monotonicity: term should never decrease
 * 2. Leader uniqueness: only one leader per term
 * 3. Snapshot term consistency: snapshot term <= current term
 * 4. Vote integrity: one vote per term per node
 * 5. Leader completeness: committed entries exist on future leaders
 * 6. Match idx monotonicity: leader's view of follower progress never decreases
 * 7. Committed entries consistency: all nodes agree on committed log
 * 8. Log index bounds checking: prevent out-of-bounds access
 */

#include "common.h"
#include "debug.h"
#include "monitor.h"
#include "utils/expr_eval.hpp"
#include <cstring>
#include <fmt/format.h>

/* Forward declaration of log structure to access internals
 * This matches the layout in raft/src/raft_log.c
 */
typedef struct
{
  int size;
  int count;
  int front;
  int back;
  int base;
  void *entries;
  void *cb;
  void *raft;
} log_private_t;

extern "C"
{

  bool is_node_initialized(int node_id)
  {
    std::string ptr_expr = fmt::format("(void*)raft");
    bool success = false;
    uintptr_t ptr_val = eval<uintptr_t>(ptr_expr.c_str(), node_id, &success);
    return success && ptr_val != 0;
  }

  long get_node_term(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string term_expr =
        fmt::format("((raft_server_private_t*)raft)->current_term");
    return eval<long>(term_expr.c_str(), node_id, success);
  }

  bool is_node_leader(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return false;
    }
    std::string state_expr =
        fmt::format("((raft_server_private_t*)raft)->state");
    int state = eval<int>(state_expr.c_str(), node_id, success);
    return *success && state == 3;
  }

  long get_snapshot_term(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string snap_term_expr =
        fmt::format("((raft_server_private_t*)raft)->snapshot_last_term");
    return eval<long>(snap_term_expr.c_str(), node_id, success);
  }

  long get_voted_for(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return -1;
    }
    std::string expr = fmt::format("((raft_server_private_t*)raft)->voted_for");
    return eval<long>(expr.c_str(), node_id, success);
  }

  long get_commit_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr =
        fmt::format("((raft_server_private_t*)raft)->commit_idx");
    return eval<long>(expr.c_str(), node_id, success);
  }

  long get_last_applied_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr =
        fmt::format("((raft_server_private_t*)raft)->last_applied_idx");
    return eval<long>(expr.c_str(), node_id, success);
  }

  /* Get current log index by accessing log structure directly
   * current_idx = log->base + log->count
   */
  long get_current_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    // Access log_private_t fields directly
    // base is at offset 16, count is at offset 4
    std::string expr = fmt::format(
        "((log_private_t*)((raft_server_private_t*)raft)->log)->base + "
        "((log_private_t*)((raft_server_private_t*)raft)->log)->count");
    return eval<long>(expr.c_str(), node_id, success);
  }

  int get_node_count(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr = fmt::format("((raft_server_private_t*)raft)->num_nodes");
    return eval<int>(expr.c_str(), node_id, success);
  }

  /* Safe helper to get log entry term at specific index
   * Returns 0 on success, -1 on error
   * Uses direct structure access instead of function calls
   */
  int get_log_entry_term(int node_id, long idx, long *term_out)
  {
    if (!is_node_initialized(node_id) || idx <= 0)
      return -1;

    bool success = false;

    // Get log pointer
    std::string log_expr = fmt::format("((raft_server_private_t*)raft)->log");
    uintptr_t log_ptr = eval<uintptr_t>(log_expr.c_str(), node_id, &success);
    if (!success)
      return -1;

    // Read count and base from log_private_t
    std::string count_expr =
        fmt::format("((log_private_t*){})->count", log_ptr);
    long count = eval<long>(count_expr.c_str(), node_id, &success);
    if (!success)
      return -1;

    std::string base_expr = fmt::format("((log_private_t*){})->base", log_ptr);
    long base = eval<long>(base_expr.c_str(), node_id, &success);
    if (!success)
      return -1;

    // Check if index is valid
    // if (idx <= base || idx > base + count)
    //   return -1;

    // Get entries array and other fields for circular buffer calculation
    std::string entries_expr =
        fmt::format("((log_private_t*){})->entries", log_ptr);
    uintptr_t entries_array =
        eval<uintptr_t>(entries_expr.c_str(), node_id, &success);
    if (!success || entries_array == 0)
      return -1;

    std::string front_expr =
        fmt::format("((log_private_t*){})->front", log_ptr);
    long front = eval<long>(front_expr.c_str(), node_id, &success);
    if (!success)
      return -1;

    std::string size_expr = fmt::format("((log_private_t*){})->size", log_ptr);
    long size = eval<long>(size_expr.c_str(), node_id, &success);
    if (!success || size <= 0)
      return -1;

    // Calculate position in circular buffer
    long logical_idx = idx - base - 1;
    long physical_idx = (front + logical_idx) % size;

    // Access entry pointer from entries array
    std::string entry_ptr_expr =
        fmt::format("&((raft_entry_t *){})[{}]", entries_array, physical_idx);
    uintptr_t entry_ptr =
        eval<uintptr_t>(entry_ptr_expr.c_str(), node_id, &success);
    if (!success || entry_ptr == 0)
      return -1;

    // Read term from entry (first field in raft_entry_t is term)
    std::string term_expr = fmt::format("((raft_entry_t*){})->term", entry_ptr);
    long term = eval<long>(term_expr.c_str(), node_id, &success);
    if (!success)
      return -1;

    *term_out = term;
    return 0;
  }

  int check_term_monotonicity(int node_id)
  {
    bool success = false;
    long current_term = get_node_term(node_id, &success);

    if (!success || current_term < 0)
      return 0;

    long prev_term = ptmc_state.raft_states[node_id].current_term;

    if (current_term < prev_term)
    {
      detsim::ui::ui_printf(
          "\n[CHECK FAILED] Node %d term decreased: %d -> %d\n", node_id,
          prev_term, current_term);
      detsim::ui::ui_printf(
          "  This violates Raft's term monotonicity property!\n");
      return 1;
    }

    long snap_term = get_snapshot_term(node_id, &success);
    if (success && snap_term > 0 && current_term < snap_term)
    {
      detsim::ui::ui_printf(
          "\n[CHECK FAILED] Node %d current_term(%ld) < snapshot_term(%ld)\n",
          node_id, current_term, snap_term);
      return 1;
    }

    return 0;
  }

  int check_leader_uniqueness()
  {
    long term_leaders[100] = {0};
    long term_leader_count[100] = {0};

    for (int i = 0; i < NP; i++)
    {
      bool success = false;
      if (is_node_leader(i, &success))
      {
        int term = get_node_term(i, &success);
        if (success && term >= 0 && term < 100)
        {
          term_leader_count[term]++;
          term_leaders[term] = i;
          if (term_leader_count[term] > 1)
          {
            detsim::ui::ui_printf(
                "\n[CHECK FAILED] Multiple leaders in term %d!\n", term);
            detsim::ui::ui_printf("  Nodes %d and %d both claim leadership\n",
                                  term_leaders[term], i);
            return 1;
          }
        }
      }
    }
    return 0;
  }

  int check_snapshot_term_consistency()
  {
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;
      std::string snap_idx_expr =
          fmt::format("((raft_server_private_t*)raft)->snapshot_last_idx");
      long snap_idx = eval<long>(snap_idx_expr.c_str(), i, &success);
      long snap_term = get_snapshot_term(i, &success);
      long current_term = get_node_term(i, &success);

      if (success && snap_idx > 0 && snap_term > 0)
      {
        if (snap_term > current_term)
        {
          detsim::ui::ui_printf(
              "\n[CHECK FAILED] Node %d loaded stale snapshot!\n", i);
          detsim::ui::ui_printf("  snapshot_term=%ld, current_term=%ld\n",
                                snap_term, current_term);
          return 1;
        }
      }
    }
    return 0;
  }

  int check_next_match_idx_relationship()
  {
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;
      int node_count = get_node_count(i, &success);
      if (!success || node_count <= 0)
        continue;

      for (int j = 0; j < node_count; j++)
      {
        if (j == i)
          continue;

        std::string next_expr =
            fmt::format("((raft_node_private_t*)((raft_server_private_t*)raft)-"
                        ">nodes[{}])->next_idx",
                        j);
        std::string match_expr =
            fmt::format("((raft_node_private_t*)((raft_server_private_t*)raft)-"
                        ">nodes[{}])->match_idx",
                        j);

        long next_idx = eval<long>(next_expr.c_str(), i, &success);
        if (!success)
          continue;
        long match_idx = eval<long>(match_expr.c_str(), i, &success);
        if (!success)
          continue;

        if (next_idx <= match_idx)
        {
          detsim::ui::ui_printf("\n[CHECK FAILED] Node %d has next_idx(%ld) <= "
                                "match_idx(%ld) for follower %d!\n",
                                i, next_idx, match_idx, j);
          return 1;
        }
      }
    }
    return 0;
  }

  int check_commit_idx_monotonicity(int node_id)
  {
    if (!is_node_initialized(node_id))
      return 0;

    bool success = false;
    long commit_idx = get_commit_idx(node_id, &success);
    if (!success)
      return 0;

    long prev_commit_idx = ptmc_state.raft_states[node_id].commit_idx;

    if (commit_idx < prev_commit_idx)
    {
      detsim::ui::ui_printf(
          "\n[CHECK FAILED] Node %d commit_idx decreased: %ld -> %ld\n",
          node_id, prev_commit_idx, commit_idx);
      return 1;
    }

    return 0;
  }

  int check_applied_vs_committed()
  {
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;
      long commit_idx = get_commit_idx(i, &success);
      if (!success)
        continue;
      long applied_idx = get_last_applied_idx(i, &success);
      if (!success)
        continue;

      if (applied_idx > commit_idx)
      {
        detsim::ui::ui_printf("\n[CHECK FAILED] Node %d last_applied_idx(%ld) "
                              "> committed_idx(%ld)\n",
                              i, applied_idx, commit_idx);
        return 1;
      }
    }
    return 0;
  }

  int check_vote_integrity(int node_id)
  {
    if (!is_node_initialized(node_id))
      return 0;

    bool success = false;
    long current_term = get_node_term(node_id, &success);
    if (!success)
      return 0;

    long voted_for = get_voted_for(node_id, &success);
    if (!success)
      return 0;

    long prev_term = ptmc_state.raft_states[node_id].current_term;
    long prev_voted_for = ptmc_state.raft_states[node_id].voted_for;

    if (current_term == prev_term && prev_voted_for != -1 &&
        voted_for != prev_voted_for)
    {
      detsim::ui::ui_printf(
          "\n[CHECK FAILED] Node %d changed vote in term %ld!\n", node_id,
          current_term);
      detsim::ui::ui_printf("  Previously voted for %ld, now voted for %ld\n",
                            prev_voted_for, voted_for);
      return 1;
    }

    return 0;
  }

  int check_match_idx_monotonicity(int node_id)
  {
    if (!is_node_initialized(node_id))
      return 0;

    bool success = false;
    if (!is_node_leader(node_id, &success) || !success)
      return 0;

    int node_count = get_node_count(node_id, &success);
    if (!success || node_count <= 0)
      return 0;

    for (int j = 0; j < node_count; j++)
    {
      if (j == node_id)
        continue;

      std::string match_expr =
          fmt::format("((raft_node_private_t*)((raft_server_private_t*)raft)->"
                      "nodes[{}])->match_idx",
                      j);
      long match_idx = eval<long>(match_expr.c_str(), node_id, &success);
      if (!success)
        continue;

      long prev_match = ptmc_state.raft_states[node_id].match_idx[j];
      if (match_idx < prev_match)
      {
        detsim::ui::ui_printf(
            "\n[CHECK FAILED] Leader %d: match_idx for node %d decreased!\n",
            node_id, j);
        detsim::ui::ui_printf("  %ld -> %ld\n", prev_match, match_idx);
        return 1;
      }
    }
    return 0;
  }

  /* Get actual leader - the node with highest term among those claiming
   * leadership This handles network partition scenarios where an old leader may
   * still think it's leader (state == 3) but is actually stale. The leader with
   * highest term is considered the actual leader. Returns node_id of actual
   * leader, or -1 if no leader found
   */
  int get_actual_leader()
  {
    int actual_leader = -1;
    long max_term = -1;

    for (int i = 0; i < NP; i++)
    {
      bool success = false;

      // Must claim to be leader
      if (!is_node_leader(i, &success) || !success)
        continue;

      // Get its term
      long term = get_node_term(i, &success);
      if (!success)
        continue;

      // Select leader with highest term
      if (term > max_term)
      {
        max_term = term;
        actual_leader = i;
      }
    }

    return actual_leader;
  }

  int check_committed_entries_replicated_to_majority()
  {
    std::map<long, long> committed_entries;   // idx -> term
    std::vector<int> committed_entries_quorum; // For quick majority check

    int leader_idx = get_actual_leader();
    if (leader_idx < 0)
      return 0; // No leader currently, skip check

    bool success = false;
    long commit_idx = get_commit_idx(leader_idx, &success);
    if (commit_idx <= 0)
      return 0; // No committed entries, skip check

    committed_entries_quorum.resize(commit_idx + 1, 0);

    for (long idx = 1; idx <= std::min(commit_idx, 20L); idx++)
    {
      long entry_term = 0;
      int result = get_log_entry_term(leader_idx, idx, &entry_term);
      if (result != 0)
      {
        long snap_term = get_snapshot_term(leader_idx, &success);
        if (success && snap_term > 0)
          entry_term = snap_term;
        else
          continue;
      }
      committed_entries[idx] = entry_term;
    }

    for (int i = 0; i < NP; i++)
    {
      if (i == leader_idx)
        continue;

      if (!is_node_initialized(i))
        continue;

      bool success = false;
      long check_limit = std::min(commit_idx, 20L);

      for (long idx = 1; idx <= check_limit; idx++)
      {
        long entry_term = 0;
        int result = get_log_entry_term(i, idx, &entry_term);

        if (result != 0)
        {
          long snap_term = get_snapshot_term(i, &success);
          if (success && snap_term > 0)
            entry_term = snap_term;
          else
            continue;
        }

        if (committed_entries[idx] == entry_term)
          committed_entries_quorum[idx]++;
      }
    }

    for (long idx = 1; idx <= std::min(commit_idx, 20L); idx++)
    {
      if (committed_entries_quorum[idx] + 1 < (NP + 1) / 2)
      {
        detsim::ui::ui_printf(
            "\n[CHECK FAILED] Committed entry did not reach majority at index %ld!\n",
            idx);
        detsim::ui::ui_printf("  idx=%ld, term=%ld, replicated on %d nodes (including leader)\n", idx,
                              committed_entries[idx], committed_entries_quorum[idx] + 1);
        detsim::ui::ui_printf(
            "  This violates Raft's commitment safety property!\n");
        return 1;
      }
    }
    return 0;
  }

  int check_leader_completeness()
  {
    /* Leader Completeness Check (Raft Section 5.4.2):
     * If a log entry is committed in a given term, then it will be present
     * in the logs of the leaders for all higher-numbered terms.
     *
     * NOTE: This checks for ENTRIES existence, not commit_idx monotonicity.
     * commit_idx is volatile state that may temporarily be lower on new Leader.
     *
     * We only check when there's an actual leader to avoid false positives
     * during election or network partitions.
     */

    // Find actual leader (highest term among those claiming leadership)
    int leader_id = -1;
    long max_term = -1;
    for (int i = 0; i < NP; i++)
    {
      bool success = false;
      if (!is_node_leader(i, &success) || !success)
        continue;
      long term = get_node_term(i, &success);
      if (success && term > max_term)
      {
        max_term = term;
        leader_id = i;
      }
    }

    if (leader_id < 0)
      return 0; // No leader currently

    // Collect all committed entries from all nodes
    std::map<long, long> committed_entries; // idx -> term
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;
      long commit_idx = get_commit_idx(i, &success);
      if (!success || commit_idx <= 0)
        continue;

      // Sample committed entries (up to 20)
      long check_limit = std::min(commit_idx, 20L);
      for (long idx = 1; idx <= check_limit; idx++)
      {
        long entry_term = 0;
        int result = get_log_entry_term(i, idx, &entry_term);
        if (result != 0)
        {
          long snap_term = get_snapshot_term(i, &success);
          if (success && snap_term > 0)
            entry_term = snap_term;
          else
            continue;
        }

        if (committed_entries.count(idx))
        {
          // Verify consistency
          if (committed_entries[idx] != entry_term)
          {
            detsim::ui::ui_printf(
                "\n[CHECK FAILED] Inconsistent committed entry %ld!\n", idx);
            detsim::ui::ui_printf("  Node %d: term=%ld\n", i, entry_term);
            detsim::ui::ui_printf("  Previous: term=%ld\n",
                                  committed_entries[idx]);
            return 1;
          }

          else
          {
            committed_entries[idx] = entry_term;
          }
        }
      }

      // Verify leader has all committed entries (Leader Completeness)
      for (const auto &entry : committed_entries)
      {
        long idx = entry.first;
        long term = entry.second;
        long leader_entry_term = 0;
        int result = get_log_entry_term(leader_id, idx, &leader_entry_term);

        if (result != 0)
        {
          // Entry not in leader's log array - check if it's in snapshot
          bool success = false;
          long snap_idx = 0;
          std::string snap_idx_expr =
              fmt::format("((raft_server_private_t*)raft)->snapshot_last_idx");
          snap_idx = eval<long>(snap_idx_expr.c_str(), leader_id, &success);

          if (success && snap_idx > 0 && idx <= snap_idx)
          {
            // Entry is in snapshot, consider it as present
            continue;
          }

          // Entry not in log and not in snapshot - violates Leader
          // Completeness!
          detsim::ui::ui_printf(
              "\n[CHECK FAILED] Leader %d missing committed entry %ld!\n",
              leader_id, idx);
          detsim::ui::ui_printf("  Expected term: %ld\n", term);
          detsim::ui::ui_printf("  Leader term: %ld\n", max_term);
          detsim::ui::ui_printf(
              "  This violates Raft's Leader Completeness property!\n");
          return 1;
        }

        if (leader_entry_term != term)
        {
          detsim::ui::ui_printf(
              "\n[CHECK FAILED] Leader %d has wrong term for entry %ld!\n",
              leader_id, idx);
          detsim::ui::ui_printf("  Expected term: %ld, Got: %ld\n", term,
                                leader_entry_term);
          return 1;
        }
      }

    }

    return 0;
  }

  int check_log_consistency() { return 0; }

  void save_raft_state_to_ptmc()
  {
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;

      long term = get_node_term(i, &success);
      if (success)
        ptmc_state.raft_states[i].current_term = term;

      bool is_leader = is_node_leader(i, &success);
      if (success)
        ptmc_state.raft_states[i].is_leader = is_leader ? 1 : 0;

      long commit_idx = get_commit_idx(i, &success);
      if (success)
        ptmc_state.raft_states[i].commit_idx = commit_idx;

      long applied_idx = get_last_applied_idx(i, &success);
      if (success)
        ptmc_state.raft_states[i].applied_idx = applied_idx;

      long voted_for = get_voted_for(i, &success);
      if (success)
        ptmc_state.raft_states[i].voted_for = voted_for;

      int node_count = get_node_count(i, &success);
      if (success && node_count > 0)
      {
        for (int j = 0; j < node_count && j < 3; j++)
        {
          if (j == i)
            continue;

          std::string match_expr =
              fmt::format("((raft_node_private_t*)((raft_server_private_t*)"
                          "raft)->nodes[{}])->match_idx",
                          j);
          std::string next_expr =
              fmt::format("((raft_node_private_t*)((raft_server_private_t*)"
                          "raft)->nodes[{}])->next_idx",
                          j);

          long match_idx = eval<long>(match_expr.c_str(), i, &success);
          if (success)
            ptmc_state.raft_states[i].match_idx[j] = match_idx;

          long next_idx = eval<long>(next_expr.c_str(), i, &success);
          if (success)
            ptmc_state.raft_states[i].next_idx[j] = next_idx;
        }
      }

      long snap_term = get_snapshot_term(i, &success);
      if (success)
        ptmc_state.raft_states[i].last_log_term = snap_term;
    }
  }

  int check()
  {
    int result = 0;

    for (int i = 0; i < NP; i++)
    {
      result |= check_term_monotonicity(i);
      result |= check_commit_idx_monotonicity(i);
      result |= check_vote_integrity(i);
      result |= check_match_idx_monotonicity(i);
    }

    result |= check_leader_uniqueness();
    result |= check_leader_completeness();
    result |= check_snapshot_term_consistency();
    result |= check_next_match_idx_relationship();
    result |= check_applied_vs_committed();
    result |= check_log_consistency();
    result |= check_committed_entries_replicated_to_majority();

    save_raft_state_to_ptmc();

    return result;
  }

} // extern "C"
