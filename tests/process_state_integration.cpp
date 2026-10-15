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
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static int failures;
static KernelResourceLimit guest_resource_output;
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
static void check_resource_queries() {
    auto& state=ptmc_state.process_states[0];
    const auto before=state;
    for(unsigned resource=0;resource<16;++resource) {
        KernelResourceLimit native{},legacy{},modeled{};
        check(syscall(SYS_prlimit64,0,resource,nullptr,&native)==0 &&
              syscall(SYS_getrlimit,resource,&legacy)==0 && native==legacy,
              "native getrlimit and prlimit64 agree for all sixteen resources");
        state.resource_limits[resource]=native;
        check(call(SYS_getrlimit,resource,reinterpret_cast<uint64_t>(&modeled))==0 && modeled==native &&
              call(SYS_prlimit64,0,resource,0,reinterpret_cast<uint64_t>(&modeled))==0 && modeled==native,
              "modeled resource queries expose the same captured table");
    }
    KernelResourceLimit value{};
    const auto target=reinterpret_cast<uint64_t>(&value);
    for(uint64_t resource:{uint64_t(16),uint64_t(-1),uint64_t(0x100000010ULL)}) {
        errno=0;
        check(syscall(SYS_getrlimit,resource,nullptr)==-1 && errno==EINVAL,
              "native invalid resource precedes bad output pointer");
        errno=0;
        check(syscall(SYS_prlimit64,0,resource,nullptr,nullptr)==-1 && errno==EINVAL,
              "native prlimit null output still validates resource");
        check(call(SYS_getrlimit,resource,0)==-EINVAL && call(SYS_prlimit64,0,resource,0,0)==-EINVAL,
              "modeled invalid resource precedes pointer handling");
    }
    errno=0;
    check(syscall(SYS_getrlimit,RLIMIT_NOFILE,nullptr)==-1 && errno==EFAULT &&
          syscall(SYS_prlimit64,0,RLIMIT_NOFILE,nullptr,nullptr)==0,
          "native resource query null pointer rules");
    check(syscall(SYS_getrlimit,0x100000000ULL|RLIMIT_NOFILE,&value)==0 &&
          value==state.resource_limits[RLIMIT_NOFILE] &&
          syscall(SYS_prlimit64,0xffffffff00000000ULL,0x100000000ULL|RLIMIT_NOFILE,nullptr,&value)==0 &&
          syscall(SYS_prlimit64,0x100000000ULL|uint64_t(getpid()),RLIMIT_NOFILE,nullptr,&value)==0,
          "native resource and PID queries use their low32 ABI");
    check(call(SYS_getrlimit,RLIMIT_NOFILE,0)==-EFAULT &&
          call(SYS_prlimit64,0,RLIMIT_NOFILE,0,0)==0,"resource query null pointer rules");
    check(call(SYS_getrlimit,0x100000000ULL|RLIMIT_NOFILE,target)==0 &&
          value==state.resource_limits[RLIMIT_NOFILE] &&
          call(SYS_prlimit64,0xffffffff00000000ULL,RLIMIT_NOFILE,0,target)==0 &&
          call(SYS_prlimit64,0x100000000ULL|uint64_t(logical_process_id(0)),RLIMIT_NOFILE,0,target)==0,
          "resource and logical self PID arguments use their low32 ABI");
    for(int nr:{SYS_getrlimit,SYS_prlimit64}) {
        auto* pages=static_cast<unsigned char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(pages==MAP_FAILED) throw std::runtime_error("resource partial copy fixture");
        memset(pages,0x5a,8192);
        if(mprotect(pages+4096,4096,PROT_READ)) throw std::runtime_error("resource partial copy protection");
        for(size_t prefix:{size_t(1),size_t(7),size_t(8),size_t(15)}) {
            auto* output=pages+4096-prefix;
            memset(output,0x5a,prefix);
            errno=0;
            const long result=nr==SYS_getrlimit ? syscall(nr,RLIMIT_NOFILE,output) :
                syscall(nr,0,RLIMIT_NOFILE,nullptr,output);
            std::array<unsigned char,16> oracle{};memcpy(oracle.data(),output,oracle.size());
            const bool native_fault=result==-1 && errno==EFAULT;
            memset(output,0x5a,prefix);
            const long model=nr==SYS_getrlimit ? call(nr,RLIMIT_NOFILE,reinterpret_cast<uint64_t>(output)) :
                call(nr,0,RLIMIT_NOFILE,0,reinterpret_cast<uint64_t>(output));
            check(native_fault && model==-EFAULT && !memcmp(output,oracle.data(),oracle.size()),
                  "resource partial copy retains native writable prefix on EFAULT");
        }
        munmap(pages,8192);
    }
    const auto encoded=archive(state);
    auto changed=state;
    --changed.resource_limits[RLIMIT_NOFILE].current;
    check(changed!=state && archive(changed)!=encoded,"resource-only change enters process identity without aliasing");
    tracee_state original;original.process_state=state;
    tracee_state different=original;different.process_state=changed;
    check(!original.metadata_equal(different),"resource-only change enters tracee metadata identity");
    ProcessKernelState restored;
    {std::istringstream input(encoded);cereal::BinaryInputArchive a(input);a(restored);}
    check(restored==state && archive(restored)==encoded,"all resource limits survive archive round trip");
    std::string malformed=encoded;
    const KernelResourceLimit invalid{1,0};
    memcpy(malformed.data()+malformed.size()-16*16,&invalid,sizeof(invalid));
    const auto unchanged=restored;
    bool rejected=false;
    try {std::istringstream input(malformed);cereal::BinaryInputArchive a(input);a(restored);}
    catch(const std::runtime_error&) {rejected=true;}
    check(rejected && restored==unchanged,"invalid resource cur above max rejects archive atomically");
    state=before;
}

