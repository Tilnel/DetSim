#include "api/plugin.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
namespace {
using Json=nlohmann::json;
bool observation(const detsim_host_v2* host,const detsim_state_view* view,Json& result) {
    std::string metadata;
    detsim_writer writer{&metadata,[](void* context,const void* bytes,size_t length) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(bytes),length);return 0;
    }};
    if(host->inspect_json(view,0,writer))return false;
    uint64_t signature=1469598103934665603ULL;
    for(unsigned char byte:metadata){signature^=byte;signature*=1099511628211ULL;}
    result["metadata_signature"]=signature;
    const auto decoded=Json::parse(metadata);
    result["lifecycle"]=decoded.at("lifecycle");result["exit_code"]=decoded.at("exit_code");
    for(const char* name:{"main_marker","worker_marker","word","main_result"}) {
        int64_t value=0;if(host->evaluate_i64(view,0,name,&value))return false;result[name]=value;
    }
    detsim_time clock{};if(host->time(view,0,&clock))return false;
    result["clock"]=Json::array({clock.seconds,clock.microseconds});
    detsim_buffer_selector selector{DETSIM_BUFFER_STDIO,1,0,0,0};char bytes[16]{};size_t copied=0;
    if(host->read_buffer(view,0,&selector,bytes,sizeof(bytes),&copied))return false;
    result["stdout"]=std::string(bytes,copied);
    return true;
}
detsim_verdict edge(const detsim_host_v2* host,const detsim_state_view* before,
                    const detsim_state_view* after,const detsim_event_v2* event,detsim_diagnostic) {
    Json old,next;if(!observation(host,before,old)||!observation(host,after,next))return DETSIM_UNAVAILABLE;
    if(event->choice==DETSIM_CHOICE_TIMEOUT) {
        if(event->phase!=DETSIM_SYSCALL_COMPLETED || !event->result_valid)return DETSIM_ERROR;
        for(const char* key:{"main_marker","worker_marker","word","main_result","stdout"})
            if(old[key]!=next[key])return DETSIM_ERROR;
    }
    Json result={{"process",event->process},{"thread",event->thread},{"choice",event->choice},
      {"syscall",event->syscall_number},{"phase",event->phase},{"result_valid",event->result_valid},
      {"result",event->result},{"before",old},{"after",next}};
    std::printf("NATIVE_SEARCH_EVENT %s\n",result.dump().c_str());std::fflush(stdout);
    return DETSIM_PASS;
}
detsim_verdict choose(const detsim_host_v2*,const detsim_state_view*,uint32_t,int32_t option,
                      detsim_time now,uint32_t,detsim_time_choice_v2* out,detsim_diagnostic) {
    now.microseconds+=option*1000;
    if(now.microseconds>=1000000){++now.seconds;now.microseconds-=1000000;}
    *out={now,0};return DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),
    "test.native-search-actors",1,nullptr,edge,nullptr,nullptr,choose,nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2(){return &descriptor;}
