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
#include "engine/thread_pool.h"
#include <linux/sched.h>
#include <linux/futex.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static_assert(NP == 1);
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static constexpr uint64_t canary = 0xaabbccdd11223344ULL;
static uint64_t parent_word = canary, child_word = canary, other_word = canary;
static uint64_t main_word = canary;
struct Job {
    uint64_t parent, child, replacement;
    long observed, tid, registration;
    uint64_t use_clone3, args_address, args_size;
};
static Job job{};
static clone_args args{};
static struct { clone_args value; uint64_t extra; } extended_args{};
alignas(16) static unsigned char stack[16384];
static long clone_return;
static int exit_main;
static int wait_operation = FUTEX_WAIT;
static unsigned char* fault_area;
static unsigned char* tail_area;
extern "C" long tid_spawn(Job*, void*);
asm(R"(
.text
.globl tid_spawn
.type tid_spawn,@function
tid_spawn:
 push %r12
 mov %rdi,%r12
 cmpq $0,48(%r12)
 jne 2f
 mov 0(%r12),%rdx
 mov 8(%r12),%r10
 xor %r8d,%r8d
 mov $0x1350f00,%edi
 mov $56,%eax
 jmp 3f
2:
 mov 56(%r12),%rdi
 mov 64(%r12),%rsi
 mov $435,%eax
3:
 syscall
 test %rax,%rax
 jz 1f
 pop %r12
 ret
1:
 mov 8(%r12),%rdi
 cmp $4096,%rdi
 jb 4f
 mov (%rdi),%eax
 mov %rax,24(%r12)
4:
 mov $186,%eax
 syscall
 mov %rax,32(%r12)
 mov 16(%r12),%rdi
 mov $218,%eax
 syscall
 mov %rax,40(%r12)
 mov $60,%eax
 xor %edi,%edi
 syscall
 ud2
.size tid_spawn,.-tid_spawn
)");
static void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
template<class T> static T read_guest(pid_t pid, const T* address) {
    T value{};
    require(guest_read_exact(pid, address, &value, sizeof value), "read guest");
    return value;
}
template<class T> static void write_guest(pid_t pid, T* address, const T& value) {
    require(guest_write_exact(pid, address, &value, sizeof value), "write guest");
}
static sys_state capture(const sys_state& source) {
    syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
    std::memcpy(info[0], source.child[0].si, sizeof info[0]);
    return sys_state(info, &source);
}
static sys_state step(const sys_state& source, int actor, int nr) {
    auto result = state_transition(source, 0, actor);
    require(!result.property_violation && result.code != CKPT_STOP && result.code != CKPT_DISCARD,
            "transition rejected");
    require(result.new_state.child[0].si[actor].nr == nr, "unexpected syscall");
    return std::move(result.new_state);
}
static void roundtrip(const sys_state& expected) {
    sys_state loaded(expected.ss_hash);
    loaded.recover_running_state();
    auto copied = capture(loaded);
    require(copied.ss_hash == expected.ss_hash, "persisted restore identity drift");
}
static void run(bool tiny, bool clone3) {
    job.parent = reinterpret_cast<uint64_t>(&parent_word);
    job.child = job.replacement = reinterpret_cast<uint64_t>(&child_word);
    job.use_clone3 = clone3;
    job.args_address = reinterpret_cast<uint64_t>(&args);
    job.args_size = sizeof args;
    args.flags = 0x1350f00;
    args.parent_tid = job.parent;
    args.child_tid = job.child;
    args.stack = reinterpret_cast<uint64_t>(stack);
    args.stack_size = sizeof stack;
    extended_args.value = args;
    extended_args.extra = 1;
    fault_area = static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ|PROT_WRITE,
                                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    require(fault_area != MAP_FAILED, "fault mapping");
    const uint32_t fault_canary = 0x44332211;
    std::memcpy(fault_area + 4093, &fault_canary, sizeof fault_canary);
    require(mprotect(fault_area + 4096, 4096, PROT_READ) == 0, "readonly fault page");
    auto* gap = static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ|PROT_WRITE,
                                                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    require(gap != MAP_FAILED, "clone3 tail mapping");
    tail_area = gap + 4096;
    tail_area[0] = 1;
    require(munmap(gap, 4096) == 0, "unmap clone3 header page");
    char directory[] = "/tmp/detsim-thread-tid-XXXXXX";
    require(mkdtemp(directory), "mkdtemp");
    auto previous = std::filesystem::current_path();
    require(chdir(directory) == 0, "chdir");
    pid_t pid = fork();
    require(pid >= 0, "fork");
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (__rseq_size && syscall(SYS_rseq,
                static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(2);
        syscall(SYS_set_tid_address, &main_word);
        if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                 MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(3);
        auto code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
        code[0] = 0x0f; code[1] = 0x05;
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
        asm volatile("int3");
        clone_return = tid_spawn(&job, stack + sizeof stack);
        if (exit_main == 2) syscall(SYS_exit_group, 0);
        if (exit_main == 1) syscall(SYS_exit, 0);
        syscall(SYS_futex, &child_word, wait_operation, 2, nullptr, nullptr, 0);
        for (;;) syscall(SYS_getpid);
    }
    auto cleanup = [&] {
        kill(pid, SIGKILL);
        while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        std::filesystem::current_path(previous);
        std::filesystem::remove_all(directory);
    };
    try {
        int status = 0;
        require(waitpid(pid, &status, __WALL) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP,
                "initial breakpoint");
        require(ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                PTRACE_O_TRACECLONE|PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL) == 0, "ptrace options");
        ptmc_state.pids[0] = pid;
        ptmc_state.cursor = -1;
        ptmc_state.fd_managers[0] = std::make_shared<FdManager>();
        ptmc_state.fs_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        ptmc_state.sock_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        adopt_clear_child_tid(0, pid);
        auto* query_address = reinterpret_cast<uint64_t*>(scratch_page + 0x300);
        const auto query_saved = read_guest(pid, query_address);
        require(tracee_do_syscall(pid, SYS_prctl, PR_GET_TID_ADDRESS,
                reinterpret_cast<uintptr_t>(query_address), 0, 0, 0, 0) == 0,
                "query adopted carrier clear address");
        const auto physical_clear_address = read_guest(pid, query_address);
        write_guest(pid, query_address, query_saved);
        require(physical_clear_address == 0, "adoption left physical clear address registered");
        StateStore::Config config;
        if (tiny) config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        auto_mode = 1;
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
        sys_state initial(info);
        require(initial.child[0].threads[0].clear_child_tid == reinterpret_cast<uint64_t>(&main_word) &&
                read_guest(pid, &main_word) == canary,
                "initial snapshot lost adopted clear address or changed its memory");
        ptmc_state.running_state = initial;
        thread::init_all(ptmc_state);
        exec::init_all();
        const int clone_nr = clone3 ? SYS_clone3 : SYS_clone;
        auto born = step(initial, 0, clone_nr);
        require(born.child[0].si[0].rval == 2, "clone returned physical identity");
        require(read_guest(pid, &parent_word) == 0xaabbccdd00000002ULL,
                "parent TID write missing or wider than 32 bits");
        require(read_guest(pid, &child_word) == canary && read_guest(pid, &job).tid == 0,
                "CHILD_SETTID or child instructions ran before child selection");
        roundtrip(born);
        auto started = step(born, 1, SYS_gettid);
        require(read_guest(pid, &child_word) == 0xaabbccdd00000002ULL &&
                read_guest(pid, &job).observed == 2,
                "first child instruction did not see logical CHILD_SETTID");
        auto waiting = step(started, 0, SYS_futex);
        require(read_guest(pid, &clone_return) == 2 && ptmc_state.thread_blocked[0][0],
                "parent join did not block using logical child TID");
        auto registered = step(waiting, 1, SYS_set_tid_address);
        require(registered.child[0].si[1].rval == 2, "set_tid_address returned physical identity");
        auto exited = step(registered, 1, SYS_exit);
        require(read_guest(pid, &child_word) == 0xaabbccdd00000000ULL &&
                !ptmc_state.thread_blocked[0][0] && !exited.child[0].threads[1].alive,
                "exit failed to clear 32 bits and wake join waiter");
        auto joined = step(exited, 0, SYS_getpid);
        require(read_guest(pid, &job).registration == 2, "guest saw wrong registration result");
        for (const auto* saved : {&born, &waiting, &registered, &exited, &joined}) roundtrip(*saved);
        auto replay_started = step(sys_state(born.ss_hash), 1, SYS_gettid);
        require(replay_started.ss_hash == started.ss_hash, "pending CHILD_SETTID replay changed identity");
        auto replay_waiting = step(replay_started, 0, SYS_futex);
        auto replay_registered = step(replay_waiting, 1, SYS_set_tid_address);
        auto replay_exited = step(replay_registered, 1, SYS_exit);
        require(replay_exited.ss_hash == exited.ss_hash, "clear/wake replay changed identity");
        auto persisted_exit = step(sys_state(registered.ss_hash), 1, SYS_exit);
        require(persisted_exit.ss_hash == exited.ss_hash && !ptmc_state.thread_blocked[0][0] &&
                read_guest(pid, &child_word) == 0xaabbccdd00000000ULL,
                "persisted registered source lost clear/wake semantics");

        started.recover_running_state();
        write_guest(pid, &wait_operation, FUTEX_WAIT | FUTEX_PRIVATE_FLAG);
        auto private_source = capture(started);
        auto private_wait = step(private_source, 0, SYS_futex);
        auto private_registered = step(private_wait, 1, SYS_set_tid_address);
        auto private_exit = step(private_registered, 1, SYS_exit);
        require(ptmc_state.thread_blocked[0][0] &&
                read_guest(pid, &child_word) == 0xaabbccdd00000000ULL,
                "shared exit wake incorrectly woke private-key waiter");
        roundtrip(private_wait);
        auto private_replay_registration = step(private_wait, 1, SYS_set_tid_address);
        auto private_replay_exit = step(private_replay_registration, 1, SYS_exit);
        require(private_replay_exit.ss_hash == private_exit.ss_hash,
                "private join queue lost identity on restore");

        // Replacing the clear address is branch-owned; exit must only clear the
        // new address. The original join remains blocked until restored.
        waiting.recover_running_state();
        auto replacement = read_guest(pid, &job);
        replacement.replacement = reinterpret_cast<uint64_t>(&other_word);
        write_guest(pid, &job, replacement);
        auto replace_source = capture(waiting);
        auto replaced = step(replace_source, 1, SYS_set_tid_address);
        roundtrip(replaced);
        auto replaced_exit = step(replaced, 1, SYS_exit);
        require(read_guest(pid, &other_word) == 0xaabbccdd00000000ULL &&
                read_guest(pid, &child_word) == 0xaabbccdd00000002ULL && ptmc_state.thread_blocked[0][0],
                "set_tid_address replacement cleared or woke original address");
        auto original_exit = step(registered, 1, SYS_exit);
        require(original_exit.ss_hash == exited.ss_hash, "replacement leaked across restored branch");

        waiting.recover_running_state();
        auto cancelled_job = read_guest(pid, &job);
        cancelled_job.replacement = 0;
        write_guest(pid, &job, cancelled_job);
        auto cancelled_source = capture(waiting);
        auto cancelled = step(cancelled_source, 1, SYS_set_tid_address);
        roundtrip(cancelled);
        auto cancelled_exit = step(cancelled, 1, SYS_exit);
        require(!cancelled_exit.child[0].threads[1].alive && ptmc_state.thread_blocked[0][0] &&
                read_guest(pid, &child_word) == 0xaabbccdd00000002ULL,
                "set_tid_address(NULL) failed to cancel exit clear/wake");

        // Linux clears only while another logical user shares the address
        // space. Retained physical carriers must not change that decision.
        born.recover_running_state();
        write_guest(pid, &exit_main, 1);
        auto main_exit_source = capture(born);
        auto main_exited = step(main_exit_source, 0, SYS_exit);
        require(read_guest(pid, &main_word) == 0xaabbccdd00000000ULL,
                "main exit did not clear its adopted address");
        auto last_started = step(main_exited, 1, SYS_gettid);
        auto last_registered = step(last_started, 1, SYS_set_tid_address);
        auto last_exited = step(last_registered, 1, SYS_exit);
        require(!last_exited.child[0].threads[0].alive && !last_exited.child[0].threads[1].alive &&
                read_guest(pid, &child_word) == 0xaabbccdd00000002ULL,
                "last logical thread incorrectly cleared TID because carriers remain");
        roundtrip(last_exited);
        roundtrip(born);
        require(read_guest(pid, &main_word) == canary &&
                ptmc_state.running_state.child[0].threads[0].clear_child_tid == reinterpret_cast<uint64_t>(&main_word),
                "rewind failed to restore adopted main clear address and memory");

        // Native Linux ignores failing clone TID put_user operations; the
        // thread is still born and can exit with an invalid clear address.
        for (const uint64_t bad_address : {uint64_t{1},
                reinterpret_cast<uint64_t>(fault_area + 4093)}) {
            initial.recover_running_state();
            auto bad_job = read_guest(pid, &job);
            bad_job.parent = bad_job.child = bad_job.replacement = bad_address;
            write_guest(pid, &job, bad_job);
            if (clone3) {
                auto bad_args = read_guest(pid, &args);
                bad_args.parent_tid = bad_args.child_tid = bad_address;
                write_guest(pid, &args, bad_args);
            }
            auto bad_source = capture(initial);
            auto bad_born = step(bad_source, 0, clone_nr);
            require(bad_born.child[0].si[0].rval == 2 && read_guest(pid, &parent_word) == canary,
                    "bad TID pointer prevented clone or changed memory");
            auto bad_start = step(bad_born, 1, SYS_gettid);
            auto bad_registration = step(bad_start, 1, SYS_set_tid_address);
            auto bad_exit = step(bad_registration, 1, SYS_exit);
            require(!bad_exit.child[0].threads[1].alive, "invalid clear address prevented exit");
            require(read_guest(pid, reinterpret_cast<const uint32_t*>(fault_area + 4093)) == fault_canary,
                    "faulting cross-page TID write changed writable prefix");
        }

        initial.recover_running_state();
        auto unaligned_job = read_guest(pid, &job);
        unaligned_job.parent += 1;
        unaligned_job.child += 1;
        unaligned_job.replacement += 1;
        write_guest(pid, &job, unaligned_job);
        if (clone3) {
            auto unaligned_args = read_guest(pid, &args);
            unaligned_args.parent_tid = unaligned_job.parent;
            unaligned_args.child_tid = unaligned_job.child;
            write_guest(pid, &args, unaligned_args);
        }
        auto unaligned_source = capture(initial);
        auto unaligned_born = step(unaligned_source, 0, clone_nr);
        require(read_guest(pid, &parent_word) == 0xaabbcc0000000244ULL,
                "unaligned parent TID write differs from Linux");
        auto unaligned_start = step(unaligned_born, 1, SYS_gettid);
        require(read_guest(pid, &child_word) == 0xaabbcc0000000244ULL,
                "unaligned child TID write differs from Linux");
        auto unaligned_registration = step(unaligned_start, 1, SYS_set_tid_address);
        auto unaligned_exit = step(unaligned_registration, 1, SYS_exit);
        require(!unaligned_exit.child[0].threads[1].alive &&
                read_guest(pid, &child_word) == 0xaabbcc0000000044ULL,
                "unaligned clear TID differs from Linux");

        if (clone3) {
            initial.recover_running_state();
            auto group_args = read_guest(pid, &args);
            group_args.flags &= ~uint64_t{CLONE_CHILD_CLEARTID};
            write_guest(pid, &args, group_args);
            write_guest(pid, &exit_main, 2);
            auto group_source = capture(initial);
            auto group_born = step(group_source, 0, SYS_clone3);
            require(read_guest(pid, &child_word) == canary, "group child ran before selection");
            auto group_exited = step(sys_state(group_born.ss_hash), 0, SYS_exit_group);
            const auto group_job = read_guest(pid, &job);
            require(!group_exited.child[0].threads[0].alive && !group_exited.child[0].threads[1].alive &&
                    group_job.observed == 0 && group_job.tid == 0 &&
                    read_guest(pid, &child_word) == 0xaabbccdd00000002ULL,
                    "exit_group skipped pending child TID write or ran child user code");
            roundtrip(group_exited);

            for (int mode = 0; mode != 5; ++mode) {
                initial.recover_running_state();
                auto invalid = read_guest(pid, &job);
                if (mode == 0) invalid.args_size = 63;
                if (mode == 1) invalid.args_address = 1;
                if (mode == 2) invalid.args_size = 4097;
                if (mode == 3) {
                    invalid.args_address = reinterpret_cast<uint64_t>(&extended_args);
                    invalid.args_size = sizeof extended_args;
                }
                if (mode == 4) {
                    invalid.args_address = reinterpret_cast<uint64_t>(tail_area) - sizeof(clone_args);
                    invalid.args_size = sizeof(clone_args) + 1;
                }
                write_guest(pid, &job, invalid);
                auto source = capture(initial);
                auto failed = step(source, 0, SYS_clone3);
                const long expected = mode == 0 ? -EINVAL : mode == 1 ? -EFAULT : -E2BIG;
                require(static_cast<int64_t>(failed.child[0].si[0].rval) == expected && failed.child[0].threads.size() == 1 &&
                        read_guest(pid, &parent_word) == canary, "clone3 invalid struct changed state or errno");
            }
            for (int mode = 0; mode != 6; ++mode) {
                initial.recover_running_state();
                auto invalid_args = read_guest(pid, &args);
                if (mode == 0) invalid_args.set_tid = reinterpret_cast<uint64_t>(&parent_word);
                if (mode == 1) invalid_args.set_tid_size = 1;
                if (mode == 2) {
                    invalid_args.set_tid = reinterpret_cast<uint64_t>(&parent_word);
                    invalid_args.set_tid_size = 33;
                }
                if (mode == 3) invalid_args.stack = uint64_t{1} << 63;
                if (mode == 4) {
                    invalid_args.set_tid = reinterpret_cast<uint64_t>(&parent_word);
                    invalid_args.set_tid_size = 1;
                }
                if (mode == 5) invalid_args.stack = uint64_t{1} << 47;
                write_guest(pid, &args, invalid_args);
                auto invalid_source = capture(initial);
                if (mode < 4) {
                    auto invalid_result = step(invalid_source, 0, SYS_clone3);
                    require(static_cast<int64_t>(invalid_result.child[0].si[0].rval) == -EINVAL &&
                            invalid_result.child[0].threads.size() == 1 &&
                            read_guest(pid, &parent_word) == canary,
                            "clone3 invalid TID/stack arguments changed state or errno");
                } else {
                    bool rejected = false;
                    try { (void)state_transition(invalid_source, 0, 0); }
                    catch (const std::runtime_error& error) {
                        const std::string expected = mode == 4 ? "explicit TID selection" :
                                                               "extended virtual-address stack";
                        rejected = std::string(error.what()).find(expected) != std::string::npos;
                    }
                    require(rejected && ptmc_state.running_state.child[0].threads.size() == 1 &&
                            read_guest(pid, &parent_word) == canary,
                            "unsupported clone3 TID/stack request did not reject before birth");
                }
            }
            initial.recover_running_state();
            auto extended = read_guest(pid, &job);
            extended.args_address = reinterpret_cast<uint64_t>(&extended_args);
            extended.args_size = sizeof extended_args;
            write_guest(pid, &job, extended);
            write_guest(pid, &extended_args.extra, uint64_t{0});
            auto extended_source = capture(initial);
            auto extended_born = step(extended_source, 0, SYS_clone3);
            require(extended_born.child[0].si[0].rval == 2 && extended_born.child[0].threads.size() == 2,
                    "zero-filled clone3 extension rejected");
        }
        cleanup();
    } catch (...) { cleanup(); throw; }
}
int main(int argc, char** argv) {
    bool tiny = false, clone3 = false;
    for (int i=1; i<argc; ++i) {
        tiny |= std::strcmp(argv[i], "--tiny-cache") == 0;
        clone3 |= std::strcmp(argv[i], "--clone3") == 0;
    }
    loglevel = 4;
    pid_t test = fork();
    if (!test) {
        setpgid(0,0);
        try { run(tiny, clone3); _exit(0); }
        catch (const std::exception& e) { std::fprintf(stderr,"thread TID: %s\n",e.what()); _exit(1); }
    }
    if (test < 0) return 1;
    setpgid(test,test);
    int status=0;
    bool done=false;
    for (int i=0; i<6000; ++i) {
        if (waitpid(test,&status,WNOHANG)==test) { done=true; break; }
        usleep(10000);
    }
    kill(-test,SIGKILL);
    if (!done) waitpid(test,&status,0);
    bool ok=done && WIFEXITED(status) && WEXITSTATUS(status)==0;
    std::printf("%s %s TID birth, join, clear-address replacement and persisted replay\n",
                ok?"PASS":"FAIL",clone3?"clone3":"clone");
    return ok?0:1;
}
