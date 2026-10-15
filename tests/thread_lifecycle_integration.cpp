#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "utils.h"
#include "proc_status.h"
#include <linux/futex.h>
#include "state/state_transition.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "engine/exec_engine.h"
#include "engine/thread_carrier.h"
#include "engine/thread_manager.h"
#include "syscall/dispatcher.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(NP == 1, "thread lifecycle fixture requires NP=1");
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

struct Job { unsigned long tid, magic, pid; };
struct TLS { unsigned long magic; };
static Job jobs[3]{};
static TLS tls[3]{{0x11112222}, {0x33334444}, {0x55556666}};
alignas(16) static unsigned char worker_stacks[3][8192]{};
static volatile long parent_tids[3]{};
static int main_exit_flag = 0;
static int clear_tid_word = 123;
static robust_list_head pending_robust{};
alignas(32) static std::array<unsigned char, 32> rseq_shaped_memory{};
alignas(32) static struct rseq registered_rseq{};

// Raw clone cannot return through libc/C on a new child stack. Only the parent
// pops its saved r12; the child executes this assembly until virtual SYS_exit.
extern "C" long lifecycle_spawn(Job*, void*, TLS*);
asm(R"(
.text
.globl lifecycle_spawn
.type lifecycle_spawn,@function
lifecycle_spawn:
 push %r12
 mov %rdi,%r12
 mov %rdx,%r8
 mov $0xd0f00,%edi
 xor %edx,%edx
 xor %r10d,%r10d
 mov $56,%eax
 syscall
 test %rax,%rax
 jz 1f
 pop %r12
 ret
1:
 mov $186,%eax
 syscall
 mov %rax,0(%r12)
 mov %fs:0,%rax
 mov %rax,8(%r12)
 mov $39,%eax
 syscall
 mov %rax,16(%r12)
 mov $60,%eax
 xor %edi,%edi
 syscall
 ud2
.size lifecycle_spawn,.-lifecycle_spawn
)");

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static void guest_program()
{
    parent_tids[0] = lifecycle_spawn(&jobs[0], worker_stacks[0] + sizeof worker_stacks[0], &tls[0]);
    parent_tids[1] = lifecycle_spawn(&jobs[1], worker_stacks[1] + sizeof worker_stacks[1], &tls[1]);
    syscall(SYS_getpid);
    parent_tids[2] = lifecycle_spawn(&jobs[2], worker_stacks[2] + sizeof worker_stacks[2], &tls[2]);
    for (;;) {
        if (main_exit_flag) {
            syscall(SYS_exit, 77);
            asm volatile("ud2");
        }
        syscall(SYS_getpid);
    }
}

static void wait_initial(pid_t child)
{
    for (int tick = 0; tick < 1000; ++tick) {
        int status = 0;
        const pid_t result = waitpid(child, &status, __WALL | WNOHANG);
        if (result == child) {
            require(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP, "initial breakpoint");
            return;
        }
        require(result == 0 || (result < 0 && errno == EINTR), "wait initial stop");
        usleep(1000);
    }
    throw std::runtime_error("initial stop deadline");
}

template<class T> static T read_guest(pid_t pid, const T *address)
{
    T result{};
    require(guest_read_exact(pid, address, &result, sizeof result), "read guest observation");
    return result;
}

static sys_state step(const sys_state& source, int actor, int expected_syscall)
{
    auto result = state_transition(source, 0, actor);
    require(!result.property_violation && result.code != CKPT_STOP && result.code != CKPT_DISCARD,
            "thread transition failed");
    require(result.new_state.child[0].si[actor].nr == expected_syscall, "unexpected thread syscall");
    return std::move(result.new_state);
}

static sys_state capture_current(const sys_state& source)
{
    syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
    std::memcpy(info[0], source.child[0].si, sizeof info[0]);
    return sys_state(info, &source);
}

static void dead_actor_rejected(const sys_state& source, int actor)
{
    const int cursor = ptmc_state.cursor;
    const auto running_hash = ptmc_state.running_state.ss_hash;
    const int selected = ptmc_state.current_thread_idx[0];
    bool rejected = false;
    try { (void)state_transition(source, 0, actor); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) == "Transition thread is not runnable";
    }
    require(rejected, "explicit exited actor was not rejected");
    require(ptmc_state.cursor == cursor && ptmc_state.running_state.ss_hash == running_hash &&
            ptmc_state.current_thread_idx[0] == selected,
            "invalid actor selection changed cursor or running state");
}

