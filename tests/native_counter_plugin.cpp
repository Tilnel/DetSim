#include "api/plugin.h"
#include <cstdio>
namespace {
detsim_verdict state(const detsim_host_v2* host, const detsim_state_view* view, detsim_diagnostic diagnostic) {
  int64_t counter;
  if (host->evaluate_i64(view, 0, "counter", &counter)) return DETSIM_UNAVAILABLE;
  if (counter >= 2) {
    if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "counter reached two");
    return DETSIM_VIOLATION;
  }
  return DETSIM_PASS;
}
detsim_verdict initial(const detsim_host_v2*, const detsim_state_view*, const char*, detsim_writer writer, detsim_diagnostic) {
  const uint8_t zero = 0;
  return writer.append(writer.context, &zero, 1) ? DETSIM_ERROR : DETSIM_PASS;
}
detsim_verdict advance(const detsim_host_v2*, const detsim_state_view*, const detsim_state_view*, const detsim_event_v2*, detsim_bytes old, detsim_writer writer, detsim_diagnostic) {
  if (old.size != 1 || old.data[0] == 255) return DETSIM_ERROR;
  const uint8_t next = old.data[0] + 1;
  return writer.append(writer.context, &next, 1) ? DETSIM_ERROR : DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2), "test.native-counter", 1,
  state, nullptr, initial, advance, nullptr, nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2() { return &descriptor; }
