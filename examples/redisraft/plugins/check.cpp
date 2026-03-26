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

extern "C"
{

  struct EvalCache
  {
    std::unordered_map<std::string, long> cache;

    void clear() { cache.clear(); }

    std::string make_key(int node_id, const std::string &expr)
    {
      static_assert(NP <= 5, "Node ID exceeds prefix array size");
      static const std::string prefix[5] = {"0:", "1:", "2:", "3:", "4:"};
      return prefix[node_id] + expr;
    }

    bool get(int node_id, const std::string &expr, long *value)
    {
      std::string key = make_key(node_id, expr);
      auto it = cache.find(key);
      if (it != cache.end())
      {
        *value = it->second;
        return true;
      }
      return false;
    }

    void set(int node_id, const std::string &expr, long value)
    {
      cache[make_key(node_id, expr)] = value;
    }
  };

  static EvalCache g_eval_cache;

  long eval_cached_long(const std::string &expr, int node_id, bool *success)
  {
    long value;
    if (g_eval_cache.get(node_id, expr, &value))
    {
      if (success)
        *success = true;
      return value;
    }

    value = eval<long>(expr.c_str(), node_id, success);
    if (success && *success)
    {
      g_eval_cache.set(node_id, expr, value);
    }
    return value;
  }

  int eval_cached_int(const std::string &expr, int node_id, bool *success)
  {
    long value;
    if (g_eval_cache.get(node_id, expr, &value))
    {
      if (success)
        *success = true;
      return static_cast<int>(value);
    }

    int int_value = eval<int>(expr.c_str(), node_id, success);
    if (success && *success)
    {
      g_eval_cache.set(node_id, expr, static_cast<long>(int_value));
    }
    return int_value;
  }

  uintptr_t eval_cached_uintptr(const std::string &expr, int node_id,
                                bool *success)
  {
    long value;
    if (g_eval_cache.get(node_id, expr, &value))
    {
      if (success)
        *success = true;
      return static_cast<uintptr_t>(value);
    }

    uintptr_t ptr_val = eval<uintptr_t>(expr.c_str(), node_id, success);
    if (success && *success)
    {
      g_eval_cache.set(node_id, expr, static_cast<long>(ptr_val));
    }
    return ptr_val;
  }

  bool is_node_initialized(int node_id)
  {
    std::string ptr_expr = "(void*)raft";
    bool success = false;
    uintptr_t ptr_val = eval_cached_uintptr(ptr_expr, node_id, &success);
    return success && ptr_val != 0;
  }

  long get_node_term(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string term_expr = "((raft_server*)raft)->current_term";
    return eval_cached_long(term_expr, node_id, success);
  }

  bool is_node_leader(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return false;
    }
    std::string state_expr = "((raft_server*)raft)->state";
    int state = eval_cached_int(state_expr, node_id, success);
    return *success && state == 4;
  }

  long get_snapshot_term(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string snap_term_expr =
        "((raft_server*)raft)->snapshot_last_term";
    return eval_cached_long(snap_term_expr, node_id, success);
  }

  long get_snapshot_last_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string snap_idx_expr =
        "((raft_server*)raft)->snapshot_last_idx";
    return eval_cached_long(snap_idx_expr, node_id, success);
  }

  long get_voted_for(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return -1;
    }
    std::string expr = "((raft_server*)raft)->voted_for";
    return eval_cached_long(expr, node_id, success);
  }

  long get_commit_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr = "((raft_server*)raft)->commit_idx";
    return eval_cached_long(expr, node_id, success);
  }

  long get_last_applied_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr = "((raft_server*)raft)->last_applied_idx";
    return eval_cached_long(expr, node_id, success);
  }

  long get_current_idx(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string log_expr = "((raft_server*)raft)->log";
    uintptr_t log_ptr = eval_cached_uintptr(log_expr, node_id, success);
    if (!*success)
      return 0;
    std::string base_expr = fmt::format("((raft_log_t*){})->base", log_ptr);
    std::string count_expr = fmt::format("((raft_log_t*){})->count", log_ptr);
    long base = eval_cached_long(base_expr, node_id, success);
    if (!*success)
      return 0;
    long count = eval_cached_long(count_expr, node_id, success);
    if (!*success)
      return 0;
    return base + count;
  }

  int get_node_count(int node_id, bool *success)
  {
    if (!is_node_initialized(node_id))
    {
      *success = false;
      return 0;
    }
    std::string expr = "((raft_server*)raft)->num_nodes";
    return eval_cached_int(expr, node_id, success);
  }

  /* Safe helper to get log entry term at specific index
   * Returns 0 on success, -1 on error
   * Uses direct log structure access
   */
  int get_log_entry_term(int node_id, long idx, long *term_out)
  {
    if (!is_node_initialized(node_id) || idx <= 0)
      return -1;

    bool success = false;

    std::string log_expr = "((raft_server*)raft)->log";
    uintptr_t log_ptr = eval_cached_uintptr(log_expr, node_id, &success);
    if (!success)
      return -1;

    std::string count_expr = fmt::format("((raft_log_t*){})->count", log_ptr);
    long count = eval_cached_long(count_expr, node_id, &success);
    if (!success)
      return -1;

    std::string base_expr = fmt::format("((raft_log_t*){})->base", log_ptr);
    long base = eval_cached_long(base_expr, node_id, &success);
    if (!success)
      return -1;

    if (idx <= base || idx > base + count)
      return -1;

    std::string entries_expr =
        fmt::format("((raft_log_t*){})->entries", log_ptr);
    uintptr_t entries_array =
        eval_cached_uintptr(entries_expr, node_id, &success);
    if (!success || entries_array == 0)
      return -1;

    std::string front_expr = fmt::format("((raft_log_t*){})->front", log_ptr);
    long front = eval_cached_long(front_expr, node_id, &success);
    if (!success)
      return -1;

    std::string size_expr = fmt::format("((raft_log_t*){})->size", log_ptr);
    long size = eval_cached_long(size_expr, node_id, &success);
    if (!success || size <= 0)
      return -1;

    long logical_idx = idx - base - 1;
    long physical_idx = (front + logical_idx) % size;

    std::string entry_ptr_expr =
        fmt::format("((raft_entry_t**){})[{}]", entries_array, physical_idx);
    uintptr_t entry_ptr =
        eval_cached_uintptr(entry_ptr_expr, node_id, &success);
    if (!success || entry_ptr == 0)
      return -1;

    // Read term from entry
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
      long snap_idx = get_snapshot_last_idx(i, &success);
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
      if (!is_node_leader(i, &success) || !success)
        continue;

      int node_count = get_node_count(i, &success);
      if (!success || node_count <= 0)
        continue;

      for (int j = 0; j < node_count; j++)
      {
        std::string node_id_expr =
            fmt::format("((raft_server*)raft)->nodes[{}]->id", j);
        int node_id = eval_cached_int(node_id_expr, i, &success);
        if (!success)
          continue;
        if (node_id == i)
          continue;

        std::string inactive_expr =
            fmt::format("((raft_server*)raft)->nodes[{}]->flags & 8", j);
        int is_inactive = eval_cached_int(inactive_expr, i, &success);
        if (!success || is_inactive)
          continue;

        std::string next_expr =
            fmt::format("((raft_server*)raft)->nodes[{}]->next_idx", j);
        std::string match_expr =
            fmt::format("((raft_server*)raft)->nodes[{}]->match_idx", j);

        long next_idx = eval_cached_long(next_expr, i, &success);
        if (!success)
          continue;
        long match_idx = eval_cached_long(match_expr, i, &success);
        if (!success)
          continue;

        if (next_idx <= match_idx)
        {
          detsim::ui::ui_printf("\n[CHECK FAILED] Node %d has next_idx(%ld) <= "
                                "match_idx(%ld) for follower %d!\n",
                                i, next_idx, match_idx, node_id);
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
    if (!ptmc_state.raft_states[node_id].is_leader)
      return 0;

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
          fmt::format("((raft_server*)raft)->nodes[{}]->match_idx", j);
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

  int get_actual_leader()
  {
    int actual_leader = -1;
    long max_term = -1;

    for (int i = 0; i < NP; i++)
    {
      bool success = false;

      if (!is_node_leader(i, &success) || !success)
        continue;

      long term = get_node_term(i, &success);
      if (!success)
        continue;

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
    std::map<long, long> committed_entries;
    std::vector<int> committed_entries_quorum;

    int leader_idx = get_actual_leader();
    if (leader_idx < 0)
      return 0;

    bool success = false;
    long commit_idx = get_commit_idx(leader_idx, &success);
    if (commit_idx <= 0)
      return 0;

    committed_entries_quorum.resize(commit_idx + 1, 0);

    for (long idx = 1; idx <= std::min(commit_idx, 20L); idx++)
    {
      long entry_term = 0;
      int result = get_log_entry_term(leader_idx, idx, &entry_term);
      if (result != 0)
      {
        long snap_idx = get_snapshot_last_idx(leader_idx, &success);
        if (success && snap_idx > 0 && idx <= snap_idx)
          continue;
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
        if (!committed_entries.count(idx))
          continue;

        long entry_term = 0;
        int result = get_log_entry_term(i, idx, &entry_term);

        if (result == 0 && committed_entries[idx] == entry_term)
        {
          committed_entries_quorum[idx]++;
        }
        else if (result != 0)
        {
          long snap_idx = get_snapshot_last_idx(i, &success);
          if (success && snap_idx > 0 && idx <= snap_idx)
            committed_entries_quorum[idx]++;
        }
      }
    }

    for (long idx = 1; idx <= std::min(commit_idx, 20L); idx++)
    {
      if (!committed_entries.count(idx))
        continue;

      if (committed_entries_quorum[idx] + 1 < (NP + 1) / 2)
      {
        detsim::ui::ui_printf("\n[CHECK FAILED] Committed entry did not reach "
                              "majority at index %ld!\n",
                              idx);
        detsim::ui::ui_printf(
            "  idx=%ld, term=%ld, replicated on %d nodes (including leader)\n",
            idx, committed_entries[idx], committed_entries_quorum[idx] + 1);
        detsim::ui::ui_printf(
            "  This violates Raft's commitment safety property!\n");
        return 1;
      }
    }
    return 0;
  }

  int check_leader_completeness()
  {
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
      return 0;

    std::map<long, long> committed_entries;
    for (int i = 0; i < NP; i++)
    {
      if (!is_node_initialized(i))
        continue;

      bool success = false;
      long commit_idx = get_commit_idx(i, &success);
      if (!success || commit_idx <= 0)
        continue;

      long check_limit = std::min(commit_idx, 20L);
      for (long idx = 1; idx <= check_limit; idx++)
      {
        long entry_term = 0;
        int result = get_log_entry_term(i, idx, &entry_term);
        if (result != 0)
        {
          long snap_idx = get_snapshot_last_idx(i, &success);
          if (success && snap_idx > 0 && idx <= snap_idx)
            continue;
          continue;
        }

        if (committed_entries.count(idx))
        {
          if (committed_entries[idx] != entry_term)
          {
            detsim::ui::ui_printf(
                "\n[CHECK FAILED] Inconsistent committed entry %ld!\n", idx);
            detsim::ui::ui_printf("  Node %d: term=%ld\n", i, entry_term);
            detsim::ui::ui_printf("  Previous: term=%ld\n",
                                  committed_entries[idx]);
            return 1;
          }
        }
        else
        {
          committed_entries[idx] = entry_term;
        }
      }

      for (const auto &entry : committed_entries)
      {
        long idx = entry.first;
        long term = entry.second;
        long leader_entry_term = 0;
        int result = get_log_entry_term(leader_id, idx, &leader_entry_term);

        if (result != 0)
        {
          long snap_idx = get_snapshot_last_idx(leader_id, &success);
          if (success && snap_idx > 0 && idx <= snap_idx)
            continue;

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
              fmt::format("((raft_server*)raft)->nodes[{}]->match_idx", j);
          std::string next_expr =
              fmt::format("((raft_server*)raft)->nodes[{}]->next_idx", j);

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
    g_eval_cache.clear();
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
}
