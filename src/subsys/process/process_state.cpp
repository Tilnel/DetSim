#include "process_state.h"
#include "monitor.h"
#include "guest.h"
#include "engine/thread_pool.h"
#include "engine/thread_control.h"
#include "cereal/archives/binary.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <limits>
#include <array>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <vector>

namespace {
constexpr uint64_t action_flags=0xdc000007ULL;
constexpr uint64_t unmaskable=(1ULL<<(SIGKILL-1))|(1ULL<<(SIGSTOP-1));
bool valid_action(int signal,const KernelSigaction& action) {
    return signal>=1 && signal<=64 && signal!=SIGKILL && signal!=SIGSTOP &&
        !(action.flags&~action_flags) && !(action.mask&unmaskable) && action!=KernelSigaction{};
}
void validate_clock(int clock) {
    switch(clock) {
    case CLOCK_REALTIME:case CLOCK_MONOTONIC:case CLOCK_REALTIME_COARSE:case CLOCK_MONOTONIC_COARSE:return;
    default:throw std::runtime_error("Unsupported modeled clock ID");
    }
}
bool valid_timer(const timeval& value) {
    return value.tv_sec>=0 && value.tv_usec>=0 && value.tv_usec<1000000;
}
uint64_t random_word(uint64_t& state) {
    state^=state>>12;state^=state<<25;state^=state>>27;
    return state*2685821657736338717ULL;
}
}

template<class Archive> void ProcessKernelState::serialize(Archive& ar) {
    uint64_t seed=random_state;
    uint32_t count=signal_actions.size();
    ar(seed,count);
    if(!seed || count>62) throw std::runtime_error("Invalid process kernel state archive");
    if constexpr(Archive::is_loading::value) {
        std::map<int,KernelSigaction> actions;
        for(uint32_t i=0;i<count;++i) {
            int signal;KernelSigaction action;
            ar(signal,action.handler,action.flags,action.restorer,action.mask);
            if(!valid_action(signal,action) || !actions.emplace(signal,action).second)
                throw std::runtime_error("Invalid signal action archive");
        }
        random_state=seed;signal_actions=std::move(actions);
    } else {
        for(const auto& [signal,action]:signal_actions) {
            if(!valid_action(signal,action)) throw std::runtime_error("Invalid signal action state");
            ar(signal,action.handler,action.flags,action.restorer,action.mask);
        }
    }
}
template void ProcessKernelState::serialize(cereal::BinaryInputArchive&);
template void ProcessKernelState::serialize(cereal::BinaryOutputArchive&);

