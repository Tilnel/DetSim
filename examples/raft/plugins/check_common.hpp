/* Workload-owned Raft projection and properties. Only the public plugin ABI is
 * used: no runtime globals, guest writes, host PIDs, or saved Raft structs. */
#include "api/plugin.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
struct Unavailable : std::runtime_error { using std::runtime_error::runtime_error; };
struct Violation : std::runtime_error { using std::runtime_error::runtime_error; };
using View = const detsim_state_view *;
struct Reader {
  const detsim_host_v1 *host;
  View view;
  unsigned node;
  int64_t eval(const std::string &expression) const {
    int64_t value = 0;
    if (host->evaluate_i64(view, node, expression.c_str(), &value))
      throw Unavailable("node " + std::to_string(node) + ": cannot evaluate " + expression);
    return value;
  }
  int64_t field(const char *name) const { return eval(std::string("((") + RAFT_SERVER_TYPE + "*)raft)->" + name); }
  int64_t peer(int64_t j, const char *name) const {
#ifdef REDISRAFT_PLUGIN
    return eval("((raft_server*)raft)->nodes[" + std::to_string(j) + "]->" + name);
#else
    return eval("((raft_node_private_t*)((raft_server_private_t*)raft)->nodes[" + std::to_string(j) + "])->" + name);
#endif
  }
};
struct Node {
  bool initialized = false;
  int64_t term = 0, role = 0, vote = 0, commit = 0, applied = 0, snap_idx = 0, snap_term = 0;
  int64_t base = 0, count = 0;
  std::map<int64_t, int64_t> logs, match;
};
using Projection = std::vector<Node>;
Projection project(const detsim_host_v1 *host, View view) {
  Projection nodes(host->process_count(view));
  for (unsigned i = 0; i < nodes.size(); ++i) {
    Reader r{host, view, i}; auto &n = nodes[i];
    n.initialized = r.eval("(void*)raft") != 0;
    if (!n.initialized) continue; // null is legitimate before application startup
    n.term = r.field("current_term"); n.role = r.field("state"); n.vote = r.field("voted_for");
    n.commit = r.field("commit_idx"); n.applied = r.field("last_applied_idx");
    n.snap_idx = r.field("snapshot_last_idx"); n.snap_term = r.field("snapshot_last_term");
    if (n.term < 0 || n.snap_idx < 0 || n.snap_term < 0 || n.commit < 0 || n.applied < 0 || n.snap_term > n.term || n.applied > n.commit)
      throw Violation("node " + std::to_string(i) + ": invalid term/commit/applied/snapshot bounds");
    std::string log = std::string("((") + RAFT_LOG_TYPE + "*)" + std::to_string(r.field("log")) + ")";
    n.base = r.eval(log + "->base"); n.count = r.eval(log + "->count");
    auto size = r.eval(log + "->size"), front = r.eval(log + "->front");
    auto entries = r.eval(log + "->entries");
    if (n.count < 0 || n.base < 0 || size <= 0 || n.count > size || front < 0 || front >= size ||
        n.base > std::numeric_limits<int64_t>::max() - n.count || n.commit > n.base + n.count)
      throw Violation("node " + std::to_string(i) + ": invalid log bounds");
    if (n.count > 1000000) throw Unavailable("Raft projection exceeds one million log entries");
    for (int64_t j = 0; j < n.count; ++j) {
      auto slot = (front + j) % size;
#ifdef REDISRAFT_PLUGIN
      std::string entry = "((raft_entry_t**)" + std::to_string(entries) + ")[" + std::to_string(slot) + "]";
      if (!r.eval(entry)) throw Violation("null retained Raft log entry");
      auto term = r.eval("(" + entry + ")->term");
#else
      auto term = r.eval("((raft_entry_t*)" + std::to_string(entries) + ")[" + std::to_string(slot) + "].term");
#endif
      if (term < 0 || term > n.term) throw Violation("retained log term exceeds current term");
      n.logs.emplace(n.base + j + 1, term);
    }
    if (n.snap_idx) n.logs.emplace(n.snap_idx, n.snap_term);
    if (n.role == RAFT_LEADER_ROLE) {
      auto peers = r.field("num_nodes");
      if (peers < 0 || peers > 1000000) throw Unavailable("invalid Raft peer count");
      auto self = r.field("node");
      for (int64_t j = 0; j < peers; ++j) {
        std::string peer_pointer = std::string("((") + RAFT_SERVER_TYPE + "*)raft)->nodes[" + std::to_string(j) + "]";
        if (r.eval(peer_pointer) == self) continue;
#ifdef REDISRAFT_PLUGIN
        if (r.peer(j, "flags") & 8) continue; // inactive peer is outside active replication set
#endif
        auto id = r.peer(j, "id"), match = r.peer(j, "match_idx"), next = r.peer(j, "next_idx");
        if (match < 0 || next <= match) throw Violation("next_idx must exceed match_idx");
        if (!n.match.emplace(id, match).second) throw Violation("duplicate peer ID");
      }
    }
  }
  return nodes;
}
void state_properties(const Projection &nodes) {
  std::map<int64_t, unsigned> leaders;
  std::map<int64_t, int64_t> committed;
  for (unsigned i = 0; i < nodes.size(); ++i) {
    const auto &n = nodes[i]; if (!n.initialized) continue;
    if (n.role == RAFT_LEADER_ROLE && !leaders.emplace(n.term, i).second)
      throw Violation("multiple leaders in term " + std::to_string(n.term));
    for (auto [idx, term] : n.logs) if (idx <= n.commit) {
      auto inserted = committed.emplace(idx, term);
      if (!inserted.second && inserted.first->second != term) throw Violation("conflicting committed entry " + std::to_string(idx));
    }
  }
}
void edge_properties(const Projection &before, const Projection &after) {
  if (before.size() != after.size()) throw Unavailable("process count changed");
  for (unsigned i = 0; i < before.size(); ++i) {
    const auto &a = before[i], &b = after[i];
    if (!a.initialized) continue;
    if (!b.initialized) throw Violation("initialized Raft pointer disappeared without a declared restart model");
    if (b.term < a.term || b.commit < a.commit) throw Violation("term or commit index decreased");
    if (b.term == a.term && a.vote != -1 && b.vote != a.vote) throw Violation("vote changed within a term");
    if (a.role == RAFT_LEADER_ROLE && b.role == RAFT_LEADER_ROLE && a.term == b.term)
      for (auto [id, match] : a.match) {
        auto found = b.match.find(id);
        if (found != b.match.end() && found->second < match) throw Violation("leader match index decreased within term");
      }
  }
}
struct Commit { int64_t term, observed_term; };
using History = std::map<int64_t, Commit>;
uint64_t read_u64(detsim_bytes bytes, size_t &position) {
  if (position > bytes.size || bytes.size - position < 8) throw Unavailable("truncated Raft monitor");
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes.data[position++]) << (8 * i);
  return value;
}
History decode(detsim_bytes bytes) {
  size_t at = 0; auto count = read_u64(bytes, at);
  if (count > 1000000 || bytes.size != 8 + count * 24) throw Unavailable("invalid Raft monitor length");
  History history;
  for (uint64_t i = 0; i < count; ++i) {
    auto idx = read_u64(bytes, at), term = read_u64(bytes, at), observed = read_u64(bytes, at);
    if (idx == 0 || idx > INT64_MAX || term > INT64_MAX || observed > INT64_MAX ||
        !history.emplace(idx, Commit{static_cast<int64_t>(term), static_cast<int64_t>(observed)}).second)
      throw Unavailable("invalid Raft monitor record");
  }
  return history;
}
void encode(const History &history, detsim_writer out) {
  auto emit = [&](uint64_t value) {
    uint8_t bytes[8]; for (unsigned i = 0; i < 8; ++i) bytes[i] = (value >> (i * 8)) & 255;
    if (out.append(out.context, bytes, sizeof(bytes))) throw Unavailable("cannot emit Raft monitor");
  };
  emit(history.size());
  for (auto [idx, entry] : history) { emit(idx); emit(entry.term); emit(entry.observed_term); }
}
void observe(History &history, const Projection &nodes) {
  for (const auto &n : nodes) if (n.initialized) {
    for (auto [idx, term] : n.logs) if (idx <= n.commit) {
      auto inserted = history.emplace(idx, Commit{term, n.term});
      if (!inserted.second) {
        if (inserted.first->second.term != term) throw Violation("committed history changed at index " + std::to_string(idx));
        inserted.first->second.observed_term = std::min(inserted.first->second.observed_term, n.term);
      }
    }
    // A compacted prefix is only verifiable if its history was observed earlier.
    auto compacted = std::min(n.base, n.commit);
    if (compacted > 1000000) throw Unavailable("Raft committed history exceeds one million entries");
    for (int64_t idx = 1; idx <= compacted; ++idx)
      if (!history.count(idx)) throw Unavailable("compacted committed prefix predates the monitor");
  }
  for (const auto &n : nodes) if (n.initialized && n.role == RAFT_LEADER_ROLE)
    for (auto [idx, committed] : history) if (n.term > committed.observed_term && idx > n.snap_idx) {
      auto found = n.logs.find(idx);
      if (found == n.logs.end() || found->second != committed.term)
        throw Violation("later-term leader lacks committed entry " + std::to_string(idx));
    }
}
template<class F> detsim_verdict guarded(detsim_diagnostic diagnostic, F action) {
  try { action(); return DETSIM_PASS; }
  catch (const Violation &e) { if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "%s", e.what()); return DETSIM_VIOLATION; }
  catch (const std::exception &e) { if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "%s", e.what()); return DETSIM_UNAVAILABLE; }
  catch (...) { return DETSIM_ERROR; }
}
detsim_verdict init(const detsim_host_v1 *host, View view, const char *, detsim_writer out, detsim_diagnostic diagnostic) {
  return guarded(diagnostic, [&] { auto nodes = project(host, view); state_properties(nodes); History history; observe(history, nodes); encode(history, out); });
}
detsim_verdict advance(const detsim_host_v1 *host, View before, View after, const detsim_event_v1 *, detsim_bytes previous, detsim_writer out, detsim_diagnostic diagnostic) {
  return guarded(diagnostic, [&] { auto a = project(host, before), b = project(host, after); state_properties(b); edge_properties(a, b); auto history = decode(previous); observe(history, b); encode(history, out); });
}
const detsim_plugin_v1 descriptor{DETSIM_PLUGIN_ABI_V1, sizeof(detsim_plugin_v1), RAFT_PLUGIN_ID, 1,
  nullptr, nullptr, init, advance, nullptr, nullptr};
}
extern "C" const detsim_plugin_v1 *detsim_plugin_entry_v1() { return &descriptor; }
