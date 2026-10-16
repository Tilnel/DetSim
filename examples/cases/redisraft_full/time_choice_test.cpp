#include "api/plugin.h"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

static void need(bool okay,const char *message) { if(!okay)throw std::runtime_error(message); }
int main() {
    try {
        const auto *plugin=detsim_plugin_entry_v2();
        need(plugin && plugin->abi_version==DETSIM_PLUGIN_ABI_V2 && plugin->choose_time,"public time-choice ABI missing");
        char text[128]{};detsim_diagnostic diagnostic{text,sizeof(text)};
        const auto check=[&](int choice,detsim_time now,detsim_verdict verdict,int64_t sec=0,int32_t usec=0) {
            detsim_time_choice_v2 out{{71,72},73};
            need(plugin->choose_time(nullptr,nullptr,3,choice,now,0,&out,diagnostic)==verdict,"time-choice verdict differs");
            if(verdict==DETSIM_PASS)need(out.value.seconds==sec && out.value.microseconds==usec && out.budget_cost==0,"clock/cost differs");
            else need(out.value.seconds==71 && out.value.microseconds==72 && out.budget_cost==73,"rejection mutated output");
        };
        check(0,{1,17},DETSIM_PASS,1,17);
        check(1,{1,17},DETSIM_PASS,1,10017);
        check(1,{1,999999},DETSIM_PASS,2,9999);
        check(1,{1,990000},DETSIM_PASS,2,0);
        check(0,{1,17},DETSIM_PASS,1,17); // Repeated calls have no persistent clock state.
        for(auto choice:{-2,-1,2})check(choice,{1,0},DETSIM_UNAVAILABLE);
        for(auto now:{detsim_time{-1,0},detsim_time{0,-1},detsim_time{0,1000000}})
            for(auto choice:{0,1})check(choice,now,DETSIM_UNAVAILABLE);
        const auto limit=std::numeric_limits<int64_t>::max();
        check(0,{limit,999999},DETSIM_PASS,limit,999999);
        check(1,{limit,989999},DETSIM_PASS,limit,999999);
        check(1,{limit,990000},DETSIM_UNAVAILABLE);
        need(plugin->choose_time(nullptr,nullptr,0,0,{1,0},0,nullptr,diagnostic)==DETSIM_UNAVAILABLE,"null output accepted");
        std::puts("PASS public module time choices: hold, 10ms increment, carry, source/choice/overflow rejection, cost0, repeatability");
        return 0;
    } catch(const std::exception &error) {std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
