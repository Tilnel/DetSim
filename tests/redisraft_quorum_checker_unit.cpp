// Focused property tests; actual saved-memory projection is exercised by the
// full/pages three-process scenarios and the existing Raft checker ABI tests.
#include "../examples/redisraft/plugins/quorum.cpp"
#include <iostream>

namespace {
unsigned checks = 0;
Projection cluster() {
  Projection nodes(3);
  for (auto &n : nodes) { n.initialized = true; n.term = 1; n.role = 1; n.vote = -1; }
  nodes[0].role = RAFT_LEADER_ROLE;
  nodes[0].commit = 2;
  const Entry noop{1, 4, {}};
  for (auto &n : nodes) n.logs.emplace(1, noop);
  const Entry command{1, 0, std::string("set alpha 17\0tail", 17)};
  nodes[0].logs.emplace(2, command);
  nodes[1].logs.emplace(2, command);
  return nodes;
}
void expect(const Projection &nodes, detsim_verdict wanted, const char *message = "") {
  char diagnostic[256]{};
  auto actual = guarded({diagnostic, sizeof diagnostic}, [&] { state_properties(nodes); });
  ++checks;
  if (actual != wanted || std::string(diagnostic) != message)
    throw std::runtime_error("case " + std::to_string(checks) + ": " + diagnostic);
}
constexpr const char *missing = "committed NORMAL entry 2 lacks replica quorum";
}
int main() {
  auto nodes = cluster();
  expect(nodes, DETSIM_PASS); // follower need not have learned the commit index
  nodes[1].logs.erase(2);
  expect(nodes, DETSIM_VIOLATION, missing);
  nodes[0].commit = 1;
  expect(nodes, DETSIM_PASS); // an uncommitted proposal may have one copy
  nodes = cluster(); nodes[1].logs[2].term = 2;
  expect(nodes, DETSIM_VIOLATION, missing);
  nodes = cluster(); nodes[1].logs[2].type = 4;
  expect(nodes, DETSIM_VIOLATION, missing);
  nodes = cluster(); nodes[1].logs[2].command.back() = 'x';
  expect(nodes, DETSIM_VIOLATION, missing); // include bytes after embedded NUL
  nodes = cluster(); nodes[1].logs[2].command.pop_back();
  expect(nodes, DETSIM_VIOLATION, missing);
  nodes = cluster(); nodes[1].logs[3] = nodes[1].logs[2]; nodes[1].logs.erase(2);
  expect(nodes, DETSIM_VIOLATION, missing); // same bytes at wrong index do not count
  nodes = cluster(); nodes[1].initialized = false;
  expect(nodes, DETSIM_VIOLATION, missing);
  nodes = cluster(); nodes[2].logs[2] = nodes[0].logs[2]; nodes[1].logs.erase(2);
  expect(nodes, DETSIM_PASS); // quorum may be any two actual replicas
  nodes[2].commit = 2;
  expect(nodes, DETSIM_PASS);
  nodes = cluster(); nodes.resize(2);
  expect(nodes, DETSIM_UNAVAILABLE, "replication quorum requires three fixed members");
  nodes = cluster(); nodes.emplace_back();
  expect(nodes, DETSIM_UNAVAILABLE, "replication quorum requires three fixed members");
  nodes = cluster(); nodes[1].base = 1;
  expect(nodes, DETSIM_UNAVAILABLE, "replication quorum requires uncompacted logs");
  nodes = cluster(); nodes[1].snap_idx = 1;
  expect(nodes, DETSIM_UNAVAILABLE, "replication quorum requires uncompacted logs");
  nodes = cluster(); nodes[0].commit = 1; nodes[0].logs.erase(2); nodes[1].logs.erase(2);
  expect(nodes, DETSIM_PASS); // no-op bootstrap is outside NORMAL-only property
  nodes = cluster(); nodes[2].role = RAFT_LEADER_ROLE;
  expect(nodes, DETSIM_VIOLATION, "multiple leaders in term 1");
  std::cout << "PASS: " << checks << " fixed-member NORMAL quorum property cases\n";
}
