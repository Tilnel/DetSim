#include "api/plugin.h"
#include <cstdio>
namespace {
detsim_verdict unavailable(const detsim_host_v1 *, const detsim_state_view *, detsim_diagnostic diagnostic) {
  if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity,
    "Paxos safety projection is not implemented; remove this plugin to run unchecked");
  return DETSIM_UNAVAILABLE;
}
const detsim_plugin_v1 descriptor{DETSIM_PLUGIN_ABI_V1, sizeof(detsim_plugin_v1), "paxos.safety", 1,
  unavailable, nullptr, nullptr, nullptr, nullptr, nullptr};
}
extern "C" const detsim_plugin_v1 *detsim_plugin_entry_v1() { return &descriptor; }
