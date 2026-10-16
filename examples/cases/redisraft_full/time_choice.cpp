/* Case environment input: hold or advance the supplied saved clock by 10ms. */
#include "api/plugin.h"
#include <cstdio>
#include <limits>

namespace {
detsim_verdict choose_time(const detsim_host_v2 *,const detsim_state_view *,uint32_t,
    int32_t choice,detsim_time now,uint32_t,detsim_time_choice_v2 *out,detsim_diagnostic diagnostic) {
    const auto fail=[&](const char *message) {
        if(diagnostic.data && diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"%s",message);
        return DETSIM_UNAVAILABLE;
    };
    if(!out)return fail("module clock output unavailable");
    if(choice!=0 && choice!=1)return fail("module clock supports only hold0 and advance1");
    if(now.seconds<0 || now.microseconds<0 || now.microseconds>=1000000)
        return fail("module clock source is not normalized");
    const int32_t usec=now.microseconds+(choice?10000:0);
    const int64_t carry=usec/1000000;
    if(now.seconds>std::numeric_limits<int64_t>::max()-carry)return fail("module clock overflow");
    *out={{now.seconds+carry,usec%1000000},0};
    return DETSIM_PASS;
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),
    "cases.redisraft-full.clock-input",1,nullptr,nullptr,nullptr,nullptr,choose_time,nullptr};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &plugin; }
