#include "config.h"
#include "monitor.h"
#include "engine/thread_pool.h"
#include "engine/pending_syscall.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "dispatcher.h"
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static int failures;
alignas(32) static std::array<unsigned char,32> rseq_buffer{};
static uint64_t robust_head_output=0x11223344,robust_length_output=0x55667788;
static const char replacement_name[] = "escaped";
static rlimit guest_limit{};
static char guest_name[16]{};
static timespec guest_zero{}, guest_delay{1,0}, guest_invalid{-1,0};

static void check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

static int stopped(pid_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int status;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (!WIFSTOPPED(status)) throw std::runtime_error("child did not stop");
            return WSTOPSIG(status);
        }
        if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("child stop deadline");
        usleep(1000);
    }
}

struct Result { bool rejected; long value; };
static Result execute(SyscallDispatcher& dispatcher, pid_t pid, int number,
                      std::array<uint64_t, 6> arguments = {}) {
    user_regs_struct original{}, registers{};
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &original)) throw std::runtime_error("get registers");
    registers = original;
    registers.rip = scratch_page + 0xff0;
    registers.rax = number;
    registers.rdi = arguments[0]; registers.rsi = arguments[1]; registers.rdx = arguments[2];
    registers.r10 = arguments[3]; registers.r8 = arguments[4]; registers.r9 = arguments[5];
    if (ptrace(PTRACE_SETREGS, pid, nullptr, &registers)) throw std::runtime_error("set registers");
    auto advance = [&] {
        if (ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr)) throw std::runtime_error("resume syscall");
        if (stopped(pid) != (SIGTRAP | 0x80)) throw std::runtime_error("unexpected syscall stop");
    };
    advance();
    syscall_info info{};
    info.nr = number;
    std::memcpy(info.args, arguments.data(), sizeof(info.args));
    bool rejected = false;
    try { dispatcher.on_enter(pid, number, info); }
    catch (const std::runtime_error&) {
        rejected = true;
        // Consume a harmless skipped syscall exit before restoring the fixture.
        // No rejected syscall is ever resumed with its original syscall number.
        tracee_set_orig_rax(pid, -1);
    }
    advance();
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &registers)) throw std::runtime_error("get syscall result");
    info.rval = registers.rax;
    if (!rejected) {
        try { dispatcher.on_exit(pid, info); }
        catch (const std::runtime_error&) { rejected=true; }
    }
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &registers) ||
        ptrace(PTRACE_SETREGS, pid, nullptr, &original)) throw std::runtime_error("restore registers");
    return {rejected, static_cast<long>(registers.rax)};
}

static std::string child_name(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (!std::getline(input, name)) throw std::runtime_error("read child comm");
    return name;
}

