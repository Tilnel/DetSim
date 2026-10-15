#include "api/plugin.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#ifdef DETSIM_FIXTURE_LEGACY
extern "C" int check() { return 0; }
#else
namespace {
#ifndef DETSIM_FIXTURE_DECODER
detsim_verdict check(const detsim_host_v2 *host, const detsim_state_view *view, detsim_diagnostic d) {
  if (!host->configuration || !*host->configuration || !host->inspect_json || !host->read_buffer) return DETSIM_ERROR;
  std::string metadata;
  detsim_writer writer{&metadata, [](void* context,const void* bytes,size_t length)->int {
    static_cast<std::string*>(context)->append(static_cast<const char*>(bytes),length); return 0;
  }};
  if (host->inspect_json(view,0,writer) || metadata.find("\"schema\":1")==std::string::npos) return DETSIM_UNAVAILABLE;
  detsim_buffer_selector selected{DETSIM_BUFFER_FILE,1,0,0,1}; char payload[8]{}; size_t copied=0;
  if (host->read_buffer(view,0,&selected,payload,sizeof(payload),&copied) || copied!=2 || payload[0]!='b' || payload[1]!='c') return DETSIM_UNAVAILABLE;
  selected.fd=999;
  if (host->read_buffer(view,0,&selected,payload,sizeof(payload),&copied)==0 || copied!=0) return DETSIM_ERROR;
  int64_t value = 0;
  if (host->read(view, 0, 0x1000, &value, sizeof(value))) return DETSIM_UNAVAILABLE;
  if (value < 0) { if (d.capacity) std::snprintf(d.data, d.capacity, "negative counter"); return DETSIM_VIOLATION; }
  return DETSIM_PASS;
}
detsim_verdict edge(const detsim_host_v2 *host, const detsim_state_view *a, const detsim_state_view *b, const detsim_event_v2 *, detsim_diagnostic) {
  int64_t x = 0, y = 0;
  if (host->read(a, 0, 0x1000, &x, sizeof(x)) || host->read(b, 0, 0x1000, &y, sizeof(y))) return DETSIM_UNAVAILABLE;
  return y < x ? DETSIM_VIOLATION : DETSIM_PASS;
}
detsim_verdict init(const detsim_host_v2 *, const detsim_state_view *, const char *configuration, detsim_writer out, detsim_diagnostic) {
  const uint8_t zero = 0;
  if (out.append(out.context, &zero, 1)) return DETSIM_ERROR;
  return std::strstr(configuration, "partial_initial") ? DETSIM_VIOLATION : DETSIM_PASS;
}
detsim_verdict advance(const detsim_host_v2 *, const detsim_state_view *, const detsim_state_view *, const detsim_event_v2 *event, detsim_bytes previous, detsim_writer out, detsim_diagnostic) {
  if (previous.size != 1) return DETSIM_ERROR;
  const uint8_t next = previous.data[0] + event->choice;
  if (out.append(out.context, &next, 1)) return DETSIM_ERROR;
  return event->choice == 99 ? DETSIM_VIOLATION : DETSIM_PASS;
}
detsim_verdict choose(const detsim_host_v2 *, const detsim_state_view *, uint32_t, int32_t choice,
 detsim_time now, uint32_t budget, detsim_time_choice_v2 *out, detsim_diagnostic) {
  *out = {now, choice == 2 ? budget + 1 : 0};
  out->value.seconds += choice;
  if (choice == 3) out->value.microseconds = 1000000;
  return DETSIM_PASS;
}
#endif
int describe(detsim_bytes input, detsim_writer out) {
  if (input.size == 3 && !std::memcmp(input.data, "BAD", 3)) {
    out.append(out.context, "partial", 7);
    return -1;
  }
  if (input.size == 3 && !std::memcmp(input.data, "OVF", 3)) {
    out.append(out.context, nullptr, 1);
    return 1; // deliberately ignores the writer failure
  }
  if (input.size == 3 && !std::memcmp(input.data, "THR", 3))
    throw std::runtime_error("fixture decoder exception");
  if (input.size != 3 || std::memcmp(input.data, "MSG", 3)) return 0;
  return out.append(out.context, "fixture message", 15) ? -1 : 1;
}
const detsim_plugin_v2 descriptor{
#ifdef DETSIM_FIXTURE_BAD_ABI
1, // ABI1 cannot receive the phase-aware ABI2 event contract.
#else
DETSIM_PLUGIN_ABI_V2,
#endif
#ifdef DETSIM_FIXTURE_DECODER
sizeof(detsim_plugin_v2), "test.display", 0, nullptr, nullptr, nullptr, nullptr, nullptr, describe};
#else
sizeof(detsim_plugin_v2), "test.counter", 1, check, edge, init, advance, choose, describe};
#endif
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &descriptor; }
#endif