static void restore_roundtrip(const sys_state& expected)
{
    // Load through persisted machine and tracee metadata rather than retaining
    // runtime physical bindings in an in-memory copy.
    sys_state loaded(expected.ss_hash);
    loaded.recover_running_state();
    syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
    std::memcpy(info[0], loaded.child[0].si, sizeof info[0]);
    sys_state recaptured(info, &loaded);
    require(recaptured.ss_hash == expected.ss_hash && recaptured.ts_hash[0] == expected.ts_hash[0],
            "persisted lifecycle restore/recapture identity drift");
}

static void assert_worker(pid_t pid, int index, unsigned long logical_tid)
{
    const auto job = read_guest(pid, &jobs[index]);
    require(job.tid == logical_tid, "worker observed wrong logical gettid");
    require(job.magic == tls[index].magic, "worker TLS was not isolated/restored");
    require(job.pid == 1, "worker getpid differs from logical main thread");
}

// Registration is deliberately installed through the host-only injection path:
// guests in the unavailable-rseq model cannot create it through the dispatcher.
static void reject_active_rseq(pid_t carrier, const sys_state& current,
                               const sys_state& other)
{
    current.recover_running_state();
    const pid_t main = ptmc_state.pids[0];
    require(tracee_do_syscall(carrier, SYS_rseq,
            reinterpret_cast<uintptr_t>(&registered_rseq), sizeof registered_rseq,
            0, RSEQ_SIG, 0, 0) == 0, "register carrier rseq fixture");
    const auto tasks = get_thread_list(main);
    const auto saved_bytes = read_guest(main, &registered_rseq);
    const auto saved_job = read_guest(main, &jobs[0]);
    const auto saved_hash = ptmc_state.running_state.ss_hash;
    const auto saves = StateStore::instance().get_stats().save_calls;
    user_regs_struct before_main{}, before_carrier{};
    require(ptrace(PTRACE_GETREGS, main, nullptr, &before_main) == 0 &&
            ptrace(PTRACE_GETREGS, carrier, nullptr, &before_carrier) == 0,
            "read registered carrier registers");
    auto rejected_without_mutation = [&](auto operation) {
        bool rejected = false;
        try { operation(); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("active rseq registration") != std::string::npos;
        }
        user_regs_struct after_main{}, after_carrier{};
        require(ptrace(PTRACE_GETREGS, main, nullptr, &after_main) == 0 &&
                ptrace(PTRACE_GETREGS, carrier, nullptr, &after_carrier) == 0,
                "read rejected operation registers");
        const auto bytes = read_guest(main, &registered_rseq);
        const auto job = read_guest(main, &jobs[0]);
        require(rejected && get_thread_list(main) == tasks &&
                StateStore::instance().get_stats().save_calls == saves &&
                ptmc_state.running_state.ss_hash == saved_hash &&
                std::memcmp(&before_main, &after_main, sizeof before_main) == 0 &&
                std::memcmp(&before_carrier, &after_carrier, sizeof before_carrier) == 0 &&
                std::memcmp(&saved_bytes, &bytes, sizeof bytes) == 0 &&
                std::memcmp(&saved_job, &job, sizeof job) == 0,
                "active carrier rseq was accepted or rejection mutated state");
    };
    rejected_without_mutation([&] { (void)capture_current(current); });
    rejected_without_mutation([&] { current.recover_running_state(); });
    rejected_without_mutation([&] { other.recover_running_state(); });
    rejected_without_mutation([&] { (void)create_thread_carrier(carrier); });
    std::vector<maps_item> maps;
    rejected_without_mutation([&] {
        other.child[0].restore_memory_mappings(other.ts_hash[0], main, maps);
    });
    require(maps.empty(), "rejected mapping restore changed output mappings");
    rejected_without_mutation([&] {
        other.child[0].recover_mem_reg_snapshot(maps, other.ts_hash[0], main);
    });
    require(maps.empty(), "rejected memory restore changed input mappings");
    require(tracee_do_syscall(carrier, SYS_rseq,
            reinterpret_cast<uintptr_t>(&registered_rseq), sizeof registered_rseq,
            RSEQ_FLAG_UNREGISTER, RSEQ_SIG, 0, 0) == 0, "unregister carrier rseq fixture");
    ptmc_state.tracee_image_dirty[0] = true;
    current.recover_running_state();
}

