#include "api/plugin.h"
#include <cstdio>
#include <limits>
namespace {
detsim_verdict choose_time(const detsim_host_v2 *, const detsim_state_view *, uint32_t,
 int32_t choice, detsim_time now, uint32_t, detsim_time_choice_v2 *out, detsim_diagnostic diagnostic) {
  if (choice < 0 || choice > 1 || now.seconds == std::numeric_limits<int64_t>::max()) {
    if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "invalid time choice");
    return DETSIM_ERROR;
  }
  int32_t usec = now.microseconds + (choice ? 10000 : 0);
  *out = {{now.seconds + usec / 1000000, usec % 1000000}, 0};
  return DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2), "paxos.timeout-model", 1,
  nullptr, nullptr, nullptr, nullptr, choose_time, nullptr};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &descriptor; }