static void check_nohugepage(SyscallDispatcher& dispatcher, pid_t child) {
    auto* native=static_cast<unsigned char*>(mmap(nullptr,12288,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(native==MAP_FAILED) throw std::runtime_error("native nohugepage mapping");
    const auto mapped=execute(dispatcher,child,SYS_mmap,
        {0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0});
    if(mapped.rejected || mapped.value<=0) throw std::runtime_error("virtual nohugepage mapping");
    const auto address=uint64_t(mapped.value);
    std::array<unsigned char,12288> expected;expected.fill(0x5a);
    memcpy(native,expected.data(),expected.size());
    if(!guest_write_exact(child,reinterpret_cast<void*>(address),expected.data(),expected.size()))
        throw std::runtime_error("virtual nohugepage bytes");
    const auto provenance=ptmc_state.fs_states[0].virtual_file_mappings;
    auto compare=[&](uint64_t offset,uint64_t length,uint64_t advice,long expected_result) {
        errno=0;
        const long raw=syscall(SYS_madvise,reinterpret_cast<uint64_t>(native)+offset,length,advice);
        const long oracle=raw<0?-errno:raw;
        const auto actual=execute(dispatcher,child,SYS_madvise,{address+offset,length,advice,0,0,0});
        check(oracle==expected_result,"native MADV_NOHUGEPAGE argument oracle");
        check(!actual.rejected && actual.value==oracle,"MADV_NOHUGEPAGE retains native argument result");
    };
    for(uint64_t advice:{uint64_t(MADV_NOHUGEPAGE),uint64_t(0x10000000fULL),uint64_t(0xffffffff0000000fULL)})
        compare(0,1,advice,0);
    compare(1,1,MADV_NOHUGEPAGE,-EINVAL);
    compare(1,0,MADV_NOHUGEPAGE,-EINVAL);
    std::array<unsigned char,12288> actual{};
    check(!memcmp(native,expected.data(),expected.size()) &&
          guest_read_exact(child,reinterpret_cast<void*>(address),actual.data(),actual.size()) && actual==expected &&
          ptmc_state.fs_states[0].virtual_file_mappings==provenance,
          "MADV_NOHUGEPAGE preserves all bytes and virtual mapping provenance");
    for(uint64_t advice:{uint64_t(MADV_FREE),uint64_t(MADV_HUGEPAGE),uint64_t(-1)})
        check(execute(dispatcher,child,SYS_madvise,{address,4096,advice,0,0,0}).rejected,
              "other advice remains explicitly rejected");
    auto& fs=ptmc_state.fs_states[0];
    fs.add_virtual_file_mapping(address,address+4096);
    check(execute(dispatcher,child,SYS_madvise,{address,1,MADV_NOHUGEPAGE,0,0,0}).rejected,
          "MADV_NOHUGEPAGE rejects virtual file provenance");
    fs.remove_virtual_file_mapping(address,address+4096);
    check(munmap(native+4096,4096)==0,"native nohugepage hole");
    const auto hole=execute(dispatcher,child,SYS_munmap,{address+4096,4096,0,0,0,0});
    check(!hole.rejected && hole.value==0,"virtual nohugepage hole");
    compare(4096,0,MADV_NOHUGEPAGE,0);
    compare(4096,1,MADV_NOHUGEPAGE,-ENOMEM);
    errno=0;
    check(syscall(SYS_madvise,UINT64_MAX-4095,8192,MADV_NOHUGEPAGE)==-1 && errno==EINVAL,
          "native wrapped MADV_NOHUGEPAGE returns EINVAL");
    check(execute(dispatcher,child,SYS_madvise,{UINT64_MAX-4095,8192,MADV_NOHUGEPAGE,0,0,0}).rejected,
          "wrapped MADV_NOHUGEPAGE remains outside model range admission");
    check(execute(dispatcher,child,SYS_madvise,{scratch_page,4096,MADV_NOHUGEPAGE,0,0,0}).rejected,
          "MADV_NOHUGEPAGE rejects control memory");
    check(execute(dispatcher,child,SYS_madvise,
          {reinterpret_cast<uint64_t>(&replacement_name)&~uint64_t(4095),4096,MADV_NOHUGEPAGE,0,0,0}).rejected,
          "MADV_NOHUGEPAGE rejects file-backed mappings");
    const long shared=tracee_do_syscall(child,SYS_mmap,0,4096,PROT_READ|PROT_WRITE,
                                       MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(shared<=0) throw std::runtime_error("shared nohugepage fixture");
    check(execute(dispatcher,child,SYS_madvise,{uint64_t(shared),4096,MADV_NOHUGEPAGE,0,0,0}).rejected,
          "MADV_NOHUGEPAGE rejects shared mappings");
    tracee_do_syscall(child,SYS_munmap,shared,4096,0,0,0,0);
    execute(dispatcher,child,SYS_munmap,{address,12288,0,0,0,0});
    munmap(native,12288);
}

int main() {
    pid_t child = -1;
    try {
        child = fork();
        if (child < 0) throw std::runtime_error("fork");
        if (!child) {
            if (__rseq_size && syscall(SYS_rseq,
                    static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                    32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(9);
            if (setpgid(0, 0) || prctl(PR_SET_NAME, "policy-child", 0, 0, 0)) _exit(2);
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) _exit(2);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(91);
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            asm volatile("int3");
            _exit(0);
        }
        if (stopped(child) != SIGTRAP) throw std::runtime_error("unexpected initial stop");
        if (getpgid(child) != child) throw std::runtime_error("fixture process group");
        if (ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL))
            throw std::runtime_error("ptrace options");
        ptmc_state.pids[0] = child;
        ptmc_state.cursor = 0;
        adopt_thread_signal_mask(0,child);
        SyscallDispatcher dispatcher;
        check(SyscallRoute{}.action == SyscallAction::REJECT, "default route rejects unknown host effects");
        bool unknown_rejected = true;
        for (const auto number : {-1, 1000000, SYS_kill, SYS_ptrace, SYS_reboot})
            unknown_rejected &= dispatcher.get_route(number).action == SyscallAction::REJECT;
        check(unknown_rejected, "unmodeled and out-of-range syscalls reject");
        for (const auto args : {std::array<uint64_t,6>{reinterpret_cast<uint64_t>(rseq_buffer.data()),32,0,RSEQ_SIG,0,0},
                               std::array<uint64_t,6>{1,UINT32_MAX,UINT32_MAX,0,0,0},
                               std::array<uint64_t,6>{reinterpret_cast<uint64_t>(rseq_buffer.data()),32,RSEQ_FLAG_UNREGISTER,RSEQ_SIG,0,0}}) {
            const auto unavailable=execute(dispatcher,child,SYS_rseq,args);
            check(!unavailable.rejected && unavailable.value==-ENOSYS,"rseq unavailable for all argument forms");
        }
        std::array<unsigned char,32> observed_rseq{};
        __ptrace_rseq_configuration registration{};
        check(guest_read_exact(child,rseq_buffer.data(),observed_rseq.data(),observed_rseq.size()) &&
              observed_rseq==rseq_buffer,"unavailable rseq leaves user memory unchanged");
        check(ptrace(PTRACE_GET_RSEQ_CONFIGURATION,child,reinterpret_cast<void*>(sizeof(registration)),&registration)==sizeof(registration) &&
              !registration.rseq_abi_pointer,"unavailable rseq never registers with host kernel");
        const auto robust_set=execute(dispatcher,child,SYS_set_robust_list,{0,24,0,0,0,0});
        check(!robust_set.rejected && robust_set.value==0,"logical robust-list registration supported");
        const uint64_t arbitrary_head=0x12345678;
        auto robust=execute(dispatcher,child,SYS_set_robust_list,{arbitrary_head,24,0,0,0,0});
        check(!robust.rejected && robust.value==0,"robust set does not dereference head");
        robust=execute(dispatcher,child,SYS_set_robust_list,{0,23,0,0,0,0});
        check(!robust.rejected && robust.value==-EINVAL,"robust length validation preserves registration");
        robust=execute(dispatcher,child,SYS_get_robust_list,
                       {0,reinterpret_cast<uint64_t>(&robust_head_output),reinterpret_cast<uint64_t>(&robust_length_output),0,0,0});
        uint64_t head=0,length=0;
        check(!robust.rejected && robust.value==0 &&
              guest_read_exact(child,&robust_head_output,&head,sizeof(head)) && head==arbitrary_head &&
              guest_read_exact(child,&robust_length_output,&length,sizeof(length)) && length==24,
              "robust get returns logical pointer and ABI length");
        robust=execute(dispatcher,child,SYS_get_robust_list,
                       {uint64_t(logical_process_id(0)),reinterpret_cast<uint64_t>(&robust_head_output),reinterpret_cast<uint64_t>(&robust_head_output),0,0,0});
        check(!robust.rejected && robust.value==0 && guest_read_exact(child,&robust_head_output,&head,sizeof(head)) &&
              head==arbitrary_head,"robust aliased outputs write length before pointer");
        length=99;guest_write_exact(child,&robust_length_output,&length,sizeof(length));
        robust=execute(dispatcher,child,SYS_get_robust_list,
                       {0,1,reinterpret_cast<uint64_t>(&robust_length_output),0,0,0});
        check(!robust.rejected && robust.value==-EFAULT && guest_read_exact(child,&robust_length_output,&length,sizeof(length)) &&
              length==24,"robust get head fault retains preceding length write");
        robust=execute(dispatcher,child,SYS_get_robust_list,{uint64_t(-1),1,1,0,0,0});
        check(!robust.rejected && robust.value==-ESRCH,"unknown logical robust owner returns ESRCH first");
        execute(dispatcher,child,SYS_set_robust_list,{0,24,0,0,0,0});
        auto result = execute(dispatcher, child, SYS_prctl,
                              {PR_SET_NAME, reinterpret_cast<uint64_t>(replacement_name), 0, 0, 0, 0});
        check(!result.rejected && result.value==0 && child_name(child)=="policy-child",
              "PR_SET_NAME updates logical name without host mutation");
        result=execute(dispatcher,child,SYS_prctl,
                       {PR_GET_NAME,reinterpret_cast<uint64_t>(guest_name),0,0,0,0});
        char observed_name[16]{};
        check(!result.rejected && result.value==0 &&
              guest_read_exact(child,guest_name,observed_name,sizeof(observed_name)) &&
              !strcmp(observed_name,replacement_name),"PR_GET_NAME reads saved logical name");
        auto& prctl_state=ptmc_state.process_states[0];
        for(unsigned capability=0;capability<=64;++capability) {
            const long native=tracee_do_syscall(child,SYS_prctl,PR_CAPBSET_READ,capability,0,0,0,0);
            if(native==-EINVAL) {prctl_state.capability_count=capability;break;}
            if(capability==64 || native<0) throw std::runtime_error("policy capability oracle");
            prctl_state.capability_mask|=uint64_t(native)<<capability;
        }
        for(uint64_t capability:{uint64_t(0),uint64_t(prctl_state.capability_count),uint64_t(1)<<32}) {
            const auto query=execute(dispatcher,child,SYS_prctl,
                {0xffffffff00000000ULL|PR_CAPBSET_READ,capability,~0ULL,~0ULL,~0ULL,0});
            const long expected=capability>=prctl_state.capability_count ? -EINVAL : (prctl_state.capability_mask>>capability)&1;
            check(!query.rejected && query.value==expected,"actual capability query preserves cap64 option32 and ignored tails");
        }
        const auto prctl_comm=ptmc_state.running_state.child[0].threads[0].comm;
        std::array<unsigned char,4096> prctl_page{},prctl_after{};
        if(!guest_read_exact(child,reinterpret_cast<void*>(scratch_page),prctl_page.data(),prctl_page.size()))
            throw std::runtime_error("prctl scratch fixture");
        for(uint64_t option:{uint64_t(PR_GET_DUMPABLE),uint64_t(PR_SET_DUMPABLE),uint64_t(PR_CAPBSET_DROP),uint64_t(PR_SET_NO_NEW_PRIVS),uint64_t(-1)})
            check(execute(dispatcher,child,SYS_prctl,{option,scratch_page+0xff0,~0ULL,~0ULL,~0ULL,0}).rejected,
                  "unknown prctl options reject before pointer or host state effects");
        for(uint64_t option:{uint64_t(PR_SET_NAME),uint64_t(PR_GET_NAME)})
            for(uint64_t address:{scratch_page+0x100,scratch_page+0xff0,scratch_page-8}) {
                user_regs_struct before{},after{};ptrace(PTRACE_GETREGS,child,nullptr,&before);
                const auto rejected=execute(dispatcher,child,SYS_prctl,{option,address,~0ULL,~0ULL,~0ULL,0});
                check(rejected.rejected && guest_read_exact(child,reinterpret_cast<void*>(scratch_page),prctl_after.data(),prctl_after.size()) &&
                      prctl_page==prctl_after && ptmc_state.running_state.child[0].threads[0].comm==prctl_comm &&
                      ptrace(PTRACE_GETREGS,child,nullptr,&after)==0 && !memcmp(&before,&after,sizeof(before)),
                      "prctl name control-page overlap rejects without name page or register mutation");
            }

        for (const auto number : {SYS_getrandom, SYS_clock_gettime, SYS_time, SYS_gettimeofday}) {
            const auto action = dispatcher.get_route(number).action;
            check(action == SyscallAction::EMULATE || action == SyscallAction::REJECT,
                  "clock and random input never passes through");
        }
        result = execute(dispatcher, child, SYS_getpid);
        check(!result.rejected && result.value == logical_process_id(0), "getpid reports the modeled process identity");
        const auto tid = execute(dispatcher, child, SYS_gettid);
        check(!tid.rejected && tid.value == result.value, "main gettid agrees with logical getpid");
        check(dispatcher.get_route(SYS_clone).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_exit).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_clone3).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_set_tid_address).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_rt_sigprocmask).action==SyscallAction::EMULATE,
              "thread lifecycle never executes the guest kernel syscall");
        result=execute(dispatcher,child,SYS_clone);
        check(result.rejected,"unsupported clone flags reject before kernel");
        result = execute(dispatcher, child, SYS_brk);
        check(!result.rejected && result.value > 0, "brk query remains available");
        for (const int flags : {MAP_ANONYMOUS, MAP_SHARED | MAP_ANONYMOUS,
                                MAP_PRIVATE | MAP_ANONYMOUS}) {
            const auto empty = execute(dispatcher, child, SYS_mmap,
                {0, 0, PROT_READ, static_cast<uint64_t>(flags), uint64_t(-1), 0});
            check(!empty.rejected && empty.value == -EINVAL,
                  "zero-length mmap returns EINVAL before mapping policy");
        }
        result=execute(dispatcher,child,SYS_mmap,
                       {0,0,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,uint64_t(-1),0});
        check(result.rejected,"zero-length huge-page setup remains explicitly unsupported");
        FILE* native_file = tmpfile();
        if (!native_file) throw std::runtime_error("native mmap oracle file");
        errno = 0;
        const long native_bad_fd = syscall(SYS_mmap, 0, 0, PROT_READ, MAP_PRIVATE, -1, 0);
        check(native_bad_fd == -1 && errno == EBADF,
              "native zero-length file mmap checks bad descriptor first");
        errno = 0;
        const long native_valid_fd = syscall(SYS_mmap, 0, 0, PROT_READ, MAP_PRIVATE, fileno(native_file), 0);
        check(native_valid_fd == -1 && errno == EINVAL,
              "native zero-length file mmap with valid descriptor returns EINVAL");
        errno = 0;
        const long native_anon = syscall(SYS_mmap, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(native_anon == -1 && errno == EINVAL,
              "native zero-length anonymous mmap ignores bad descriptor");
        fclose(native_file);
        if (!ptmc_state.fd_managers[0]) ptmc_state.fd_managers[0] = std::make_shared<FdManager>();
        const int virtual_file = ptmc_state.fd_managers[0]->allocate_fd(FdType::FILE);
        result = execute(dispatcher, child, SYS_mmap, {0, 0, PROT_READ, MAP_PRIVATE, uint64_t(-1), 0});
        check(!result.rejected && result.value == -EBADF,
              "zero-length file mmap preserves bad descriptor precedence");
        result = execute(dispatcher, child, SYS_mmap, {0, 0, PROT_READ, MAP_PRIVATE, uint64_t(virtual_file), 0});
        check(!result.rejected && result.value == -EINVAL,
              "zero-length file mmap with valid descriptor returns EINVAL");
        ptmc_state.fd_managers[0]->release_fd(virtual_file);
        for (int legacy : {MAP_DENYWRITE, MAP_EXECUTABLE, MAP_DENYWRITE|MAP_EXECUTABLE}) {
            void* native=mmap(nullptr,4096,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|legacy,-1,0);
            check(native!=MAP_FAILED,"native obsolete mmap flags are ignored");
            if (native!=MAP_FAILED) munmap(native,4096);
            const auto ignored=execute(dispatcher,child,SYS_mmap,
                {0,4096,PROT_READ|PROT_WRITE,uint64_t(MAP_PRIVATE|MAP_ANONYMOUS|legacy),uint64_t(-1),0});
            check(!ignored.rejected && ignored.value>0,"obsolete mmap flags preserve private mapping behavior");
            if (!ignored.rejected && ignored.value>0)
                execute(dispatcher,child,SYS_munmap,{uint64_t(ignored.value),4096,0,0,0,0});
        }
        const auto mapping = execute(dispatcher, child, SYS_mmap,
                                     {0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                      uint64_t(-1), 0});
        check(!mapping.rejected && mapping.value > 0, "private anonymous mmap remains available");
        if (!mapping.rejected && mapping.value > 0) {
            result = execute(dispatcher, child, SYS_mprotect,
                             {uint64_t(mapping.value), 4096, PROT_READ, 0, 0, 0});
            check(!result.rejected && result.value == 0, "mprotect remains available");
            result = execute(dispatcher, child, SYS_mprotect,
                             {uint64_t(mapping.value + 1), 4096, PROT_READ, 0, 0, 0});
            check(!result.rejected && result.value == -EINVAL, "native invalid memory arguments preserve EINVAL");
            result = execute(dispatcher, child, SYS_munmap, {uint64_t(mapping.value), 4096, 0, 0, 0, 0});
            check(!result.rejected && result.value == 0, "munmap remains available");
        }
        check(dispatcher.get_route(SYS_getrlimit).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_prlimit64).action==SyscallAction::EMULATE,
              "resource queries use captured process state instead of host execution");
        result=execute(dispatcher,child,SYS_getrlimit,
                       {RLIMIT_NOFILE,reinterpret_cast<uint64_t>(&guest_limit),0,0,0,0});
        check(!result.rejected && result.value==0,"getrlimit query is available");
        ptmc_state.process_states[0].resource_limits[RLIMIT_NOFILE]={321,654};
        result = execute(dispatcher, child, SYS_prlimit64,
                         {0, RLIMIT_NOFILE, 0, reinterpret_cast<uint64_t>(&guest_limit), 0, 0});
        rlimit limit{};
        check(!result.rejected && result.value == 0 &&
              guest_read_exact(child, &guest_limit, &limit, sizeof(limit)) && limit.rlim_cur == 321 && limit.rlim_max == 654,
              "prlimit64 self query reads captured resource values");
        result=execute(dispatcher,child,SYS_prlimit64,
                       {uint64_t(logical_process_id(0)),RLIMIT_NOFILE,0,reinterpret_cast<uint64_t>(&guest_limit),0,0});
        check(!result.rejected && result.value==0,"logical self PID resolves to owned resource query");
        result=execute(dispatcher,child,SYS_prlimit64,
                       {0xffffffff00000000ULL|uint64_t(logical_process_id(0)),
                        0x100000000ULL|RLIMIT_NOFILE,0,reinterpret_cast<uint64_t>(&guest_limit),0,0});
        check(!result.rejected && result.value==0,"resource query entry admits low32 logical PID and resource aliases");
        const auto resource_state=ptmc_state.process_states[0];
        std::array<unsigned char,4096> resource_page_before{},resource_page_after{};
        if(!guest_read_exact(child,reinterpret_cast<void*>(scratch_page),resource_page_before.data(),resource_page_before.size()))
            throw std::runtime_error("resource reserved page fixture");
        user_regs_struct resource_regs_before{},resource_regs_after{};
        if(ptrace(PTRACE_GETREGS,child,nullptr,&resource_regs_before)) throw std::runtime_error("resource saved registers");
        for(int nr:{SYS_getrlimit,SYS_prlimit64}) {
            for(uint64_t destination:{scratch_page+0x100,scratch_page+0xff0,scratch_page-8}) {
                const auto arguments=nr==SYS_getrlimit ?
                    std::array<uint64_t,6>{RLIMIT_NOFILE,destination,0,0,0,0} :
                    std::array<uint64_t,6>{0,RLIMIT_NOFILE,0,destination,0,0};
                const auto rejected=execute(dispatcher,child,nr,arguments);
                check(rejected.rejected &&
                      guest_read_exact(child,reinterpret_cast<void*>(scratch_page),resource_page_after.data(),resource_page_after.size()) &&
                      resource_page_after==resource_page_before && ptmc_state.process_states[0]==resource_state &&
                      ptrace(PTRACE_GETREGS,child,nullptr,&resource_regs_after)==0 &&
                      !memcmp(&resource_regs_before,&resource_regs_after,sizeof(resource_regs_before)),
                      "actual resource queries reject reserved output without state or register mutation");
                auto invalid=arguments;invalid[nr==SYS_getrlimit?0:1]=16;
                const auto bad_resource=execute(dispatcher,child,nr,invalid);
                check(!bad_resource.rejected && bad_resource.value==-EINVAL,
                      "actual invalid resource precedes reserved output overlap");
            }
        }
        const auto null_old=execute(dispatcher,child,SYS_prlimit64,{0,RLIMIT_NOFILE,0,0,0,0});
        const auto null_output=execute(dispatcher,child,SYS_getrlimit,{RLIMIT_NOFILE,0,0,0,0,0});
        check(!null_old.rejected && null_old.value==0 && !null_output.rejected && null_output.value==-EFAULT,
              "actual resource null output behavior survives reserved-page guard");
        result = execute(dispatcher, child, SYS_prlimit64,
                         {0, RLIMIT_NOFILE, reinterpret_cast<uint64_t>(&guest_limit), 0, 0, 0});
        check(result.rejected, "prlimit64 mutation rejected before execution");
        result = execute(dispatcher, child, SYS_prlimit64,
                         {uint64_t(getpid()), RLIMIT_NOFILE, 0, reinterpret_cast<uint64_t>(&guest_limit), 0, 0});
        check(result.rejected, "prlimit64 foreign process query rejected");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,uint64_t(-1),0});
        check(result.rejected,"shared anonymous mmap rejected before kernel");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0});
        check(!result.rejected && result.value > 0,"anonymous guard mmap supported");
        if (!result.rejected && result.value > 0) {
            const auto guard = uint64_t(result.value);
            char byte = 0;
            check(!guest_read_exact(child, reinterpret_cast<void*>(guard), &byte, 1),
                  "guard mapping remains unreadable to guest copies");
            result=execute(dispatcher,child,SYS_mprotect,{guard,4096,PROT_READ|PROT_WRITE,0,0,0});
            check(!result.rejected && result.value==0,"guard can become writable");
            result=execute(dispatcher,child,SYS_mprotect,{guard,4096,PROT_NONE,0,0,0});
            check(!result.rejected && result.value==0,"anonymous mprotect can install guard");
            result=execute(dispatcher,child,SYS_munmap,{guard,4096,0,0,0,0});
            check(!result.rejected && result.value==0,"guard can be unmapped");
        }
        check_nohugepage(dispatcher,child);
        const auto discard=execute(dispatcher,child,SYS_mmap,
                       {0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0});
        check(!discard.rejected && discard.value>0,"allocate discard fixture");
        if (!discard.rejected && discard.value>0) {
            const auto address=uint64_t(discard.value);const uint32_t marker=0x12345678;
            guest_write_exact(child,reinterpret_cast<void*>(address),&marker,sizeof(marker));
            guest_write_exact(child,reinterpret_cast<void*>(address+4096),&marker,sizeof(marker));
            result=execute(dispatcher,child,SYS_madvise,{address,4096,MADV_DONTNEED,0,0,0});
            uint32_t zero=1,retained=0;
            check(!result.rejected && result.value==0 &&
                  guest_read_exact(child,reinterpret_cast<void*>(address),&zero,sizeof(zero)) && zero==0 &&
                  guest_read_exact(child,reinterpret_cast<void*>(address+4096),&retained,sizeof(retained)) && retained==marker,
                  "MADV_DONTNEED zeros only requested anonymous pages");
            result=execute(dispatcher,child,SYS_madvise,{address,4096,MADV_FREE,0,0,0});
            check(result.rejected,"lazy nondeterministic MADV_FREE remains rejected");
            result=execute(dispatcher,child,SYS_madvise,{address+1,4096,MADV_DONTNEED,0,0,0});
            check(!result.rejected && result.value==-EINVAL,"unaligned madvise preserves native EINVAL");
            execute(dispatcher,child,SYS_munmap,{address,8192,0,0,0,0});
        }
        result=execute(dispatcher,child,SYS_madvise,{scratch_page,4096,MADV_DONTNEED,0,0,0});
        check(result.rejected,"madvise cannot discard control memory");
        result=execute(dispatcher,child,SYS_madvise,
                       {reinterpret_cast<uint64_t>(&replacement_name)&~uint64_t(4095),4096,MADV_DONTNEED,0,0,0});
        check(result.rejected,"madvise cannot discard file-backed state");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,uint64_t(-1),0});
        check(result.rejected,"write-only mappings remain unsupported");
        result=execute(dispatcher,child,SYS_mmap,
                       {0,4096,PROT_NONE,MAP_PRIVATE,uint64_t(-1),0});
        check(result.rejected,"file-backed guard mappings remain unsupported");
        result=execute(dispatcher,child,SYS_mprotect,
                       {reinterpret_cast<uint64_t>(&replacement_name)&~uint64_t(4095),4096,PROT_NONE,0,0,0});
        check(result.rejected,"file-backed guard mprotect rejected before kernel");
        result=execute(dispatcher,child,SYS_mprotect,{scratch_page,4096,PROT_NONE,0,0,0});
        check(result.rejected,"guard cannot replace reserved scratch page");
        result=execute(dispatcher,child,SYS_mprotect,{scratch_page,4096,PROT_READ,0,0,0});
        check(result.rejected,"mprotect cannot change reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_munmap,{scratch_page,4096,0,0,0,0});
        check(result.rejected,"munmap cannot remove reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_mmap,
                       {scratch_page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,uint64_t(-1),0});
        check(result.rejected,"fixed mmap cannot replace reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_mmap,
                       {scratch_page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,uint64_t(-1),0});
        check(result.rejected,"fixed noreplace mmap cannot target reserved syscall scratch page");
        result=execute(dispatcher,child,SYS_munmap,{UINT64_MAX-4095,8192,0,0,0,0});
        check(result.rejected,"mapping address overflow rejected before kernel");
        result=execute(dispatcher,child,SYS_mprotect,{0,UINT64_MAX,PROT_READ,0,0,0});
        check(result.rejected,"mapping length overflow rejected before kernel");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_zero),0,0,0,0,0});
        check(!result.rejected && result.value==0,"valid zero nanosleep completes");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_delay),0,0,0,0,0});
        check(!result.rejected && result.value==-ENOSYS &&
              ptmc_state.running_state.child[0].threads[0].pending_syscall &&
              ptmc_state.running_state.child[0].threads[0].pending_syscall->reason==PendingReason::Timer,
              "nonzero nanosleep registers unfinished timer");
        check(!expire_pending_syscall(0,1),"sleep cannot expire before its deadline");
        ptmc_state.time[0].tv_sec+=2;
        syscall_info completion{};
        check(expire_pending_syscall(0,1) && complete_pending_syscall(0,child,completion) &&
              completion.nr==SYS_nanosleep && completion.rval==0,
              "sleep expires through explicit completion");
        result=execute(dispatcher,child,SYS_clock_nanosleep,
                       {CLOCK_MONOTONIC,TIMER_ABSTIME,reinterpret_cast<uint64_t>(&guest_delay),0,0,0});
        check(!result.rejected && result.value==0,"expired absolute sleep completes immediately");
        result=execute(dispatcher,child,SYS_clock_nanosleep,
                       {CLOCK_BOOTTIME,0,reinterpret_cast<uint64_t>(&guest_zero),0,0,0});
        check(result.rejected,"unmodeled sleep clock rejects explicitly");
        result=execute(dispatcher,child,SYS_nanosleep,{reinterpret_cast<uint64_t>(&guest_invalid),0,0,0,0,0});
        check(!result.rejected && result.value==-EINVAL,"invalid nanosleep duration returns EINVAL");
        result=execute(dispatcher,child,SYS_nanosleep,{1,0,0,0,0,0});
        check(!result.rejected && result.value==-EFAULT,"invalid nanosleep pointer returns EFAULT");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::fprintf(stderr, "syscall policy: %s\n", error.what());
        return 1;
    }
}