static void roundtrip_unregistered_rseq_bytes(pid_t child, const sys_state& initial)
{
    // Every byte is ordinary branch memory when rseq is unavailable, including
    // cpu_id, rseq_cs, flags and extension/padding bytes. No live substitution.
    std::array<unsigned char, 32> first{}, second{};
    for (size_t i = 0; i < first.size(); ++i) {
        first[i] = static_cast<unsigned char>(0x20 + i);
        second[i] = static_cast<unsigned char>(0xe0 - i);
    }
    require(guest_write_exact(child, &rseq_shaped_memory, first.data(), first.size()),
            "write first rseq-shaped memory");
    auto a = capture_current(initial);
    auto copied_a = a;
    require(guest_write_exact(child, &rseq_shaped_memory, second.data(), second.size()),
            "write second rseq-shaped memory");
    auto b = capture_current(a);
    require(a.ss_hash != b.ss_hash && a.ts_hash[0] != b.ts_hash[0],
            "unregistered rseq bytes did not contribute to state identity");
    for (int iteration = 0; iteration < 2; ++iteration) {
        sys_state loaded(a.ss_hash);
        loaded.recover_running_state();
        require(read_guest(child, &rseq_shaped_memory) == first,
                "persisted restore substituted live rseq-shaped bytes");
        restore_roundtrip(loaded);
        b.recover_running_state();
        require(read_guest(child, &rseq_shaped_memory) == second,
                "second rseq-shaped branch did not restore exactly");
        copied_a.recover_running_state();
        require(read_guest(child, &rseq_shaped_memory) == first,
                "snapshot copy did not isolate rseq-shaped bytes");
    }
    initial.recover_running_state();
}

