#include "api/plugin.h"
#include <cstdio>
#include <limits>
#include <string>
namespace {
detsim_verdict choose_time(const detsim_host_v1 *host, const detsim_state_view *view,
 uint32_t node, int32_t choice, detsim_time now, uint32_t budget,
 detsim_time_choice_v1 *out, detsim_diagnostic diagnostic) {
  auto fail = [&](const char *message) {
    if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "%s", message);
    return DETSIM_UNAVAILABLE;
  };
  try {
    if (choice < 0 || choice > 1) return fail("timeout model supports exactly two choices");
    *out = {now, 0};
    if (!choice) return DETSIM_PASS;
    int64_t pointer = 0, role = 0, timeout = 0;
    if (host->evaluate_i64(view, node, "(void*)raft", &pointer)) return fail("Raft root symbol unavailable");
    if (!pointer) return DETSIM_PASS; // before Raft initialization time remains fixed
    if (host->evaluate_i64(view, node, "((" RAFT_SERVER_TYPE "*)raft)->state", &role)) return fail("Raft role unavailable");
    const char *expression = role == RAFT_LEADER_ROLE ?
      "((" RAFT_SERVER_TYPE "*)raft)->request_timeout" :
      "((" RAFT_SERVER_TYPE "*)raft)->election_timeout_rand";
    if (host->evaluate_i64(view, node, expression, &timeout)) return fail("Raft timeout unavailable");
    if (timeout < 0 || timeout > INT32_MAX) return fail("Raft timeout out of range");
#ifdef REDISRAFT_PLUGIN
    out->budget_cost = role == RAFT_LEADER_ROLE ? 0 : 1;
    if (out->budget_cost > budget) return fail("timeout model budget exhausted");
#else
    (void)budget;
#endif
    int64_t advance = (timeout + 1) * 1000 + now.microseconds;
    if (now.seconds > std::numeric_limits<int64_t>::max() - advance / 1000000) return fail("time overflow");
    out->value = {now.seconds + advance / 1000000, static_cast<int32_t>(advance % 1000000)};
    return DETSIM_PASS;
  } catch (...) { return fail("timeout projection failed"); }
}
const detsim_plugin_v1 descriptor{DETSIM_PLUGIN_ABI_V1, sizeof(detsim_plugin_v1), RAFT_CHOICE_ID, 1,
  nullptr, nullptr, nullptr, nullptr, choose_time, nullptr};
}
extern "C" const detsim_plugin_v1 *detsim_plugin_entry_v1() { return &descriptor; }
