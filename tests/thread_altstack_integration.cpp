#include "config.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/vector.hpp"
#include <sstream>
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "utils.h"
#include "state/state_transition.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static_assert(NP == 1);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
struct Job { long nr; uint64_t args[6]; long result; Job* child; };
static Job jobs[4]{};
alignas(16) static unsigned char stacks[3][16384];
struct AltStack { uint64_t sp; uint32_t flags; uint32_t padding; uint64_t size; };
static_assert(sizeof(AltStack)==24);
static AltStack input_stack{}, output_stack{};
static unsigned char* fault_pages;
static constexpr uint32_t autodisarm=0x80000000u;
extern "C" void altstack_guest(Job*);
asm(R"(
.text
.globl altstack_guest
.type altstack_guest,@function
altstack_guest:
 mov %rdi,%r12
1:
 mov 0(%r12),%rax
 mov %rax,%rbx
 mov 8(%r12),%rdi
 mov 16(%r12),%rsi
 mov 24(%r12),%rdx
 mov 32(%r12),%r10
 mov 40(%r12),%r8
 mov 48(%r12),%r9
 syscall
 cmp $56,%rbx
 jne 2f
 test %rax,%rax
 jnz 2f
 mov 64(%r12),%r12
 jmp 1b
2:
 mov %rax,56(%r12)
 jmp 1b
.size altstack_guest,.-altstack_guest
)");
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> static T read_guest(pid_t pid, const T* address) {
    T value{}; require(guest_read_exact(pid,address,&value,sizeof value),"guest read"); return value;
}
template<class T> static void write_guest(pid_t pid, T* address, const T& value) {
    require(guest_write_exact(pid,address,&value,sizeof value),"guest write");
}
static sys_state capture(const sys_state& source) {
    syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
    std::memcpy(info[0],source.child[0].si,sizeof info[0]);
    return sys_state(info,&source);
}
static sys_state step(const sys_state& source,int actor,int nr) {
    auto result=state_transition(source,0,actor);
    require(!result.property_violation && result.code!=CKPT_STOP && result.code!=CKPT_DISCARD,"transition rejected");
    require(result.new_state.child[0].si[actor].nr==nr,"unexpected syscall");
    return std::move(result.new_state);
}
static sys_state request(const sys_state& source,int actor,long nr,
                         std::array<uint64_t,6> args={},int child=-1) {
    source.recover_running_state();
    Job job=read_guest(ptmc_state.pids[0],&jobs[actor]);
    job.nr=nr;
    std::copy(args.begin(),args.end(),job.args);
    if(child>=0)job.child=&jobs[child];
    write_guest(ptmc_state.pids[0],&jobs[actor],job);
    return capture(source);
}
static void physical_disabled() {
    for(const auto& task:ptmc_state.running_state.child[0].threads) {
        if(!task.alive)continue;
        auto* address=reinterpret_cast<AltStack*>(scratch_page+2048);
        auto saved=read_guest(task.physical_tid,address);
        require(static_cast<int64_t>(tracee_do_syscall(task.physical_tid,SYS_sigaltstack,0,
                      reinterpret_cast<uint64_t>(address),0,0,0,0))==0,"physical stack query");
        auto physical=read_guest(task.physical_tid,address);
        write_guest(task.physical_tid,address,saved);
        require(physical.sp==0 && physical.size==0 && physical.flags==SS_DISABLE,
                "physical alternate stack was enabled");
    }
}
static void restore(const sys_state& expected) {
    sys_state loaded(expected.ss_hash);
    loaded.recover_running_state();
    physical_disabled();
    require(capture(loaded).ss_hash==expected.ss_hash,"altstack persisted identity drift");
}
static sys_state call(const sys_state& source,int actor,const AltStack* value,
                      uint64_t output=reinterpret_cast<uint64_t>(&output_stack)) {
    source.recover_running_state();
    if(value)write_guest(ptmc_state.pids[0],&input_stack,*value);
    auto changed=capture(source);
    return step(request(changed,actor,SYS_sigaltstack,
                       {value?reinterpret_cast<uint64_t>(&input_stack):0,output}),actor,SYS_sigaltstack);
}
static sys_state spawn(const sys_state& source,int parent,int child) {
    auto entry=request(source,parent,SYS_clone,
                       {0x50f00,reinterpret_cast<uint64_t>(stacks[child-1]+sizeof stacks[0]),0,0,0},child);
    auto result=step(entry,parent,SYS_clone);
    require(result.child[0].threads.size()==static_cast<size_t>(child+1),"clone slot missing");
    return result;
}
static void native_oracle() {
    const pid_t onstack_child=fork();require(onstack_child>=0,"native onstack fork");
    if(!onstack_child) {
        uint64_t rsp=0;asm volatile("mov %%rsp,%0":"=r"(rsp));
        AltStack inside{rsp-2048,0,0,4096},query{},bad{0,4,0,0};
        if(syscall(SYS_sigaltstack,&inside,nullptr))_exit(20);
        if(syscall(SYS_sigaltstack,nullptr,&query) || query.flags!=SS_ONSTACK)_exit(21);
        errno=0;
        if(syscall(SYS_sigaltstack,&bad,reinterpret_cast<void*>(1))!=-1 || errno!=EPERM)_exit(22);
        errno=0;
        if(syscall(SYS_sigaltstack,reinterpret_cast<void*>(1),nullptr)!=-1 || errno!=EFAULT)_exit(23);
        _exit(0);
    }
    int native_status=0;
    require(waitpid(onstack_child,&native_status,0)==onstack_child && WIFEXITED(native_status) &&
            !WEXITSTATUS(native_status),"native ONSTACK/input/EPERM ordering");
    AltStack original{};
    require(syscall(SYS_sigaltstack,nullptr,&original)==0,"native initial query");
    AltStack disabled{0,SS_DISABLE,0,0};
    require(syscall(SYS_sigaltstack,&disabled,nullptr)==0,"native disable");
    for(uint32_t flags:{0u,1u,2u,3u,autodisarm,autodisarm|1u,autodisarm|2u,4u}) {
        AltStack value{0x100000,flags,0xdeadbeef,2048}, old{};
        errno=0;long result=syscall(SYS_sigaltstack,&value,&old);
        require(result==((flags==3 || flags==4)?-1:0),"native flags oracle");
        if(result)require(errno==EINVAL,"native flags errno");
    }
    for(uint64_t size:{0u,2047u,2048u}) {
        AltStack value{0x100000,0,0,size};errno=0;
        long result=syscall(SYS_sigaltstack,&value,nullptr);
        require(result==(size<2048?-1:0) && (result==0 || errno==ENOMEM),"native minimum2048 profile mismatch");
    }
    AltStack value{0x200000,0,0,8192};errno=0;
    require(syscall(SYS_sigaltstack,&value,reinterpret_cast<void*>(1))==-1 && errno==EFAULT,
            "native old output fault");
    AltStack old{};require(syscall(SYS_sigaltstack,nullptr,&old)==0 && old.sp==value.sp,
                            "native output fault failed to commit registration");
    value={0x300000,SS_ONSTACK,0xdeadbeef,4096};
    require(syscall(SYS_sigaltstack,&value,&value)==0 && value.sp==old.sp && value.padding==0,
            "native alias and padding");
    auto* pages=static_cast<unsigned char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,
                                               MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    require(pages!=MAP_FAILED && mprotect(pages+4096,4096,PROT_NONE)==0,"native fault pages");
    require(syscall(SYS_sigaltstack,nullptr,&old)==0,"native current query");
    for(unsigned tail:{1u,4u,8u,9u,12u,16u,20u,23u,24u}) {
        std::memset(pages,0x5a,4096);errno=0;
        long result=syscall(SYS_sigaltstack,nullptr,pages+4096-tail);
        require(result==(tail==24?0:-1) && (result==0 || errno==EFAULT),"native partial output errno");
        require(std::memcmp(pages+4096-tail,&old,tail)==0,"native partial output bytes");
    }
    munmap(pages,8192);
    require(syscall(SYS_sigaltstack,&original,nullptr)==0,"native original restore");
}
static void run(bool tiny) {
    native_oracle();
    fault_pages=static_cast<unsigned char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,
                                               MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    require(fault_pages!=MAP_FAILED && mprotect(fault_pages+4096,4096,PROT_NONE)==0,"fault pages");
    for(auto& job:jobs)job.nr=SYS_getpid;
    char directory[]="/tmp/detsim-thread-altstack-XXXXXX";
    require(mkdtemp(directory),"mkdtemp");
    const auto previous=std::filesystem::current_path();
    require(chdir(directory)==0,"chdir");
    const pid_t pid=fork();require(pid>=0,"fork");
    if(!pid) {
        prctl(PR_SET_PDEATHSIG,SIGKILL);
        if(__rseq_size && syscall(SYS_rseq,static_cast<char*>(__builtin_thread_pointer())+__rseq_offset,
                                 32,RSEQ_FLAG_UNREGISTER,RSEQ_SIG))_exit(2);
        syscall(SYS_set_tid_address,nullptr);
        uint64_t empty=0;
        if(syscall(SYS_rt_sigprocmask,SIG_SETMASK,&empty,nullptr,8))_exit(3);
        if(mmap(reinterpret_cast<void*>(scratch_page),4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED)_exit(4);
        auto* code=reinterpret_cast<unsigned char*>(scratch_page+0xff0);code[0]=0x0f;code[1]=0x05;
        if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(91);
        stack_t disabled{}; disabled.ss_flags=SS_DISABLE;
        if(syscall(SYS_sigaltstack,&disabled,nullptr))_exit(92);
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr))_exit(5);
        asm volatile("int3");altstack_guest(&jobs[0]);_exit(6);
    }
    auto cleanup=[&] {
        kill(pid,SIGKILL);while(waitpid(-1,nullptr,__WALL)>0 || errno==EINTR){}
        StateStore::instance().shutdown();SysStateStore::instance().shutdown();
        std::filesystem::current_path(previous);std::filesystem::remove_all(directory);
    };
    try {
        int status=0;
        require(waitpid(pid,&status,__WALL)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==SIGTRAP,
                "initial breakpoint");
        require(ptrace(PTRACE_SETOPTIONS,pid,nullptr,
                       PTRACE_O_TRACECLONE|PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)==0,"ptrace options");
        ptmc_state.pids[0]=pid;ptmc_state.cursor=-1;
        ptmc_state.fd_managers[0]=std::make_shared<FdManager>();
        ptmc_state.fs_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        ptmc_state.sock_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        StateStore::Config config;if(tiny)config.hot_cache_size=config.warm_cache_size=1;
        StateStore::instance().init(config);auto_mode=1;
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};sys_state initial(info);
        ptmc_state.running_state=initial;thread::init_all(ptmc_state);exec::init_all();
        auto queried=call(initial,0,nullptr);
        require(queried.child[0].si[0].rval==0,"initial sigaltstack query failed");
        auto old=read_guest(pid,&output_stack);
        require(old.sp==0 && old.size==0 && old.flags==SS_DISABLE && old.padding==0,
                "initial altstack query differs from native");
        const AltStack registration{0x100000,0,0xdeadbeef,8192};
        auto registered=call(initial,0,&registration);
        require(registered.child[0].si[0].rval==0,"sigaltstack registration failed");
        auto query=call(registered,0,nullptr);
        old=read_guest(pid,&output_stack);
        require(old.sp==registration.sp && old.size==registration.size && old.flags==0 && !old.padding,
                "registered altstack query mismatch");
        restore(registered);
        require(registered.child[0].threads[0].altstack_sp==registration.sp &&
                registered.child[0].threads[0].altstack_size==8192 &&
                registered.child[0].threads[0].altstack_flags==0,"registration not semantic state");
        for(int field=0;field!=3;++field) {
            registered.recover_running_state();
            auto& task=ptmc_state.running_state.child[0].threads[0];
            if(field==0)++task.altstack_sp;
            if(field==1)++task.altstack_size;
            if(field==2)task.altstack_flags=autodisarm;
            auto branch=capture(registered);
            require(branch.ss_hash!=registered.ss_hash,"single altstack field omitted from identity");
            restore(branch);restore(registered);
        }
        AltStack disabled{0xdeadbeef,SS_DISABLE,0xaabbccdd,1};
        auto off=call(registered,0,&disabled);
        require(off.child[0].si[0].rval==0,"disable rejected ignored address/size");
        auto off_query=call(off,0,nullptr);
        old=read_guest(pid,&output_stack);
        require(!old.sp && !old.size && old.flags==SS_DISABLE && !old.padding,"disable query");
        for(uint32_t flags:{0u,1u,2u,3u,autodisarm,autodisarm|1u,autodisarm|2u,4u}) {
            AltStack value{0x200000,flags,0xffffffff,4096};
            auto result=call(registered,0,&value);
            require(static_cast<int64_t>(result.child[0].si[0].rval)==((flags==3 || flags==4)?-EINVAL:0),
                    "mode flags acceptance mismatch");
            auto next=call(result,0,nullptr);old=read_guest(pid,&output_stack);
            const bool invalid=flags==3 || flags==4, disarmed=(flags&3)==SS_DISABLE;
            require(old.sp==(invalid?registration.sp:(disarmed?0:value.sp)) &&
                    old.size==(invalid?registration.size:(disarmed?0:value.size)) &&
                    old.flags==(invalid?0:((flags&autodisarm)|(disarmed?uint32_t(SS_DISABLE):0u))) && !old.padding,
                    "mode flags normalization mismatch");
            restore(result);
        }
        for(int mode=0;mode!=5;++mode) {
            registered.recover_running_state();
            AltStack value{0x400000,mode==2?4u:0u,0xdeadbeef,mode==3?2047u:8192u};
            write_guest(pid,&input_stack,value);
            write_guest(pid,&output_stack,AltStack{0x55555555,0x55555555,0x55555555,0x55555555});
            auto entry=request(capture(registered),0,SYS_sigaltstack,
                  {mode==0?1:reinterpret_cast<uint64_t>(&input_stack),
                   mode==1?1:reinterpret_cast<uint64_t>(&output_stack)});
            auto result=step(entry,0,SYS_sigaltstack);
            const int64_t error=mode<2?-EFAULT:mode==2?-EINVAL:mode==3?-ENOMEM:0;
            require(static_cast<int64_t>(result.child[0].si[0].rval)==error,"failure precedence");
            require(result.child[0].threads[0].altstack_sp==(mode==1 || mode==4?value.sp:registration.sp),
                    "failure committed wrong stack");
            if(mode<4)require(read_guest(pid,&output_stack).flags==0x55555555,"failed input wrote old stack");
            restore(result);
        }
        auto too_small=registration;too_small.size=2048;
        require(call(initial,0,&too_small).child[0].si[0].rval==0,"raw2048 minimum rejected");
        registered.recover_running_state();
        AltStack alias_value{0x500000,autodisarm,0xdeadbeef,4096};
        write_guest(pid,&input_stack,alias_value);
        auto alias=step(request(capture(registered),0,SYS_sigaltstack,
                  {reinterpret_cast<uint64_t>(&input_stack),reinterpret_cast<uint64_t>(&input_stack)}),0,SYS_sigaltstack);
        old=read_guest(pid,&input_stack);
        require(alias.child[0].threads[0].altstack_sp==alias_value.sp && old.sp==registration.sp && !old.padding,
                "aliased copy ordering");
        for(unsigned tail:{1u,4u,8u,9u,12u,16u,20u,23u,24u}) {
            registered.recover_running_state();
            std::array<unsigned char,24> sentinel{};sentinel.fill(0x5a);
            require(guest_write_exact(pid,fault_pages+4096-tail,sentinel.data(),tail),"partial setup");
            auto result=call(capture(registered),0,nullptr,reinterpret_cast<uint64_t>(fault_pages+4096-tail));
            require(static_cast<int64_t>(result.child[0].si[0].rval)==(tail==24?0:-EFAULT),"partial copy error");
            AltStack expected{registration.sp,0,0,registration.size};
            std::array<unsigned char,24> actual{};
            require(guest_read_exact(pid,fault_pages+4096-tail,actual.data(),tail) &&
                    std::memcmp(actual.data(),&expected,tail)==0,"partial copy bytes differ from native");
            restore(result);
        }
        // Raw input copies all24 bytes before EPERM/flags/size and before output.
        for(unsigned tail:{1u,8u,12u,16u,23u}) {
            auto entry=request(registered,0,SYS_sigaltstack,
                               {reinterpret_cast<uint64_t>(fault_pages+4096-tail),1});
            auto result=step(entry,0,SYS_sigaltstack);
            require(static_cast<int64_t>(result.child[0].si[0].rval)==-EFAULT &&
                    result.child[0].threads[0].altstack_sp==registration.sp,"partial input committed state");
        }
        // Active guest RSP must determine ONSTACK; saved task.regs is stale at entry.
        registered.recover_running_state();user_regs_struct live{};
        require(ptrace(PTRACE_GETREGS,pid,nullptr,&live)==0,"active RSP");
        AltStack containing{live.rsp-1024,0,0,4096};
        auto inside=call(registered,0,&containing);
        auto inside_query=call(inside,0,nullptr);
        require(read_guest(pid,&output_stack).flags==SS_ONSTACK,"active RSP ONSTACK query");
        auto forbidden=call(inside,0,&disabled);
        require(static_cast<int64_t>(forbidden.child[0].si[0].rval)==-EPERM,"active stack modification accepted");
        auto automatic=containing;automatic.flags=autodisarm;
        auto auto_stack=call(registered,0,&automatic);
        auto auto_query=call(auto_stack,0,nullptr);
        require(read_guest(pid,&output_stack).flags==autodisarm,"AUTODISARM must suppress ONSTACK");
        require(call(auto_stack,0,&disabled).child[0].si[0].rval==0,"AUTODISARM change rejected");
        auto born=spawn(registered,0,1);
        require(born.child[0].threads[1].altstack_flags==SS_DISABLE && !born.child[0].threads[1].altstack_sp,
                "CLONE_VM child inherited alternate stack");
        auto worker_stack=registration;worker_stack.sp=0x600000;worker_stack.flags=autodisarm;
        auto worker=call(born,1,&worker_stack);
        require(worker.child[0].threads[0].altstack_sp==registration.sp,"worker mutated parent stack");
        auto exited=step(request(worker,1,SYS_exit),1,SYS_exit);
        require(!exited.child[0].threads[1].altstack_sp && !exited.child[0].threads[1].altstack_size &&
                exited.child[0].threads[1].altstack_flags==SS_DISABLE,"exit retained alternate stack");
        auto reused=spawn(exited,0,2);
        require(get_thread_list(pid).size()==2 && reused.child[0].threads[2].altstack_flags==SS_DISABLE &&
                !reused.child[0].threads[2].altstack_sp,"carrier reuse leaked alternate stack");
        for(const auto* saved:{&initial,&registered,&off,&alias,&inside,&auto_stack,&born,&worker,&exited,&reused})restore(*saved);
        for(int mode=0;mode!=5;++mode) {
            const auto& source=mode==4?exited:registered;
            const size_t slot=mode==4?1:0;
            auto malformed_task=source.child[0].threads[slot];
            if(mode==0)malformed_task.altstack_flags=4;
            if(mode==1){malformed_task.altstack_flags=SS_DISABLE;malformed_task.altstack_size=0;}
            if(mode==2){malformed_task.altstack_flags=SS_DISABLE;malformed_task.altstack_sp=0;}
            if(mode==3)malformed_task.altstack_size=2047;
            if(mode==4){malformed_task.altstack_flags=0;malformed_task.altstack_sp=0x700000;malformed_task.altstack_size=4096;}
            source.recover_running_state();
            ptmc_state.running_state.child[0].threads[slot]=malformed_task;
            const auto saves=StateStore::instance().get_stats().save_calls;
            bool capture_rejected=false;
            try{(void)capture(source);}catch(const std::runtime_error& e){
                capture_rejected=std::string(e.what()).find("Invalid logical alternate signal stack")!=std::string::npos;
            }
            require(capture_rejected && StateStore::instance().get_stats().save_calls==saves,
                    "malformed alternate stack captured or published");
            restore(source);
            std::ostringstream encoded;
            {cereal::BinaryOutputArchive archive(encoded);archive(malformed_task);}
            thread_state decoded;std::istringstream bytes(encoded.str());
            {cereal::BinaryInputArchive archive(bytes);archive(decoded);}
            auto malformed=source;malformed.child[0].threads[slot]=decoded;
            bool restore_rejected=false;
            try{malformed.recover_running_state();}catch(const std::runtime_error& e){
                restore_rejected=std::string(e.what()).find("Invalid logical alternate signal stack")!=std::string::npos;
            }
            require(restore_rejected && capture(source).ss_hash==source.ss_hash,
                    "malformed archived alternate stack restored or changed running state");
        }
        restore(reused); // Same-hash restoration also preserves the physical disabled invariant.
        auto replay=call(sys_state(born.ss_hash),1,&worker_stack);
        require(replay.ss_hash==worker.ss_hash,"persisted worker transition diverged");
        cleanup();
    }catch(...){cleanup();throw;}
}
int main(int argc,char** argv) {
    bool tiny=false;
    for(int i=1;i<argc;++i)tiny|=std::strcmp(argv[i],"--tiny-cache")==0;
    loglevel=4;pid_t test=fork();
    if(!test){setpgid(0,0);try{run(tiny);_exit(0);}catch(const std::exception& e){
        std::fprintf(stderr,"thread altstack: %s\n",e.what());_exit(1);}}
    if(test<0)return 1;
    setpgid(test,test);int status=0;bool done=false;
    for(int i=0;i<6000;++i){if(waitpid(test,&status,WNOHANG)==test){done=true;break;}usleep(10000);}
    kill(-test,SIGKILL);if(!done)waitpid(test,&status,0);
    bool ok=done && WIFEXITED(status) && WEXITSTATUS(status)==0;
    std::printf("%s task alternate stacks, ABI errors, clone, exit and carrier restore\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
