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
struct detsim_state_view { int64_t term, role, vote, commit, entry_term; bool readable = true; };
namespace {
size_t count(const detsim_state_view *) { return 1; }
int evaluate(const detsim_state_view *view, uint32_t, const char *expression, int64_t *value) {
  if (!view->readable) return -1;
  std::string e(expression);
  if (e == "(void*)raft") *value = 100;
  else if (e.ends_with("->current_term")) *value = view->term;
  else if (e.ends_with("->state")) *value = view->role;
  else if (e.ends_with("->voted_for")) *value = view->vote;
  else if (e.ends_with("->commit_idx")) *value = view->commit;
  else if (e.ends_with("->last_applied_idx") || e.ends_with("->snapshot_last_idx") || e.ends_with("->snapshot_last_term") || e.ends_with("->base") || e.ends_with("->front") || e.ends_with("->num_nodes")) *value = 0;
  else if (e.ends_with("->log")) *value = 200;
  else if (e.ends_with("->count")) *value = 1;
  else if (e.ends_with("->size")) *value = 8;
  else if (e.ends_with("->entries")) *value = 300;
  else if (e.ends_with("->node")) *value = 400;
  else if (e.ends_with(".term") || e.ends_with("->term")) *value = view->entry_term;
  else if (e.ends_with("[0]")) *value = 500; // Redisraft pointer entry
  else return -1;
  return 0;
}
struct Buffer {
  std::vector<uint8_t> data;
  static int append(void *p, const void *bytes, size_t length) {
    auto &out = static_cast<Buffer *>(p)->data;
    auto start = static_cast<const uint8_t *>(bytes); out.insert(out.end(), start, start + length); return 0;
  }
  detsim_writer writer() { return {this, append}; }
  detsim_bytes bytes() { return {data.data(), data.size()}; }
};
void expect(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
  const detsim_host_v1 host{1, count, nullptr, evaluate, nullptr, "{}", nullptr, nullptr};
  char message[1024]{}; detsim_diagnostic diagnostic{message, sizeof(message)};
  detsim_event_v1 event{};
  detsim_state_view a{1, 0, -1, 1, 1}; Buffer history;
  expect(init(&host, &a, "{}", history.writer(), diagnostic) == DETSIM_PASS, "init");
  auto b = a; b.term = 2; b.role = RAFT_LEADER_ROLE; Buffer next;
  expect(advance(&host, &a, &b, &event, history.bytes(), next.writer(), diagnostic) == DETSIM_PASS, "valid later leader");
  auto bad = b; bad.entry_term = 2; Buffer ignored;
  expect(advance(&host, &a, &bad, &event, history.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "committed history conflict");
  auto decreasing = a; decreasing.term = 0; decreasing.entry_term = 0;
  expect(advance(&host, &a, &decreasing, &event, history.bytes(), ignored.writer(), diagnostic) == DETSIM_VIOLATION, "decreased term");
  auto unreadable = a; unreadable.readable = false;
  expect(init(&host, &unreadable, "{}", ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "query failure passed");
  expect(advance(&host, &a, &b, &event, {nullptr, 0}, ignored.writer(), diagnostic) == DETSIM_UNAVAILABLE, "missing monitor passed");
  expect(history.data.size() == 32, "monitor encoding");
  std::puts("PASS workload Raft projection, transition monotonicity, committed history, fail-closed query/monitor");
}
