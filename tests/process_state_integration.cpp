#include "process/process_state.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "engine/thread_pool.h"
#include "signal_handler.h"
#include "cereal/archives/binary.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <cstdio>
#include <sstream>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static int failures;
static void check(bool value, const char* name) {
    printf("%s %s\n", value ? "PASS" : "FAIL", name);
    failures += !value;
}
static long call(int nr, uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    syscall_info info{}; info.nr=nr;info.args[0]=a;info.args[1]=b;info.args[2]=c;info.args[3]=d;
    return emu_process_syscall(info);
}
static std::string archive(ProcessKernelState& state) {
    std::ostringstream output; {cereal::BinaryOutputArchive a(output);a(state);} return output.str();
}
int main() {
    ptmc_state.cursor=0;ptmc_state.pids[0]=getpid();
    ptmc_state.process_states[0]=ProcessKernelState{};
    ptmc_state.time[0]={123,456789};
    auto original=ptmc_state.process_states[0];
    std::array<unsigned char,40> bytes{},again{};
    check(call(SYS_getrandom,reinterpret_cast<uint64_t>(bytes.data()),bytes.size(),0)==40,"random returns bytes");
    const auto successor=ptmc_state.process_states[0];
    check(successor!=original,"random advances captured stream");
    ptmc_state.process_states[0]=original;
    check(call(SYS_getrandom,reinterpret_cast<uint64_t>(again.data()),again.size(),GRND_NONBLOCK)==40 && bytes==again && ptmc_state.process_states[0]==successor,"random branch replay matches");
    check(call(SYS_getrandom,1,40,0)==-EFAULT && ptmc_state.process_states[0]==successor,"failed random copy preserves generator");
    check(call(SYS_getrandom,0,0,0)==0,"empty random needs no buffer");
    check(call(SYS_getrandom,0,0,0x80)==-EINVAL,"unknown random flags fail");
    KernelSigaction action{0x1234,0x10000000,0,0},old{};
    check(call(SYS_rt_sigaction,SIGUSR1,reinterpret_cast<uint64_t>(&action),reinterpret_cast<uint64_t>(&old),8)==0 && old==KernelSigaction{},"sigaction saves disposition and reports old default");
    const auto installed=ptmc_state.process_states[0];
    check(call(SYS_rt_sigaction,SIGUSR1,reinterpret_cast<uint64_t>(&action),1,8)==-EFAULT && ptmc_state.process_states[0]==installed,"failed old-action copy does not commit");
    check(call(SYS_rt_sigaction,SIGKILL,reinterpret_cast<uint64_t>(&action),0,8)==-EINVAL,"SIGKILL cannot be caught");
    check(call(SYS_rt_sigaction,SIGKILL,0,reinterpret_cast<uint64_t>(&old),8)==0 && old==KernelSigaction{},"SIGKILL disposition can be queried");
    check(call(SYS_rt_sigaction,SIGUSR1,0,0,128)==-EINVAL,"kernel signal-set size enforced");
    const auto encoded=archive(ptmc_state.process_states[0]);
    ProcessKernelState restored;std::istringstream input(encoded);{cereal::BinaryInputArchive a(input);a(restored);}
    check(restored==installed && archive(restored)==encoded,"process kernel state archive canonical round trip");
    auto copy=restored;copy.signal_actions.erase(SIGUSR1);
    check(restored==installed && copy!=restored,"process kernel state copy isolates branches");
    auto malformed=encoded;std::memset(malformed.data(),0,sizeof(uint64_t));
    bool bad_archive=false;
    try {std::istringstream bad(malformed);cereal::BinaryInputArchive a(bad);a(copy);}
    catch(const std::runtime_error&){bad_archive=true;}
    check(bad_archive && copy.random_state!=0,"zero random seed archive rejected without state mutation");
    action={};
    check(call(SYS_rt_sigaction,SIGUSR1,reinterpret_cast<uint64_t>(&action),0,8)==0 &&
          ptmc_state.process_states[0].signal_actions.empty(),"default signal dispositions have canonical absent encoding");
    timespec ts{};
    check(call(SYS_clock_gettime,CLOCK_MONOTONIC,reinterpret_cast<uint64_t>(&ts))==0 && ts.tv_sec==123 && ts.tv_nsec==456789000,"clock reads model time");
    check(call(SYS_clock_getres,CLOCK_REALTIME,reinterpret_cast<uint64_t>(&ts))==0 && ts.tv_sec==0 && ts.tv_nsec==1000,"clock resolution matches model microseconds");
    check(call(SYS_time,0)==123,"time reads model seconds");
    check(call(SYS_clock_gettime,CLOCK_REALTIME,1)==-EFAULT && call(SYS_time,1)==-EFAULT,"clock guest pointer failures are explicit");
    bool unsupported_clock=false;
    try {call(SYS_clock_gettime,CLOCK_PROCESS_CPUTIME_ID,reinterpret_cast<uint64_t>(&ts));}
    catch(const std::runtime_error&){unsupported_clock=true;}
    check(unsupported_clock,"unmodeled CPU clocks do not use host time");
    struct sysinfo si{};
    check(call(SYS_sysinfo,reinterpret_cast<uint64_t>(&si))==0 && si.totalram==(1ULL<<30) && si.mem_unit==1,"sysinfo uses deterministic machine profile");
    // Raw Linux ABI returns copied bytes, unlike libc's zero success value.
    auto native_affinity=[](long target,unsigned long size,void* mask) {
        errno=0;
        const long result=syscall(SYS_sched_getaffinity,target,size,mask);
        return result<0 ? -long(errno) : result;
    };
    std::array<uint64_t,128> host_mask{};
    const long host_bytes=native_affinity(0,sizeof(host_mask),host_mask.data());
    check(host_bytes>0 && host_bytes<=long(sizeof(host_mask)) && host_bytes%8==0,
          "native affinity returns whole-word copied byte count");
    check(native_affinity(-1,0,nullptr)==-EINVAL &&
          native_affinity(-1,sizeof(host_mask),nullptr)==-ESRCH &&
          native_affinity(0,sizeof(host_mask)-1,host_mask.data())==-EINVAL &&
          native_affinity(0,sizeof(host_mask),nullptr)==-EFAULT &&
          native_affinity(0,1UL<<29,nullptr)==-EINVAL,
          "native affinity validates size then target then copy");
    auto& tasks=ptmc_state.running_state.child[0].threads;
    thread_state affinity_main{};affinity_main.tid=1;affinity_main.physical_tid=getpid();affinity_main.alive=true;
    thread_state affinity_peer{};affinity_peer.tid=2;affinity_peer.physical_tid=123456;affinity_peer.alive=true;
    tasks={affinity_main,affinity_peer};
    auto affinity=[&](uint64_t target,uint64_t size,uint64_t buffer) {
        try {return call(SYS_sched_getaffinity,target,size,buffer);}
        catch(const std::runtime_error&) {return -long(ENOSYS);}
    };
    std::array<uint64_t,2> mask{0,0xfeedbeef};
    const auto mask_address=reinterpret_cast<uint64_t>(mask.data());
    check(affinity(0,sizeof(mask),mask_address)==8 && mask[0]==1 && mask[1]==0xfeedbeef,
          "affinity exposes only virtual CPU0 and preserves bytes past kernel mask");
    check(affinity(logical_process_id(0),8,mask_address)==8 &&
          affinity(logical_thread_id(0,2),8,mask_address)==8,
          "affinity accepts logical process and sibling task IDs");
    tasks[1].alive=false;
    check(affinity(logical_thread_id(0,2),8,mask_address)==-ESRCH &&
          affinity(123456,8,mask_address)==-ESRCH && affinity(-1,8,mask_address)==-ESRCH,
          "affinity rejects dead tasks unknown IDs and physical carrier IDs");
    check(affinity(-1,0,0)==-EINVAL && affinity(-1,8,0)==-ESRCH &&
          affinity(0,8,0)==-EFAULT && affinity(0,7,mask_address)==-EINVAL &&
          affinity(0,9,mask_address)==-EINVAL,
          "affinity error precedence matches Linux");
    check(affinity(1ULL<<32,(1ULL<<32)+8,mask_address)==8 &&
          affinity(0,1ULL<<32,mask_address)==-EINVAL &&
          affinity(0,1ULL<<29,mask_address)==-EINVAL,
          "affinity uses signed pid32 and unsigned len32 including bit-count wrap");
    const auto process_before_affinity=archive(ptmc_state.process_states[0]);
    mask[0]=0;
    check(affinity(0,8,mask_address)==8 && mask[0]==1 &&
          archive(ptmc_state.process_states[0])==process_before_affinity,
          "affinity query has no mutable process state");
    if constexpr(NP>1) {
        auto& peer_tasks=ptmc_state.running_state.child[1].threads;
        peer_tasks={affinity_main};
        check(affinity(logical_process_id(1),8,mask_address)==8,
              "affinity recognizes live logical tasks in other modeled nodes");
        peer_tasks.clear();
    }
    tasks.clear();
    itimerval timer{},timerold{};
    check(call(SYS_setitimer,ITIMER_REAL,reinterpret_cast<uint64_t>(&timer),reinterpret_cast<uint64_t>(&timerold))==0 && timerold.it_value.tv_sec==0,"disabled interval timers are modeled");
    timer.it_value.tv_sec=1;bool rejected=false;
    try {call(SYS_setitimer,ITIMER_REAL,reinterpret_cast<uint64_t>(&timer),0);}catch(const std::runtime_error&){rejected=true;}
    check(rejected,"armed interval timers fail explicitly");
    timer.it_value.tv_sec=-1;
    check(call(SYS_setitimer,ITIMER_REAL,reinterpret_cast<uint64_t>(&timer),0)==-EINVAL,"invalid interval timer fails validation");
    check(call(SYS_umask,0077)==0022 && call(SYS_umask,0022)==0077,"umask is branch-owned virtual filesystem state");
    SignalHandler handler;SignalContext context;
    context.pid=getpid();context.signal=SIGSEGV;context.is_stopped=true;context.category=handler.classify(SIGSEGV);
    ptmc_state.process_states[0].signal_actions[SIGSEGV]=KernelSigaction{0x1234,0,0,0};
    bool rejected_handler=false;
    try {handler.handle(context);}catch(const std::runtime_error&){rejected_handler=true;}
    check(rejected_handler,"custom fatal-signal handlers fail before fatal classification");
    ptmc_state.process_states[0].signal_actions[SIGSEGV]=KernelSigaction{1,0,0,0};
    bool ignored_fault_rejected=false;
    try {handler.handle(context);}catch(const std::runtime_error&){ignored_fault_rejected=true;}
    check(ignored_fault_rejected,"ignored fatal fault delivery is explicitly unsupported");
    context.signal=SIGUSR1;context.category=handler.classify(SIGUSR1);
    ptmc_state.process_states[0].signal_actions[SIGUSR1]=KernelSigaction{1,0,0,0};
    const auto ignored=handler.handle(context);
    check(ignored.action==SignalAction::SPECIAL && ignored.should_retry,"captured ignored signal retries execution");
    ptmc_state.process_states[0].signal_actions.clear();bool default_rejected=false;
    try {handler.handle(context);}catch(const std::runtime_error&){default_rejected=true;}
    check(default_rejected && handler.classify(SIGSEGV)==SignalCategory::FATAL,"default signal policy retains explicit rejection and fatal classification");

    const pid_t child=fork();
    if(child==0) {
        signal(SIGINT,SIG_IGN);umask(0077);
        if(mmap(reinterpret_cast<void*>(scratch_page),4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED) _exit(2);
        auto* code=reinterpret_cast<unsigned char*>(scratch_page+0xff0);code[0]=0x0f;code[1]=0x05;
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(3);
        asm volatile("int3");_exit(0);
    }
    bool bootstrap_ok=false,mask_adopted=false,pending_rejected=false;
    if(child>0) {
        int status=0;
        if(waitpid(child,&status,0)==child && WIFSTOPPED(status) &&
           ptrace(PTRACE_SETOPTIONS,child,nullptr,PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)==0) {
            try {
                ptmc_state.pids[0]=child;
                capture_process_kernel_state(child,0);
                bootstrap_ok=ptmc_state.process_states[0].signal_actions.at(SIGINT).handler==1 &&
                    ptmc_state.fs_states[0].creation_mask()==0077;
                const uint64_t mask=1ULL<<(SIGUSR1-1);
                guest_write_exact(child,reinterpret_cast<void*>(scratch_page+0x900),&mask,sizeof(mask));
                tracee_do_syscall(child,SYS_rt_sigprocmask,SIG_BLOCK,scratch_page+0x900,0,8,0,0);
                capture_process_kernel_state(child,0);
                mask_adopted=ptmc_state.running_state.child[0].threads.front().signal_mask==mask;
                kill(child,SIGUSR1);
                try {capture_process_kernel_state(child,0);}catch(const std::runtime_error&){pending_rejected=true;}
            } catch(const std::exception& e) {fprintf(stderr,"bootstrap fixture: %s\n",e.what());}
        }
        kill(child,SIGKILL);waitpid(child,&status,0);
    }
    check(bootstrap_ok,"native bootstrap imports inherited dispositions and umask");
    check(mask_adopted,"native bootstrap adopts inherited blocked mask");
    check(pending_rejected,"native bootstrap rejects inherited pending signals");
    printf("process kernel model: %d failures\n",failures);return failures?1:0;
}
