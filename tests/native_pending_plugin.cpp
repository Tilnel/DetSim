#include "api/plugin.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
namespace {
using Json=nlohmann::json;
struct Counts { uint64_t blocked, completed; };
int append(void* context,const void* data,size_t size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data),size);return 0;
}
Json inspect(const detsim_host_v2* host,const detsim_state_view* view,unsigned node) {
  std::string text;
  if(host->inspect_json(view,node,{&text,append})) throw std::runtime_error("inspect failed");
  return Json::parse(text);
}
detsim_verdict initial(const detsim_host_v2*,const detsim_state_view*,const char*,detsim_writer out,detsim_diagnostic) {
  Counts zero{};return out.append(out.context,&zero,sizeof zero)?DETSIM_ERROR:DETSIM_PASS;
}
detsim_verdict advance(const detsim_host_v2* host,const detsim_state_view* before,
  const detsim_state_view* after,const detsim_event_v2* event,detsim_bytes previous,
  detsim_writer out,detsim_diagnostic diagnostic) {
  try {
    if(previous.size!=sizeof(Counts)) throw std::runtime_error("invalid monitor bytes");
    Counts counts{};std::memcpy(&counts,previous.data,sizeof counts);
    const auto a=inspect(host,before,event->process).at("threads").at(event->thread);
    const auto b=inspect(host,after,event->process).at("threads").at(event->thread);
    if(event->phase==DETSIM_SYSCALL_BLOCKED) {
      if(event->result_valid || event->result || b.at("pending_syscall").is_null())
        throw std::runtime_error("blocked event exposed a syscall return");
      ++counts.blocked;
      std::fprintf(stderr,"PENDING_BLOCKED\n");
    } else if(event->phase==DETSIM_SYSCALL_COMPLETED) {
      if(!event->result_valid || !b.at("pending_syscall").is_null())
        throw std::runtime_error("completion event retained unfinished call");
      if(!a.at("pending_syscall").is_null()) {
        if(a.at("pending_syscall").at("syscall")!=event->syscall_number ||
           a.at("registers").at("rip")!=b.at("registers").at("rip"))
          throw std::runtime_error("pending completion executed user instructions");
        ++counts.completed;
        std::fprintf(stderr,"PENDING_COMPLETED\n");
      }
    } else if(event->phase==DETSIM_EXECUTION_STOPPED) {
      if(event->result_valid || event->result || event->syscall_number!=-1)
        throw std::runtime_error("crash invented a syscall return");
    } else if(event->phase==DETSIM_THREAD_EXITED) {
      if(event->result_valid || event->result || b.at("alive")!=false)
        throw std::runtime_error("thread exit invented a syscall return");
    } else throw std::runtime_error("unknown event phase");
    if(event->syscall_number==SYS_exit_group) {
      if(event->phase!=DETSIM_THREAD_EXITED || event->args[0]!=0)
        throw std::runtime_error("pending fixture did not exit successfully");
      std::fprintf(stderr,"PENDING_EXIT blocked=%llu completed=%llu\n",
          static_cast<unsigned long long>(counts.blocked),static_cast<unsigned long long>(counts.completed));
    }
    return out.append(out.context,&counts,sizeof counts)?DETSIM_ERROR:DETSIM_PASS;
  } catch(const std::exception& error) {
    if(diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"%s",error.what());
    return DETSIM_VIOLATION;
  }
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),"test.pending",1,
  nullptr,nullptr,initial,advance,nullptr,nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2(){return &plugin;}
