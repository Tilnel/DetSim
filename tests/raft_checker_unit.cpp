#ifdef REDISRAFT_PLUGIN
#define RAFT_SERVER_TYPE "raft_server"
#define RAFT_LOG_TYPE "raft_log_t"
#define RAFT_LEADER_ROLE 4
#else
#define RAFT_SERVER_TYPE "raft_server_private_t"
#define RAFT_LOG_TYPE "log_private_t"
#define RAFT_LEADER_ROLE 3
#endif
#define RAFT_PLUGIN_ID "test.raft"
#include "../examples/raft/plugins/check_common.hpp"
#include <array>
struct FakeNode {
  int64_t term = 1, role = 0, vote = -1, commit = 1, entry_term = 1;
  int64_t type = 0, base = 0, length = 3;
  std::string command = std::string("a\0b", 3);
  bool readable = true, bytes_readable = true;
};
struct detsim_state_view { std::array<FakeNode, 2> nodes; };
namespace {
size_t count(const detsim_state_view *) { return 2; }
int evaluate(const detsim_state_view *view, uint32_t process, const char *expression, int64_t *value) {
  const auto &n = view->nodes.at(process);
  if (!n.readable) return -1;
  std::string e(expression);
  if (e == "(void*)raft") *value = 100;
  else if (e.ends_with("->current_term")) *value = n.term;
  else if (e.ends_with("->state")) *value = n.role;
  else if (e.ends_with("->voted_for")) *value = n.vote;
  else if (e.ends_with("->commit_idx")) *value = n.commit;
  else if (e.ends_with("->base") || e.ends_with("->snapshot_last_idx")) *value = n.base;
  else if (e.ends_with("->snapshot_last_term")) *value = n.base ? 1 : 0;
  else if (e.ends_with("->last_applied_idx") || e.ends_with("->front") || e.ends_with("->num_nodes")) *value = 0;
  else if (e.ends_with("->log")) *value = 200;
  else if (e.ends_with("->count")) *value = n.base ? 0 : 1;
  else if (e.ends_with("->size")) *value = 8;
  else if (e.ends_with("->entries")) *value = 300;
  else if (e.ends_with("->node")) *value = 400;
  else if (e.ends_with(".term") || e.ends_with("->term")) *value = n.entry_term;
  else if (e.ends_with(".type") || e.ends_with("->type")) *value = n.type;
  else if (e.ends_with(".data.len") || e.ends_with("->data_len")) *value = n.length;
  else if (e.ends_with(".data.buf") || e.ends_with("->data")) *value = 600;
  else if (e.ends_with("[0]")) *value = 500;
  else return -1;
  return 0;
}
int read(const detsim_state_view *view, uint32_t process, uint64_t address, void *out, size_t length) {
  const auto &n = view->nodes.at(process);
  if (!n.bytes_readable || address != 600 || length > n.command.size()) return -1;
  std::memcpy(out, n.command.data(), length); return 0;
}
struct Buffer {
  std::vector<uint8_t> data;
  static int append(void *p, const void *bytes, size_t length) {
    auto &out = static_cast<Buffer *>(p)->data;
    auto start = static_cast<const uint8_t *>(bytes); out.insert(out.end(), start, start + length); return 0;
  }
  detsim_writer writer() { return {this, append}; }
  detsim_bytes bytes() const { return {data.data(), data.size()}; }
};
void expect(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
  const detsim_host_v2 host{DETSIM_PLUGIN_ABI_V2, count, read, evaluate, nullptr, "{}", nullptr, nullptr};
  char message[1024]{}; detsim_diagnostic diagnostic{message, sizeof(message)};
  detsim_event_v2 event{};
  detsim_state_view a; Buffer history;
  expect(init(&host, &a, "{}", history.writer(), diagnostic) == DETSIM_PASS, "init");
  auto b = a; b.nodes[0].term = 2; b.nodes[0].role = RAFT_LEADER_ROLE; Buffer next;
  expect(advance(&host, &a, &b, &event, history.bytes(), next.writer(), diagnostic) == DETSIM_PASS, "valid later leader");
  auto lacking = a; lacking.nodes[0].commit = 0; Buffer lacking_history;
  expect(init(&host, &lacking, "{}", lacking_history.writer(), diagnostic) == DETSIM_PASS, "follower lags committed peer");
  auto later = lacking; later.nodes[0].term = 2; later.nodes[0].role = RAFT_LEADER_ROLE;
  later.nodes[0].command[2] = 'c'; Buffer rejected;
  expect(advance(&host, &lacking, &later, &event, lacking_history.bytes(), rejected.writer(), diagnostic) == DETSIM_VIOLATION, "later leader with conflicting uncommitted command passed");
  auto bad = a; bad.nodes[1].command[2] = 'c'; Buffer ignored;
  expect(init(&host, &bad, "{}", ignored.writer(), diagnostic) == DETSIM_VIOLATION, "same index/term different command bytes passed");
  bad = a; for (auto &n : bad.nodes) n.command[2] = 'c';
  expect(advance(&host, &a, &bad, &event, history.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "committed command history changed");
  bad = a; bad.nodes[1].type = 1;
  expect(init(&host, &bad, "{}", ignored.writer(), diagnostic) == DETSIM_VIOLATION, "same bytes different entry type passed");
  bad = a; bad.nodes[0].entry_term = 0;
  expect(init(&host, &bad, "{}", ignored.writer(), diagnostic) == DETSIM_VIOLATION, "committed term conflict");
  auto decreasing = a; decreasing.nodes[0].term = 0; decreasing.nodes[0].entry_term = 0;
  expect(advance(&host, &a, &decreasing, &event, history.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "decreased term");
  auto voted = a; voted.nodes[0].vote = 0; Buffer voted_history;
  expect(init(&host, &voted, "{}", voted_history.writer(), diagnostic) == DETSIM_PASS, "vote baseline");
  auto revoted = voted; revoted.nodes[0].vote = 1;
  expect(advance(&host, &voted, &revoted, &event, voted_history.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "vote changed in term");
  auto leader_a = a; leader_a.nodes[0].role = RAFT_LEADER_ROLE; Buffer branch_a;
  auto leader_b = a; leader_b.nodes[1].role = RAFT_LEADER_ROLE; Buffer branch_b;
  expect(advance(&host, &a, &leader_a, &event, history.bytes(), branch_a.writer(), diagnostic) == DETSIM_PASS, "branch A leader");
  expect(advance(&host, &a, &leader_b, &event, history.bytes(), branch_b.writer(), diagnostic) == DETSIM_PASS, "branch B leaked leader history");
  expect(advance(&host, &leader_a, &leader_b, &event, branch_a.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "sequential leaders in same term passed");
  expect(branch_a.data != branch_b.data, "leader identity absent from monitor");
  Buffer branch_a_again;
  expect(advance(&host, &a, &leader_a, &event, history.bytes(), branch_a_again.writer(), diagnostic) == DETSIM_PASS && branch_a.data == branch_a_again.data, "monitor replay differs");
  auto unreadable = a; unreadable.nodes[0].readable = false;
  expect(init(&host, &unreadable, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "query failure passed");
  unreadable = a; unreadable.nodes[0].bytes_readable = false;
  expect(init(&host, &unreadable, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "missing command bytes passed");
  auto empty = a; for (auto &n : empty.nodes) { n.length = 0; n.command.clear(); }
  expect(init(&host, &empty, "{}", ignored.writer(), diagnostic) == DETSIM_PASS, "empty normal command is valid");
#ifdef REDISRAFT_PLUGIN
  auto noop = empty; for (auto &n : noop.nodes) { n.type = 4; n.bytes_readable = false; }
  expect(init(&host, &noop, "{}", ignored.writer(), diagnostic) == DETSIM_PASS, "no-op must not read absent payload");
  noop.nodes[0].length = 1;
  expect(init(&host, &noop, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "no-op with malformed payload passed");
#endif
  auto compacted = a; for (auto &n : compacted.nodes) n.base = 1;
  expect(init(&host, &compacted, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "unobserved compacted command passed");
  expect(advance(&host, &a, &compacted, &event, history.bytes(), ignored.writer(), diagnostic) == DETSIM_PASS, "observed compacted prefix");
  auto oversized = a; oversized.nodes[0].length = INT64_MAX;
  expect(init(&host, &oversized, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "unbounded payload passed");
  expect(advance(&host, &a, &b, &event, {nullptr, 0}, ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "missing monitor passed");
  expect(advance(&host, &a, &b, &event, {nullptr, history.data.size()}, ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "null monitor passed");
  for (size_t length = 0; length < history.data.size(); ++length)
    expect(advance(&host, &a, &b, &event, {history.data.data(), length}, ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "truncated monitor passed");
  auto malformed = history; malformed.data.push_back(0);
  expect(advance(&host, &a, &b, &event, malformed.bytes(), ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "trailing monitor bytes passed");
  malformed = history; malformed.data[0] ^= 1;
  expect(advance(&host, &a, &b, &event, malformed.bytes(), ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "wrong monitor version passed");
  auto mutate_word = [](Buffer &buffer, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) buffer.data.at(offset + i) = (value >> (8 * i)) & 255;
  };
  for (auto [offset, value] : std::vector<std::pair<size_t, uint64_t>>{
         {8, UINT64_MAX}, {16, UINT64_MAX}, {24, 0}, {24, 2},
         {32, UINT64_MAX}, {40, 0}, {48, UINT64_MAX}, {56, UINT64_MAX}}) {
    malformed = history; mutate_word(malformed, offset, value);
    expect(advance(&host, &a, &b, &event, malformed.bytes(), ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "invalid serialized commit record passed");
  }
  malformed = branch_a; mutate_word(malformed, malformed.data.size() - 8, 2);
  expect(advance(&host, &a, &b, &event, malformed.bytes(), ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "invalid serialized leader passed");
  expect(advance(&host, &a, &b, &event, {nullptr, max_monitor_bytes + 1}, ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "unbounded monitor passed");
  auto no_reader = host; no_reader.read = nullptr;
  expect(init(&no_reader, &a, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "missing saved-memory reader passed");
  detsim_writer failing{nullptr, [](void *, const void *, size_t) { return -1; }};
  expect(init(&host, &a, "{}", failing, diagnostic) == DETSIM_UNAVAILABLE, "monitor write failure passed");
  Buffer roundtrip; encode(decode(branch_a.bytes(), 2), roundtrip.writer());
  expect(roundtrip.data == branch_a.data, "monitor roundtrip is not canonical");
  Projection monotone(1), regressed(1);
  monotone[0].initialized = regressed[0].initialized = true;
  monotone[0].role = regressed[0].role = RAFT_LEADER_ROLE;
  monotone[0].match.emplace(9, 2); regressed[0].match.emplace(9, 1);
  expect(guarded(diagnostic, [&] { edge_properties(monotone, regressed); }) == DETSIM_VIOLATION, "leader match guard lost");
  expect(descriptor.monitor_schema == 2, "monitor schema not versioned");
  std::puts("PASS Raft exact committed commands/types, branch leader history, replay, bounds, malformed/missing saved data");
}
