#include "api/plugin.h"
#include <cstring>
#include <cstdio>
#include <sys/syscall.h>
namespace {
detsim_verdict init(const detsim_host_v2*, const detsim_state_view*, const char*, detsim_writer out, detsim_diagnostic) {
    const uint8_t seen=0;return out.append(out.context,&seen,1)?DETSIM_ERROR:DETSIM_PASS;
}
detsim_verdict advance(const detsim_host_v2* host,const detsim_state_view*,const detsim_state_view* after,
 const detsim_event_v2* event,detsim_bytes previous,detsim_writer out,detsim_diagnostic diagnostic) {
    if(previous.size!=1)return DETSIM_ERROR;
    uint8_t seen=previous.data[0];
    if(event->phase==DETSIM_EXECUTION_STOPPED) {
        if(diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"runtime probe crashed before checked completion");
        return DETSIM_VIOLATION;
    }
    if(event->phase==DETSIM_SYSCALL_COMPLETED && event->result_valid &&
       event->syscall_number==SYS_write && event->args[0]==1 && event->result>0) {
        char bytes[4097]{};size_t copied=0;
        const detsim_buffer_selector selector{DETSIM_BUFFER_FILE,1,0,0,0};
        if(host->read_buffer(after,event->process,&selector,bytes,sizeof(bytes)-1,&copied))return DETSIM_UNAVAILABLE;
        if(std::strstr(bytes,"RUNTIME_OK\n"))seen=1;
    }
    if(event->phase==DETSIM_THREAD_EXITED && event->syscall_number==SYS_exit_group &&
       (event->args[0]!=0 || !seen)) {
        if(diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"runtime probe exited without checked RUNTIME_OK (status=%llu)",(unsigned long long)event->args[0]);
        return DETSIM_VIOLATION;
    }
    return out.append(out.context,&seen,1)?DETSIM_ERROR:DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),"cases.runtime-admission",1,
 nullptr,nullptr,init,advance,nullptr,nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2(){return &descriptor;}
