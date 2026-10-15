/* Explicit timeout choices for the bounded library fixture. */
#include "api/plugin.h"
#include <cstdio>
#include <cstdint>
#include <limits>
namespace {
detsim_verdict choose_time(const detsim_host_v2 *host, const detsim_state_view *view,
    uint32_t node, int32_t choice, detsim_time now, uint32_t,
    detsim_time_choice_v2 *out, detsim_diagnostic diagnostic) {
    auto fail = [&](const char *message) {
        if (diagnostic.capacity) std::snprintf(diagnostic.data, diagnostic.capacity, "%s", message);
        return DETSIM_UNAVAILABLE;
    };
    if (choice < 0 || choice > 1) return fail("Raft fixture supports choices 0 and 1");
    *out = {now, 0};
    if (!choice) return DETSIM_PASS;
    int64_t root=0, role=0, timeout=0;
    if (host->evaluate_i64(view,node,"(void*)r",&root)) return fail("Raft root unavailable");
    if (!root) return DETSIM_PASS;
    if (host->evaluate_i64(view,node,"((raft_server_private_t*)r)->state",&role)) return fail("Raft role unavailable");
    const char *field=role==3 ? "((raft_server_private_t*)r)->request_timeout" :
                              "((raft_server_private_t*)r)->election_timeout_rand";
    if (host->evaluate_i64(view,node,field,&timeout) || timeout<0 || timeout>INT32_MAX)
        return fail("Raft timeout unavailable or invalid");
    int64_t delta=(timeout+1)*1000+now.microseconds;
    if (now.seconds>std::numeric_limits<int64_t>::max()-delta/1000000) return fail("Raft clock overflow");
    out->value={now.seconds+delta/1000000,static_cast<int32_t>(delta%1000000)};
    return DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),
    "case.willemt-raft.timeout",1,nullptr,nullptr,nullptr,nullptr,choose_time,nullptr};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &descriptor; }