static void run_case(bool tiny)
{
    char directory[] = "/tmp/detsim-thread-lifecycle-XXXXXX";
    require(mkdtemp(directory), "temporary store directory");
    const auto previous = std::filesystem::current_path();
    require(chdir(directory) == 0, "enter temporary store directory");
    pid_t child = fork();
    require(child >= 0, "fork tracee before writer threads");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (__rseq_size && syscall(SYS_rseq,
            static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
            32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(2);
        syscall(SYS_set_tid_address, nullptr);
        syscall(SYS_set_robust_list, nullptr, sizeof(robust_list_head));
        pending_robust.list.next = &pending_robust.list;
        pending_robust.list_op_pending = reinterpret_cast<robust_list*>(&clear_tid_word);
        if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(3);
        auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
        code[0] = 0x0f; code[1] = 0x05;
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
        asm volatile("int3");
        guest_program();
        _exit(5);
    }
    try {
        wait_initial(child);
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr,
                PTRACE_O_TRACECLONE | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
        ptmc_state.pids[0] = child;
        ptmc_state.cursor = -1;
        ptmc_state.fd_managers[0] = std::make_shared<FdManager>();
        ptmc_state.fs_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        ptmc_state.sock_states[0].set_fd_manager(ptmc_state.fd_managers[0]);
        StateStore::Config config;
        if (tiny) config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        auto_mode = 1;
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
        sys_state initial(info);
        ptmc_state.running_state = initial;
        thread::init_all(ptmc_state);
        exec::init_all();
        roundtrip_unregistered_rseq_bytes(child, initial);

        // A host-only clear_child_tid registration must block logical clone
        // before adding either a guest slot or a physical carrier.
        initial.recover_running_state();
        (void)tracee_do_syscall(child, SYS_set_tid_address,
            reinterpret_cast<uintptr_t>(&clear_tid_word), 0, 0, 0, 0, 0);
        const auto original_tasks = get_thread_list(child);
        bool clear_rejected = false;
        try { (void)state_transition(initial, 0, 0); }
        catch (const std::runtime_error& error) {
            clear_rejected = std::string(error.what()).find("inherited clear_child_tid registration") != std::string::npos;
        }
        require(clear_rejected && get_thread_list(child) == original_tasks &&
                ptmc_state.running_state.child[0].threads.size() == 1 && read_guest(child, &jobs[0]).tid == 0,
                "registered clear_child_tid clone was not rejected before birth");
        (void)tracee_do_syscall(child, SYS_set_tid_address, 0, 0, 0, 0, 0, 0);

        if (__rseq_size) {
            initial.recover_running_state();
            const auto rseq_address = reinterpret_cast<uintptr_t>(
                static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset);
            require(tracee_do_syscall(child, SYS_rseq, rseq_address, 32, 0, RSEQ_SIG, 0, 0) == 0,
                    "register actual kernel rseq fixture");
            const auto saves_before_rseq = StateStore::instance().get_stats().save_calls;
            bool capture_rseq_rejected = false;
            try { (void)capture_current(initial); }
            catch (const std::runtime_error& error) {
                capture_rseq_rejected = std::string(error.what()).find("active rseq registration") != std::string::npos;
            }
            require(capture_rseq_rejected &&
                    StateStore::instance().get_stats().save_calls == saves_before_rseq,
                    "active rseq capture was accepted or emitted snapshot data");
            bool rseq_rejected = false;
            try { (void)state_transition(initial, 0, 0); }
            catch (const std::runtime_error& error) {
                rseq_rejected = std::string(error.what()).find("active rseq registration") != std::string::npos;
            }
            require(rseq_rejected && get_thread_list(child) == original_tasks &&
                    ptmc_state.running_state.child[0].threads.size() == 1,
                    "actual kernel rseq registration did not reject before birth");
            require(tracee_do_syscall(child, SYS_rseq, rseq_address, 32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG, 0, 0) == 0,
                    "unregister actual kernel rseq fixture");
            initial.recover_running_state();
        }

        // Capture must reject a pending syscall before reading/writing the
        // machine image or emitting store data. A rejected dispatcher entry
        // must then be cancellable without ever executing its real clone.
        initial.recover_running_state();
        require(ptrace(PTRACE_SYSCALL, child, nullptr, nullptr) == 0, "advance to pending clone entry");
        ptmc_state.tracee_image_dirty[0] = true;
        bool at_entry = false;
        for (int tick = 0; tick < 1000; ++tick) {
            int status = 0;
            const auto waited = waitpid(child, &status, __WALL | WNOHANG);
            if (waited == child) {
                require(WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80), "pending clone entry stop");
                at_entry = true;
                break;
            }
            require(waited == 0 || (waited < 0 && errno == EINTR), "wait pending clone entry");
            usleep(1000);
        }
        require(at_entry, "pending clone entry deadline");
        user_regs_struct entry_before{}, entry_after{};
        require(ptrace(PTRACE_GETREGS, child, nullptr, &entry_before) == 0 && entry_before.orig_rax == SYS_clone,
                "pending syscall is not clone");
        const auto saves_before = StateStore::instance().get_stats().save_calls;
        bool capture_rejected = false;
        try { (void)capture_current(initial); }
        catch (const std::runtime_error& error) {
            capture_rejected = std::string(error.what()).find("Snapshot capture requires a completed native syscall") != std::string::npos;
        }
        require(ptrace(PTRACE_GETREGS, child, nullptr, &entry_after) == 0, "read rejected capture registers");
        require(capture_rejected && StateStore::instance().get_stats().save_calls == saves_before &&
                std::memcmp(&entry_before, &entry_after, sizeof entry_before) == 0,
                "entry capture changed registers or emitted snapshot data");
        syscall_info invalid_clone{};
        invalid_clone.nr = SYS_clone;
        invalid_clone.args[0] = 0; // Deliberately invalid model flags, real entry remains untouched.
        bool flags_rejected = false;
        try { (void)syscall_dispatch::get()->on_enter(child, SYS_clone, invalid_clone); }
        catch (const std::runtime_error& error) {
            flags_rejected = std::string(error.what()) == "Unsupported raw clone thread flags";
        }
        require(flags_rejected, "invalid pending clone flags were accepted");
        initial.recover_running_state();
        require(get_thread_list(child) == original_tasks, "entry rollback accidentally executed real clone");

        auto after_a = step(initial, 0, SYS_clone);
        require(after_a.child[0].threads.size() == 2 && after_a.child[0].threads[1].alive,
                "clone A did not publish a live logical thread");
        require(read_guest(child, &jobs[0]).tid == 0 && read_guest(child, &jobs[0]).magic == 0,
                "newborn A executed guest instructions before clone checkpoint");
        require(after_a.child[0].threads[1].regs.rax == 0 &&
                after_a.child[0].threads[1].regs.fs_base == reinterpret_cast<uintptr_t>(&tls[0]),
                "newborn A clone return/TLS state incorrect");
        reject_active_rseq(child, after_a, initial);
        const pid_t a_carrier = ptmc_state.running_state.child[0].threads[1].physical_tid;
        reject_active_rseq(a_carrier, after_a, initial);
        auto after_b = step(after_a, 0, SYS_clone);
        require(after_b.child[0].threads.size() == 3 && read_guest(child, &jobs[1]).tid == 0,
                "clone B publication or newborn stop failed");
        require(read_guest(child, const_cast<const long*>(&parent_tids[0])) == 2,
                "parent clone return did not use logical child identity");

        auto a_tid = step(after_b, 1, SYS_gettid);
        require(a_tid.child[0].si[1].rval == 2, "A gettid result is physical");
        auto a_pid = step(a_tid, 1, SYS_getpid);
        auto after_a_exit = step(a_pid, 1, SYS_exit);
        require(!after_a_exit.child[0].threads[1].alive && after_a_exit.child[0].threads[0].alive &&
                after_a_exit.child[0].threads[2].alive, "thread exit terminated peers or lost its slot");
        assert_worker(child, 0, 2);
        reject_active_rseq(a_carrier, after_a_exit, initial);
        dead_actor_rejected(after_a_exit, 1);
        auto parent_boundary = step(after_a_exit, 0, SYS_getpid);
        require(read_guest(child, const_cast<const long*>(&parent_tids[1])) == 3,
                "parent observed wrong B clone ID");
        auto after_c = step(parent_boundary, 0, SYS_clone);
        require(after_c.child[0].threads.size() == 4 && after_c.child[0].threads[3].tid == 4 &&
                !after_c.child[0].threads[1].alive, "C reused the exited logical A slot");
        require(read_guest(child, &jobs[2]).tid == 0 && read_guest(child, &jobs[2]).magic == 0,
                "newborn C executed before clone checkpoint");
        require(get_thread_list(child).size() == 3,
                "C birth retained four physical tasks instead of reusing exited A carrier");
        auto b_tid = step(after_c, 2, SYS_gettid);
        auto b_pid = step(b_tid, 2, SYS_getpid);
        auto b_exit = step(b_pid, 2, SYS_exit);
        assert_worker(child, 1, 3);
        auto c_tid = step(b_exit, 3, SYS_gettid);
        auto c_pid = step(c_tid, 3, SYS_getpid);
        auto c_exit = step(c_pid, 3, SYS_exit);
        assert_worker(child, 2, 4);

        // Execute workers again after restoring a state whose dead A slot
        // causes the pool to bind surviving B/C onto different physical tasks.
        restore_roundtrip(after_c);
        auto rebound_b_tid = step(after_c, 2, SYS_gettid);
        auto rebound_b_pid = step(rebound_b_tid, 2, SYS_getpid);
        auto rebound_b_exit = step(rebound_b_pid, 2, SYS_exit);
        assert_worker(child, 1, 3);
        auto rebound_c_tid = step(rebound_b_exit, 3, SYS_gettid);
        auto rebound_c_pid = step(rebound_c_tid, 3, SYS_getpid);
        auto rebound_c_exit = step(rebound_c_pid, 3, SYS_exit);
        assert_worker(child, 2, 4);
        require(rebound_c_exit.ss_hash == c_exit.ss_hash, "rebound worker outcomes changed identity");

        // Main SYS_exit must preserve the physical anchor while workers live.
        after_c.recover_running_state();
        const int exit_requested = 1;
        require(guest_write_exact(child, &main_exit_flag, &exit_requested, sizeof exit_requested), "request main exit");
        auto main_exit_branch = capture_current(after_c);
        const long robust_result = tracee_do_syscall(child, SYS_set_robust_list,
            reinterpret_cast<uintptr_t>(&pending_robust), sizeof pending_robust, 0, 0, 0, 0);
        require(robust_result == 0, "install pending robust-list fixture");
        bool robust_rejected = false;
        try { (void)state_transition(main_exit_branch, 0, 0); }
        catch (const std::runtime_error& error) {
            robust_rejected = std::string(error.what()).find("unadopted native robust-list registration") != std::string::npos;
        }
        require(robust_rejected && ptmc_state.running_state.child[0].threads[0].alive,
                "pending robust owner exit mutated logical liveness");
        require(tracee_do_syscall(child, SYS_set_robust_list, 0, sizeof pending_robust, 0, 0, 0, 0) == 0,
                "clear robust-list fixture");
        auto main_exited = step(main_exit_branch, 0, SYS_exit);
        require(!main_exited.child[0].threads[0].alive && main_exited.child[0].threads[0].exit_code == 77 &&
                main_exited.child[0].threads[2].alive && main_exited.child[0].threads[3].alive &&
                DISALIVE(main_exited.status[0]), "main exit lost workers or exit status");
        restore_roundtrip(main_exited);
        ptmc_state.cursor = -1;
        dead_actor_rejected(main_exited, 0);
        auto live_b_tid = step(main_exited, 2, SYS_gettid);
        auto live_b_pid = step(live_b_tid, 2, SYS_getpid);
        auto live_b_exit = step(live_b_pid, 2, SYS_exit);
        assert_worker(child, 1, 3);
        auto live_c_tid = step(live_b_exit, 3, SYS_gettid);
        auto live_c_pid = step(live_c_tid, 3, SYS_getpid);
        auto last_exit = step(live_c_pid, 3, SYS_exit);
        assert_worker(child, 2, 4);
        require(DISEXITED(last_exit.status[0]) && DEXITSTATUS(last_exit.status[0]) == 0,
                "last worker did not propagate its exit status");
        restore_roundtrip(main_exited);
        auto main_exited_replay_b = step(main_exited, 2, SYS_gettid);
        require(main_exited_replay_b.ss_hash == live_b_tid.ss_hash,
                "restored main-exited state changed next worker outcome");

        // Alternate states with different logical thread sets while the host
        // retains every created carrier. Include backward and forward jumps.
        for (const auto* saved : {&initial, &after_c, &after_a, &after_a_exit, &after_b, &c_exit, &initial})
            restore_roundtrip(*saved);
        initial.recover_running_state();
        auto alias_clone = state_transition(ptmc_state.running_state, 0, 0);
        require(alias_clone.new_state.ss_hash == after_a.ss_hash &&
                alias_clone.new_state.child[0].si[0].nr == SYS_clone &&
                alias_clone.new_state.child[0].threads.size() == 2 &&
                alias_clone.new_state.child[0].threads[1].regs.rax == 0,
                "runtime-aliased transition source changed clone identity or newborn context");
        auto replay_a = step(initial, 0, SYS_clone);
        require(replay_a.ss_hash == after_a.ss_hash, "spare carriers changed replayed clone identity");
        auto replay_b = step(replay_a, 0, SYS_clone);
        require(replay_b.ss_hash == after_b.ss_hash, "spare carriers changed second clone identity");
        auto replay_tid = step(replay_b, 1, SYS_gettid);
        auto replay_pid = step(replay_tid, 1, SYS_getpid);
        auto replay_exit = step(replay_pid, 1, SYS_exit);
        require(replay_exit.ss_hash == after_a_exit.ss_hash, "carrier rebinding changed TLS/exit replay");
        assert_worker(child, 0, 2);

        kill(child, SIGKILL);
        while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        std::filesystem::current_path(previous);
        std::filesystem::remove_all(directory);
    } catch (...) {
        kill(child, SIGKILL);
        while (waitpid(-1, nullptr, __WALL) > 0 || errno == EINTR) {}
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        std::filesystem::current_path(previous);
        std::filesystem::remove_all(directory);
        throw;
    }
}

int main(int argc, char**)
{
    loglevel = 4;
    const pid_t test = fork();
    if (!test) {
        setpgid(0, 0);
        try { run_case(argc > 1); _exit(0); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "thread lifecycle: %s\n", error.what()); _exit(1);
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
    std::printf("%s raw-clone logical lifecycle, TLS, persisted restore and carrier reuse\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