void capture_process_kernel_state(int pid,int node) {
    if(node<0 || node>=NP) throw std::runtime_error("Invalid kernel process capture node");
    size_t threads=0;
    for(const auto& entry:std::filesystem::directory_iterator("/proc/"+std::to_string(pid)+"/task")) {
        (void)entry;
        if(++threads>1) throw std::runtime_error("Already multithreaded startup boundaries are unsupported");
    }
    if(threads!=1) throw std::runtime_error("Cannot inspect startup process thread set");
    adopt_thread_signal_mask(node,pid);
    ProcessKernelState captured;
    const uintptr_t address=scratch_page+0x800;
    std::array<unsigned char,128> original{};
    if(!guest_read_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
        throw std::runtime_error("Cannot preserve kernel capture scratch bytes");
    auto query=[&](int nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
        const long result=tracee_do_syscall(pid,nr,a,b,c,d,0,0);
        if(result<0) throw std::runtime_error("Cannot query inherited process kernel state");
    };
    auto read=[&](auto& value) {
        if(!guest_read_exact(pid,reinterpret_cast<void*>(address),&value,sizeof(value)))
            throw std::runtime_error("Cannot read inherited process kernel state");
    };
    try {
        for(int signal=1;signal<=64;++signal) {
            query(SYS_rt_sigaction,signal,0,address,8);
            KernelSigaction action{};read(action);
            if(action!=KernelSigaction{}) {
                if(!valid_action(signal,action)) throw std::runtime_error("Unsupported inherited signal disposition");
                captured.signal_actions.emplace(signal,action);
            }
        }
        uint64_t mask=0;
        query(SYS_rt_sigprocmask,SIG_SETMASK,0,address,8);read(mask);
        if(mask!=ptmc_state.running_state.child[node].threads.front().signal_mask)
            throw std::runtime_error("Bootstrap signal mask changed during capture");
        query(SYS_rt_sigpending,address,8,0,0);read(mask);
        if(mask) throw std::runtime_error("Inherited pending signals are unsupported");
        for(int timer=ITIMER_REAL;timer<=ITIMER_PROF;++timer) {
            query(SYS_getitimer,timer,address,0,0);
            itimerval value{};read(value);
            if(value.it_interval.tv_sec || value.it_interval.tv_usec || value.it_value.tv_sec || value.it_value.tv_usec)
                throw std::runtime_error("Inherited interval timers are unsupported");
        }
        query(SYS_sigaltstack,0,address,0,0);
        stack_t stack{};read(stack);
        if(!(stack.ss_flags&SS_DISABLE)) throw std::runtime_error("Inherited alternate signal stacks are unsupported");
    } catch(...) {
        if(!guest_write_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
            throw std::runtime_error("Cannot restore kernel capture scratch bytes after failure");
        throw;
    }
    if(!guest_write_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
        throw std::runtime_error("Cannot restore kernel capture scratch bytes");
    std::ifstream status("/proc/"+std::to_string(pid)+"/status");
    std::string line;unsigned creation_mask=0;bool found=false;
    while(std::getline(status,line)) {
        if(line.starts_with("Umask:")) {
            std::istringstream input(line.substr(6));input>>std::oct>>creation_mask;
            if(!input || creation_mask>0777) throw std::runtime_error("Invalid inherited process umask");
            found=true;
        }
    }
    if(!found) throw std::runtime_error("Cannot read inherited process umask");
    require_no_pending_signals(pid);
    ptmc_state.process_states[node]=std::move(captured);
    ptmc_state.fs_states[node].set_creation_mask(creation_mask);
}

long emu_process_syscall(const syscall_info& info) {
    const int node=ptmc_state.cursor;
    const pid_t pid=ptmc_state.get_current_tid(node);
    auto& state=ptmc_state.process_states[node];
    const auto& now=ptmc_state.time[node];
    auto write=[&](uint64_t address,const auto& value) {
        return guest_write_exact(pid,reinterpret_cast<void*>(address),&value,sizeof(value));
    };
    auto read=[&](uint64_t address,auto& value) {
        return guest_read_exact(pid,reinterpret_cast<const void*>(address),&value,sizeof(value));
    };
    switch(info.nr) {
    case SYS_clock_gettime:case SYS_clock_getres: {
        validate_clock(static_cast<int>(info.args[0]));
        const timespec value=info.nr==SYS_clock_getres ? timespec{0,1000} : timespec{now.tv_sec,now.tv_usec*1000};
        if(info.nr==SYS_clock_getres && !info.args[1]) return 0;
        return write(info.args[1],value)?0:-EFAULT;
    }
    case SYS_time: {
        const time_t value=now.tv_sec;
        return !info.args[0] || write(info.args[0],value)?value:-EFAULT;
    }
    case SYS_getrandom: {
        const unsigned flags=info.args[2];
        if((flags&~(GRND_NONBLOCK|GRND_RANDOM|GRND_INSECURE)) ||
           ((flags&GRND_RANDOM) && (flags&GRND_INSECURE))) return -EINVAL;
        // Linux permits partial results; bound staging memory per transition.
        const size_t count=std::min<uint64_t>(info.args[1],256*1024);
        if(!count) return 0;
        std::vector<unsigned char> bytes(count);
        uint64_t seed=state.random_state;
        for(size_t i=0;i<count;) {
            const uint64_t word=random_word(seed);
            for(unsigned j=0;j<8 && i<count;++j,++i) bytes[i]=word>>(j*8);
        }
        if(!guest_write_exact(pid,reinterpret_cast<void*>(info.args[0]),bytes.data(),bytes.size())) return -EFAULT;
        state.random_state=seed;
        return count;
    }
    case SYS_sysinfo: {
        struct sysinfo value{};
        value.uptime=now.tv_sec;value.totalram=1ULL<<30;value.freeram=1ULL<<29;
        value.procs=NP;value.mem_unit=1;
        return write(info.args[0],value)?0:-EFAULT;
    }
    case SYS_rt_sigaction: {
        const int signal=info.args[0];
        if(signal<1 || signal>64 || info.args[3]!=8) return -EINVAL;
        KernelSigaction action{};
        if(info.args[1]) {
            if(signal==SIGKILL || signal==SIGSTOP) return -EINVAL;
            if(!read(info.args[1],action)) return -EFAULT;
            if(action.flags&~action_flags) throw std::runtime_error("Unsupported signal action flags");
            action.mask&=~unmaskable;
        }
        const auto found=state.signal_actions.find(signal);
        const KernelSigaction old=found==state.signal_actions.end()?KernelSigaction{}:found->second;
        if(info.args[2] && !write(info.args[2],old)) return -EFAULT;
        if(info.args[1]) {
            if(action==KernelSigaction{}) state.signal_actions.erase(signal);
            else state.signal_actions[signal]=action;
        }
        return 0;
    }
    case SYS_setitimer:case SYS_getitimer: {
        if(info.args[0]>ITIMER_PROF) return -EINVAL;
        itimerval value{};
        if(info.nr==SYS_getitimer) return write(info.args[1],value)?0:-EFAULT;
        if(info.args[1] && !read(info.args[1],value)) return -EFAULT;
        if(!valid_timer(value.it_interval) || !valid_timer(value.it_value)) return -EINVAL;
        if(value.it_interval.tv_sec || value.it_interval.tv_usec || value.it_value.tv_sec || value.it_value.tv_usec)
            throw std::runtime_error("Armed interval timers are unsupported");
        return !info.args[2] || write(info.args[2],value)?0:-EFAULT;
    }
    case SYS_umask: {
        auto& fs=ptmc_state.fs_states[node];
        const auto previous=fs.creation_mask();
        fs.set_creation_mask(info.args[0]);return previous;
    }
    default:throw std::runtime_error("Unknown process kernel model syscall");
    }
}