int main() {
    ptmc_state.cursor=0;ptmc_state.pids[0]=getpid();
    ptmc_state.process_states[0]=ProcessKernelState{};
    ptmc_state.time[0]={123,456789};
    check_resource_queries();
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
        stack_t disabled{};disabled.ss_flags=static_cast<int>(0x80000002u);
        if(syscall(SYS_sigaltstack,&disabled,nullptr))_exit(4);
        if(mmap(reinterpret_cast<void*>(scratch_page),4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED) _exit(2);
        auto* code=reinterpret_cast<unsigned char*>(scratch_page+0xff0);code[0]=0x0f;code[1]=0x05;
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(3);
        asm volatile("int3");_exit(0);
    }
    bool bootstrap_ok=false,mask_adopted=false,pending_rejected=false;
    bool altstack_adopted=false,enabled_altstack_rejected=false;
    if(child>0) {
        int status=0;
        if(waitpid(child,&status,0)==child && WIFSTOPPED(status) &&
           ptrace(PTRACE_SETOPTIONS,child,nullptr,PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)==0) {
            try {
                ptmc_state.pids[0]=child;
                std::array<unsigned char,128> scratch_before{},scratch_after{};
                scratch_before.fill(0x6b);
                guest_write_exact(child,reinterpret_cast<void*>(scratch_page+0x800),scratch_before.data(),scratch_before.size());
                capture_process_kernel_state(child,0);
                check(guest_read_exact(child,reinterpret_cast<void*>(scratch_page+0x800),scratch_after.data(),scratch_after.size()) &&
                      scratch_after==scratch_before,"resource capture preserves injected scratch bytes");
                const auto captured=ptmc_state.process_states[0];
                for(unsigned resource=0;resource<16;++resource) {
                    KernelResourceLimit native{};
                    check(tracee_do_syscall(child,SYS_prlimit64,0,resource,0,scratch_page+0x900,0,0)==0 &&
                          guest_read_exact(child,reinterpret_cast<void*>(scratch_page+0x900),&native,sizeof(native)) &&
                          captured.resource_limits[resource]==native,"bootstrap imports each native resource limit");
                }
                auto changed_limit=captured.resource_limits[RLIMIT_NOFILE];
                if(!changed_limit.current) throw std::runtime_error("resource mutation fixture has zero soft limit");
                --changed_limit.current;
                guest_write_exact(child,reinterpret_cast<void*>(scratch_page+0x900),&changed_limit,sizeof(changed_limit));
                check(tracee_do_syscall(child,SYS_prlimit64,0,RLIMIT_NOFILE,scratch_page+0x900,0,0,0)==0,
                      "source host limit changes after capture");
                KernelResourceLimit modeled{};
                check(call(SYS_getrlimit,RLIMIT_NOFILE,reinterpret_cast<uint64_t>(&guest_resource_output))==0 &&
                      guest_read_exact(child,&guest_resource_output,&modeled,sizeof(modeled)) &&
                      modeled==captured.resource_limits[RLIMIT_NOFILE] && modeled!=changed_limit &&
                      ptmc_state.process_states[0]==captured,"post-capture host limit cannot leak into modeled query");
                std::array<unsigned char,4096> page_before{},page_after{};
                check(guest_read_exact(child,reinterpret_cast<void*>(scratch_page),page_before.data(),page_before.size()),
                      "read reserved page before resource queries");
                user_regs_struct registers_before{},registers_after{};
                if(ptrace(PTRACE_GETREGS,child,nullptr,&registers_before)) throw std::runtime_error("resource registers");
                for(int nr:{SYS_getrlimit,SYS_prlimit64}) {
                    for(uint64_t destination:{scratch_page+0x100,scratch_page+0xff0,scratch_page-8}) {
                        bool rejected=false;
                        try {
                            if(nr==SYS_getrlimit) call(nr,RLIMIT_NOFILE,destination);
                            else call(nr,0,RLIMIT_NOFILE,0,destination);
                        } catch(const std::runtime_error&) {rejected=true;}
                        check(rejected && guest_read_exact(child,reinterpret_cast<void*>(scratch_page),page_after.data(),page_after.size()) &&
                              page_after==page_before && ptmc_state.process_states[0]==captured &&
                              ptrace(PTRACE_GETREGS,child,nullptr,&registers_after)==0 &&
                              !memcmp(&registers_before,&registers_after,sizeof(registers_before)),
                              "resource queries reject reserved overlap without page register or model mutation");
                        check((nr==SYS_getrlimit ? call(nr,16,destination) : call(nr,0,16,0,destination))==-EINVAL,
                              "invalid resource precedes reserved output overlap");
                    }
                }
                bootstrap_ok=ptmc_state.process_states[0].signal_actions.at(SIGINT).handler==1 &&
                    ptmc_state.fs_states[0].creation_mask()==0077;
                const auto& bootstrap_task=ptmc_state.running_state.child[0].threads.front();
                altstack_adopted=bootstrap_task.altstack_sp==0 && bootstrap_task.altstack_size==0 &&
                    bootstrap_task.altstack_flags==0x80000002u;
                stack_t enabled{};enabled.ss_sp=reinterpret_cast<void*>(0x100000);enabled.ss_size=8192;
                guest_write_exact(child,reinterpret_cast<void*>(scratch_page+0x900),&enabled,sizeof(enabled));
                if(static_cast<int64_t>(tracee_do_syscall(child,SYS_sigaltstack,scratch_page+0x900,0,0,0,0,0))==0) {
                    try {capture_process_kernel_state(child,0);}
                    catch(const std::runtime_error& e) {
                        enabled_altstack_rejected=std::string(e.what()).find("Inherited alternate signal stacks")!=std::string::npos;
                    }
                }
                stack_t disabled{};disabled.ss_flags=static_cast<int>(0x80000002u);
                guest_write_exact(child,reinterpret_cast<void*>(scratch_page+0x900),&disabled,sizeof(disabled));
                tracee_do_syscall(child,SYS_sigaltstack,scratch_page+0x900,0,0,0,0,0);
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
    check(altstack_adopted,"native bootstrap imports disabled AUTODISARM alternate stack flags");
    check(enabled_altstack_rejected,"native bootstrap rejects enabled alternate stack");
    check(mask_adopted,"native bootstrap adopts inherited blocked mask");
    check(pending_rejected,"native bootstrap rejects inherited pending signals");
    printf("process kernel model: %d failures\n",failures);return failures?1:0;
}
