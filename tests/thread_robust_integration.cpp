#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "utils.h"
#include "proc_status.h"
#include "state/state_transition.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include "engine/thread_pool.h"
#include <linux/futex.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static_assert(NP == 1);
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
struct Lock { robust_list link; uint32_t word; uint32_t canary; };
struct Ownership { robust_list_head head; Lock lock; Lock pending; };
static Ownership ownership{}, main_ownership{};
static int exit_group_requested;
struct Job { uint64_t head; long result; };
static Job job{};
alignas(16) static unsigned char stack[16384];
static int wait_operation = -1;
static uint32_t wait_value;
extern "C" long robust_spawn(Job*, void*);
asm(R"(
.text
.globl robust_spawn
.type robust_spawn,@function
robust_spawn:
 push %r12
 mov %rdi,%r12
 xor %edx,%edx
 xor %r10d,%r10d
 xor %r8d,%r8d
 mov $0x50f00,%edi
 mov $56,%eax
 syscall
 test %rax,%rax
 jz 1f
 pop %r12
 ret
1:
 mov 0(%r12),%rdi
 mov $24,%esi
 mov $273,%eax
 syscall
 mov %rax,8(%r12)
 mov $186,%eax
 syscall
 mov $60,%eax
 xor %edi,%edi
 syscall
 ud2
.size robust_spawn,.-robust_spawn
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
    require(copied.ss_hash == expected.ss_hash, "persisted robust restore identity drift");
}
// Use the same layouts in a native process and in the modeled address space.
// Absolute link addresses refer to destination, not this local temporary copy.
static Ownership layout(Ownership* destination, uint32_t owner, int kind) {
    Ownership result{};
    result.head.list.next = &destination->lock.link;
    result.head.futex_offset = offsetof(Lock, word);
    result.lock.link.next = &destination->head.list;
    result.pending.link.next = &destination->head.list;
    result.lock.word = owner | FUTEX_WAITERS;
    result.pending.word = owner | FUTEX_WAITERS;
    result.lock.canary = result.pending.canary = 0x12345678;
    if (kind == 1) { // pending operation not yet linked
        result.head.list.next = &destination->head.list;
        result.head.list_op_pending = &destination->pending.link;
    }
    if (kind == 2) result.lock.word = 17 | FUTEX_WAITERS; // another owner
    if (kind == 3) result.head.list.next = reinterpret_cast<robust_list*>(2); // inaccessible
    if (kind == 4) result.lock.link.next = &destination->lock.link; // bounded cycle
    if (kind == 5) result.head.list_op_pending = &destination->lock.link; // pending also linked
    if (kind == 6) { // futex overlaps next pointer; the old next is fetched first
        result.head.futex_offset = 0;
        result.lock.link.next = reinterpret_cast<robust_list*>(uint64_t(owner | FUTEX_WAITERS));
    }
    if (kind == 7) { // unlock completed, pending operation only needs a wake
        result.head.list.next = &destination->head.list;
        result.head.list_op_pending = &destination->pending.link;
        result.pending.word = 0;
    }
    return result;
}
static void verify_words(const Ownership& result, uint32_t owner, int kind) {
    const uint32_t dead = FUTEX_WAITERS | FUTEX_OWNER_DIED;
    const uint32_t expected = kind == 1 || kind == 3 || kind == 6 || kind == 7 ? owner | FUTEX_WAITERS :
                              kind == 2 ? 17 | FUTEX_WAITERS : dead;
    require(result.lock.word == expected, "robust list owner-death word differs from native ABI");
    require(result.pending.word == (kind == 7 ? 0 : kind == 1 ? dead : owner | FUTEX_WAITERS),
            "robust pending-operation owner-death word differs from native ABI");
    if (kind == 6)
        require(reinterpret_cast<uint64_t>(result.lock.link.next) == dead,
                "robust traversal did not preserve next-pointer fetch order");
    require(result.lock.canary == 0x12345678 && result.pending.canary == 0x12345678,
            "robust owner-death wrote beyond the 32-bit futex");
}
static void native_oracle() {
    auto* shared = static_cast<Ownership*>(mmap(nullptr, 4096, PROT_READ|PROT_WRITE,
                                               MAP_SHARED|MAP_ANONYMOUS, -1, 0));
    require(shared != MAP_FAILED, "native oracle mapping");
    for (int kind = 0; kind < 8; ++kind) {
        const pid_t child = fork();
        require(child >= 0, "native oracle fork");
        if (!child) {
            *shared = layout(shared, syscall(SYS_gettid), kind);
            if (syscall(SYS_set_robust_list, &shared->head, sizeof shared->head)) _exit(2);
            syscall(SYS_exit, 0);
            _exit(3);
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "native robust oracle exit");
        verify_words(*shared, child, kind);
    }
    require(munmap(shared, 4096) == 0, "native oracle unmap");
}
static void run(bool tiny) {
    native_oracle();
    job.head = reinterpret_cast<uint64_t>(&ownership.head);
    char directory[] = "/tmp/detsim-thread-robust-XXXXXX";
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
        syscall(SYS_set_tid_address, nullptr);
        // Native startup metadata is empty; model registration happens in the guest.
        syscall(SYS_set_robust_list, nullptr, sizeof(robust_list_head));
        if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                 MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(3);
        auto code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
        code[0] = 0x0f; code[1] = 0x05;
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
        asm volatile("int3");
        for (;;) {
            robust_spawn(&job, stack + sizeof stack);
            if (exit_group_requested) syscall(SYS_exit_group, 23);
            if (wait_operation >= 0)
                syscall(SYS_futex, &ownership.lock.word, wait_operation, wait_value, nullptr, nullptr, 0);
            syscall(SYS_getpid);
        }
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
        // Bootstrap may transfer an empty native head, but physical owner IDs
        // in a nonempty head cannot be silently converted into logical IDs.
        write_guest(pid, &main_ownership, layout(&main_ownership, pid, 0));
        require(tracee_do_syscall(pid, SYS_set_robust_list,
                    reinterpret_cast<uint64_t>(&main_ownership.head), sizeof(robust_list_head),
                    0, 0, 0, 0) == 0, "install bootstrap native robust head");
        bool adoption_rejected = false;
        try { adopt_thread_robust_list(0, pid); }
        catch (const std::runtime_error& error) {
            adoption_rejected = std::string(error.what()).find("Cannot adopt nonempty") != std::string::npos;
        }
        robust_list_head* native_head = nullptr;
        size_t native_size = 0;
        require(syscall(SYS_get_robust_list, pid, &native_head, &native_size) == 0 &&
                native_head == &main_ownership.head && adoption_rejected &&
                read_guest(pid, &main_ownership).lock.word == (uint32_t(pid) | FUTEX_WAITERS),
                "bootstrap rejection detached or changed native robust ownership");
        auto empty = layout(&main_ownership, 1, 0);
        empty.head.list.next = &main_ownership.head.list;
        write_guest(pid, &main_ownership, empty);
        adopt_thread_robust_list(0, pid);
        require(syscall(SYS_get_robust_list, pid, &native_head, &native_size) == 0 && !native_head &&
                ptmc_state.running_state.child[0].threads[0].robust_list_head ==
                    reinterpret_cast<uint64_t>(&main_ownership.head),
                "bootstrap did not transfer empty robust registration into logical state");
        StateStore::Config config;
        if (tiny) config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        auto_mode = 1;
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
        sys_state initial(info);
        ptmc_state.running_state = initial;
        thread::init_all(ptmc_state);
        exec::init_all();
        write_guest(pid, &main_ownership, layout(&main_ownership, 1, 0));
        require(emulate_set_robust_list(0, pid, reinterpret_cast<uint64_t>(&main_ownership.head),
                                      sizeof(robust_list_head)) == 0,
                "register logical parent robust list");
        initial = capture(initial);
        auto born = step(initial, 0, SYS_clone);
        require(born.child[0].threads[1].robust_list_head == 0, "newborn inherited robust registration");
        for (int kind = 0; kind < 8; ++kind) {
            born.recover_running_state();
            write_guest(pid, &ownership, layout(&ownership, 2, kind));
            auto source = capture(born);
            auto registered = step(source, 1, SYS_set_robust_list);
            require(registered.child[0].si[1].rval == 0 &&
                    registered.child[0].threads[1].robust_list_head == job.head,
                    "set_robust_list registration failed");
            auto before_exit = step(registered, 1, SYS_gettid);
            require(read_guest(pid, &job).result == 0, "guest saw wrong registration result");
            auto exited = step(before_exit, 1, SYS_exit);
            verify_words(read_guest(pid, &ownership), 2, kind);
            require(!exited.child[0].threads[1].alive &&
                    exited.child[0].threads[1].robust_list_head == 0,
                    "robust owner remained alive or retained registration");
            roundtrip(registered);
            roundtrip(before_exit);
            roundtrip(exited);
            auto replay = step(sys_state(before_exit.ss_hash), 1, SYS_exit);
            require(replay.ss_hash == exited.ss_hash, "persisted robust exit changed branch identity");
            verify_words(read_guest(pid, &ownership), 2, kind);
        }
        // Unsupported PI ownership must be detected before earlier ordinary
        // nodes are modified. The next branch can restore and continue normally.
        born.recover_running_state();
        auto mixed = layout(&ownership, 2, 0);
        mixed.lock.link.next = reinterpret_cast<robust_list*>(
            reinterpret_cast<uint64_t>(&ownership.pending.link) | 1);
        write_guest(pid, &ownership, mixed);
        auto pi_registered = step(capture(born), 1, SYS_set_robust_list);
        auto pi_before_exit = step(pi_registered, 1, SYS_gettid);
        bool pi_rejected = false;
        try { (void)step(pi_before_exit, 1, SYS_exit); }
        catch (const std::runtime_error& error) {
            pi_rejected = std::string(error.what()).find("PI robust") != std::string::npos;
        }
        const auto after_pi = read_guest(pid, &ownership);
        require(pi_rejected && ptmc_state.running_state.child[0].threads[1].alive &&
                std::memcmp(&after_pi, &mixed, sizeof mixed) == 0,
                "PI robust rejection partially changed earlier ownership or liveness");
        // Robust death wakes a shared-key waiter even when application waits used
        // the same process address; a private-key waiter must remain asleep.
        for (int wait_case = 0; wait_case < 4; ++wait_case) {
            const bool private_wait = wait_case == 1;
            const bool stays_blocked = wait_case == 1 || wait_case == 2;
            const uint32_t owner = wait_case == 3 ? 0 : wait_case == 2 ? 2 : 2 | FUTEX_WAITERS;
            born.recover_running_state();
            auto wait_layout = layout(&ownership, 2, 0);
            wait_layout.lock.word = owner;
            if (wait_case == 3) {
                wait_layout.head.list.next = &ownership.head.list;
                wait_layout.head.list_op_pending = &ownership.lock.link;
            }
            write_guest(pid, &ownership, wait_layout);
            write_guest(pid, &wait_operation, FUTEX_WAIT | (private_wait ? FUTEX_PRIVATE_FLAG : 0));
            write_guest(pid, &wait_value, owner);
            auto source = capture(born);
            auto waiting = step(source, 0, SYS_futex);
            require(ptmc_state.thread_blocked[0][0], "robust waiter did not block");
            auto registered = step(waiting, 1, SYS_set_robust_list);
            auto before_exit = step(registered, 1, SYS_gettid);
            auto exited = step(before_exit, 1, SYS_exit);
            require(ptmc_state.thread_blocked[0][0] == stays_blocked,
                    "robust owner death used the wrong futex wake key");
            if (wait_case == 3)
                require(read_guest(pid, &ownership).lock.word == 0,
                        "pending unlock spuriously marked an unowned futex OWNER_DIED");
            roundtrip(before_exit);
            auto replay = step(sys_state(before_exit.ss_hash), 1, SYS_exit);
            require(replay.ss_hash == exited.ss_hash && ptmc_state.thread_blocked[0][0] == stays_blocked,
                    "restored robust waiter lost wake semantics");
            const auto& pending = replay.child[0].threads[0].pending_syscall;
            require(pending && pending->phase == (stays_blocked ? PendingPhase::Waiting : PendingPhase::Ready),
                    "robust cleanup lost pending wait phase");
            if (!stays_blocked) {
                auto completed = step(replay, 0, SYS_futex);
                require(!completed.child[0].threads[0].pending_syscall && completed.child[0].si[0].rval == 0 &&
                        completed.child[0].threads[0].regs.rip == replay.child[0].threads[0].regs.rip,
                        "robust wake completion ran user instructions or lost success");
                require(step(sys_state(replay.ss_hash), 0, SYS_futex).ss_hash == completed.ss_hash,
                        "robust wake completion replay changed identity");
            }
        }
        // exit_group must clean every logical owner, including a worker that
        // is stopped before its own SYS_exit. Restore the live branch afterward.
        born.recover_running_state();
        write_guest(pid, &ownership, layout(&ownership, 2, 0));
        write_guest(pid, &exit_group_requested, 1);
        auto group_registered = step(capture(born), 1, SYS_set_robust_list);
        auto group_exited = step(group_registered, 0, SYS_exit_group);
        verify_words(read_guest(pid, &ownership), 2, 0);
        verify_words(read_guest(pid, &main_ownership), 1, 0);
        require(!group_exited.child[0].threads[0].alive &&
                !group_exited.child[0].threads[1].alive,
                "exit_group did not retire all robust owners");
        roundtrip(group_registered);
        roundtrip(group_exited);
        require(step(sys_state(group_registered.ss_hash), 0, SYS_exit_group).ss_hash == group_exited.ss_hash,
                "persisted exit_group robust cleanup changed identity");
        // Ordered group cleanup: the preceding owner's clear_child_tid can
        // remove the next owner's futex TID before robust cleanup sees it.
        group_registered.recover_running_state();
        record_logical_clear_tid(0, pid, reinterpret_cast<uint64_t>(&ownership.lock.word));
        auto clear_alias = capture(group_registered);
        auto clear_alias_exit = step(clear_alias, 0, SYS_exit_group);
        require(read_guest(pid, &ownership).lock.word == 0,
                "group robust plan ignored preceding clear_child_tid alias");
        roundtrip(clear_alias);
        require(step(sys_state(clear_alias.ss_hash), 0, SYS_exit_group).ss_hash == clear_alias_exit.ss_hash,
                "group clear-tid alias replay changed identity");

        // A newborn need not run user code before group exit. Its deferred
        // CHILD_SETTID still happens in slot order after the main owner's death.
        born.recover_running_state();
        write_guest(pid, &exit_group_requested, 1);
        auto& newborn = ptmc_state.running_state.child[0].threads[1];
        newborn.child_settid_pending = true;
        newborn.pending_child_settid = reinterpret_cast<uint64_t>(&main_ownership.lock.word);
        auto pending_start = capture(born);
        auto pending_group_exit = step(pending_start, 0, SYS_exit_group);
        require(read_guest(pid, &main_ownership).lock.word == 2 &&
                !pending_group_exit.child[0].threads[1].child_settid_pending,
                "group robust plan lost deferred CHILD_SETTID ordering");
        roundtrip(pending_start);
        require(step(sys_state(pending_start.ss_hash), 0, SYS_exit_group).ss_hash == pending_group_exit.ss_hash,
                "group deferred-start replay changed identity");

        // A later PI owner rejects the whole group before earlier owners' words
        // or liveness are changed, even though their cleanup plan is valid.
        group_registered.recover_running_state();
        auto group_pi = layout(&ownership, 2, 0);
        group_pi.head.list.next = reinterpret_cast<robust_list*>(
            reinterpret_cast<uint64_t>(&ownership.lock.link) | 1);
        write_guest(pid, &ownership, group_pi);
        auto group_pi_source = capture(group_registered);
        const auto main_before = read_guest(pid, &main_ownership);
        bool group_pi_rejected = false;
        try { (void)step(group_pi_source, 0, SYS_exit_group); }
        catch (const std::runtime_error& error) {
            group_pi_rejected = std::string(error.what()).find("PI robust") != std::string::npos;
        }
        const auto main_after = read_guest(pid, &main_ownership);
        const auto worker_after = read_guest(pid, &ownership);
        require(group_pi_rejected && ptmc_state.running_state.child[0].threads[0].alive &&
                ptmc_state.running_state.child[0].threads[1].alive &&
                std::memcmp(&main_before, &main_after, sizeof main_before) == 0 &&
                std::memcmp(&group_pi, &worker_after, sizeof group_pi) == 0,
                "later PI owner partially mutated preceding group owners");
        // Reuse the exited carrier for a fresh logical owner, then restore the
        // first generation. Registration cannot be retained in host task state.
        born.recover_running_state();
        write_guest(pid, &ownership, layout(&ownership, 2, 0));
        auto registered = step(capture(born), 1, SYS_set_robust_list);
        auto before_exit = step(registered, 1, SYS_gettid);
        auto exited = step(before_exit, 1, SYS_exit);
        auto boundary = step(exited, 0, SYS_getpid);
        auto reborn = step(boundary, 0, SYS_clone);
        require(get_thread_list(pid).size() == 2 &&
                reborn.child[0].threads[2].robust_list_head == 0,
                "robust exit prevented carrier reuse or leaked registration");
        write_guest(pid, &ownership, layout(&ownership, 3, 0));
        auto new_registered = step(capture(reborn), 2, SYS_set_robust_list);
        auto new_before_exit = step(new_registered, 2, SYS_gettid);
        auto new_exited = step(new_before_exit, 2, SYS_exit);
        verify_words(read_guest(pid, &ownership), 3, 0);
        for (const auto* saved : {&registered, &reborn, &new_registered, &new_exited, &before_exit})
            roundtrip(*saved);
        require(step(before_exit, 1, SYS_exit).ss_hash == exited.ss_hash,
                "carrier reuse contaminated old robust registration");
        cleanup();
    } catch (...) { cleanup(); throw; }
}
int main(int argc, char**) {
    loglevel = 4;
    const pid_t test = fork();
    if (!test) {
        setpgid(0, 0);
        try { run(argc > 1); _exit(0); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "thread robust: %s\n", error.what()); _exit(1);
        }
    }
    if (test < 0) return 1;
    setpgid(test, test);
    int status = 0;
    bool done = false;
    for (int tick = 0; tick < 6000; ++tick) {
        if (waitpid(test, &status, WNOHANG) == test) { done = true; break; }
        usleep(10000);
    }
    kill(-test, SIGKILL);
    if (!done) waitpid(test, &status, 0);
    const bool passed = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s robust registration, native owner-death ABI, persisted restore and carrier reuse\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
