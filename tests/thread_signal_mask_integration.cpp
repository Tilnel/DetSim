#include "config.h"
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
static uint64_t mask_word, old_word;
static constexpr uint64_t usr1 = uint64_t{1} << (SIGUSR1-1);
static constexpr uint64_t usr2 = uint64_t{1} << (SIGUSR2-1);
extern "C" void mask_guest(Job*);
asm(R"(
.text
.globl mask_guest
.type mask_guest,@function
mask_guest:
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
.size mask_guest,.-mask_guest
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
static uint64_t physical_mask(pid_t tid) {
    uint64_t value=0;
    require(ptrace(PTRACE_GETSIGMASK,tid,reinterpret_cast<void*>(sizeof value),&value)==0,
            "PTRACE_GETSIGMASK");
    return value;
}
static void masks(const sys_state& expected) {
    const auto& live=ptmc_state.running_state.child[0].threads;
    for(size_t i=0;i<expected.child[0].threads.size();++i) {
        const auto& saved=expected.child[0].threads[i];
        if(saved.alive)require(physical_mask(live.at(i).physical_tid)==saved.signal_mask,
                             "logical/physical signal masks differ");
    }
}
static void restore(const sys_state& expected) {
    sys_state loaded(expected.ss_hash);
    loaded.recover_running_state();
    masks(loaded);
    require(capture(loaded).ss_hash==expected.ss_hash,"signal mask persisted identity drift");
}
static sys_state set_mask(const sys_state& source,int actor,uint64_t value) {
    source.recover_running_state();
    write_guest(ptmc_state.pids[0],&mask_word,value);
    auto changed=capture(source);
    auto entry=request(changed,actor,SYS_rt_sigprocmask,
                       {SIG_SETMASK,reinterpret_cast<uint64_t>(&mask_word),0,8});
    auto result=step(entry,actor,SYS_rt_sigprocmask);
    require(result.child[0].si[actor].rval==0,"set mask failed");
    masks(result);
    return result;
}
static sys_state spawn(const sys_state& source,int parent,int child) {
    auto entry=request(source,parent,SYS_clone,
                       {0x50f00,reinterpret_cast<uint64_t>(stacks[child-1]+sizeof stacks[0]),0,0,0},child);
    auto result=step(entry,parent,SYS_clone);
    require(result.child[0].threads.size()==static_cast<size_t>(child+1),"clone slot missing");
    masks(result);
    return result;
}
static void run(bool tiny,bool shared_pending) {
    for(auto& job:jobs)job.nr=SYS_getpid;
    char directory[]="/tmp/detsim-thread-mask-XXXXXX";
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
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr))_exit(5);
        asm volatile("int3");mask_guest(&jobs[0]);_exit(6);
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
        require(initial.child[0].threads[0].signal_mask==0,"initial signal mask");
        const uint64_t trap=uint64_t{1}<<(SIGTRAP-1);
        auto trap_masked=set_mask(initial,0,usr1|trap);
        auto trap_born=spawn(trap_masked,0,1);
        require(trap_born.child[0].threads[0].signal_mask==(usr1|trap) &&
                trap_born.child[0].threads[1].signal_mask==(usr1|trap),
                "internal carrier trap changed parent or child guest mask");
        restore(trap_born);
        auto main_masked=set_mask(initial,0,usr1);
        auto born_a=spawn(main_masked,0,1);
        require(born_a.child[0].threads[1].signal_mask==usr1,"child did not inherit main mask");
        auto a_masked=set_mask(born_a,1,usr2);
        auto born_b=spawn(a_masked,1,2);
        require(born_b.child[0].threads[2].signal_mask==usr2 &&
                born_b.child[0].threads[0].signal_mask==usr1,"worker clone inherited anchor mask");
        auto exit_entry=request(born_b,1,SYS_exit);
        auto exited_a=step(exit_entry,1,SYS_exit);
        auto born_c=spawn(exited_a,0,3);
        require(born_c.child[0].threads[3].signal_mask==usr1,"reused carrier leaked retired mask");
        require(get_thread_list(pid).size()==3,"carrier was not reused");
        for(const auto* saved:{&initial,&main_masked,&born_a,&a_masked,&born_b,&exited_a,&born_c})restore(*saved);
        restore(born_c); // same-hash restore still installs/checks task masks
        auto persisted_a=set_mask(sys_state(born_a.ss_hash),1,usr2);
        require(persisted_a.ss_hash==a_masked.ss_hash,"persisted worker mask transition diverged");

        const uint64_t reserved=(uint64_t{1}<<31)|(uint64_t{1}<<32);
        auto filtered=set_mask(main_masked,0,usr2|reserved|(uint64_t{1}<<(SIGKILL-1))|
                                                      (uint64_t{1}<<(SIGSTOP-1)));
        require(filtered.child[0].threads[0].signal_mask==(usr2|reserved),"mask filtering differs from raw Linux ABI");
        auto query_entry=request(filtered,0,SYS_rt_sigprocmask,
                                 {999,0,reinterpret_cast<uint64_t>(&old_word),8});
        auto queried=step(query_entry,0,SYS_rt_sigprocmask);
        require(queried.child[0].si[0].rval==0 && read_guest(pid,&old_word)==(usr2|reserved),
                "NULL set query did not ignore how");
        for(int mode=0;mode!=4;++mode) {
            main_masked.recover_running_state();
            write_guest(pid,&mask_word,usr2);write_guest(pid,&old_word,uint64_t{0x12345678});
            auto source=capture(main_masked);
            std::array<uint64_t,6> args{SIG_SETMASK,reinterpret_cast<uint64_t>(&mask_word),
                                       reinterpret_cast<uint64_t>(&old_word),8};
            if(mode==0)args[1]=1;
            if(mode==1)args[2]=1;
            if(mode==2)args[3]=128;
            if(mode==3)args[0]=999;
            auto entry=request(source,0,SYS_rt_sigprocmask,args);
            auto result=step(entry,0,SYS_rt_sigprocmask);
            require(static_cast<int64_t>(result.child[0].si[0].rval)==(mode<2?-EFAULT:-EINVAL),"sigprocmask error precedence");
            require(result.child[0].threads[0].signal_mask==(mode==1?usr2:usr1) &&
                    read_guest(pid,&old_word)==0x12345678,"sigprocmask error changed wrong memory/state");
            masks(result);restore(result);
        }
        for(const int how:{SIG_BLOCK,SIG_UNBLOCK}) {
            main_masked.recover_running_state();write_guest(pid,&mask_word,usr1|usr2);
            auto source=capture(main_masked);
            auto entry=request(source,0,SYS_rt_sigprocmask,
                               {static_cast<uint64_t>(how),reinterpret_cast<uint64_t>(&mask_word),
                                reinterpret_cast<uint64_t>(&old_word),8});
            auto result=step(entry,0,SYS_rt_sigprocmask);
            require(result.child[0].si[0].rval==0 && read_guest(pid,&old_word)==usr1 &&
                    result.child[0].threads[0].signal_mask==(how==SIG_BLOCK?(usr1|usr2):0),
                    "BLOCK/UNBLOCK mask semantics");masks(result);
        }
        main_masked.recover_running_state();write_guest(pid,&mask_word,usr2);
        auto alias_source=capture(main_masked);
        auto alias_entry=request(alias_source,0,SYS_rt_sigprocmask,
                                 {SIG_SETMASK,reinterpret_cast<uint64_t>(&mask_word),
                                  reinterpret_cast<uint64_t>(&mask_word),8});
        auto alias_result=step(alias_entry,0,SYS_rt_sigprocmask);
        require(alias_result.child[0].threads[0].signal_mask==usr2 && read_guest(pid,&mask_word)==usr1,
                "aliased set/old ordering incorrect");masks(alias_result);

        main_masked.recover_running_state();
        if(shared_pending)require(kill(pid,SIGUSR1)==0,"queue shared signal");
        else require(syscall(SYS_tgkill,pid,pid,SIGUSR1)==0,"queue thread signal");
        const auto saves=StateStore::instance().get_stats().save_calls;
        bool capture_rejected=false,restore_rejected=false;
        try{(void)capture(main_masked);}catch(const std::runtime_error& e){
            capture_rejected=std::string(e.what()).find("pending signal")!=std::string::npos;
        }
        require(capture_rejected && StateStore::instance().get_stats().save_calls==saves,
                "pending signal capture silently omitted kernel state");
        try{initial.recover_running_state();}catch(const std::runtime_error& e){
            restore_rejected=std::string(e.what()).find("pending signal")!=std::string::npos;
        }
        require(restore_rejected,"restore silently retained pending signal");
        cleanup();
    }catch(...){cleanup();throw;}
}
int main(int argc,char** argv) {
    bool tiny=false,shared=false;
    for(int i=1;i<argc;++i){tiny|=std::strcmp(argv[i],"--tiny-cache")==0;shared|=std::strcmp(argv[i],"--shared-pending")==0;}
    loglevel=4;pid_t test=fork();
    if(!test){setpgid(0,0);try{run(tiny,shared);_exit(0);}catch(const std::exception& e){
        std::fprintf(stderr,"thread signal mask: %s\n",e.what());_exit(1);}}
    if(test<0)return 1;
    setpgid(test,test);int status=0;bool done=false;
    for(int i=0;i<6000;++i){if(waitpid(test,&status,WNOHANG)==test){done=true;break;}usleep(10000);}
    kill(-test,SIGKILL);if(!done)waitpid(test,&status,0);
    bool ok=done && WIFEXITED(status) && WEXITSTATUS(status)==0;
    std::printf("%s task signal masks, ABI errors, worker clone, carrier restore and %s pending rejection\n",
                ok?"PASS":"FAIL",shared?"shared":"thread");return ok?0:1;
}
