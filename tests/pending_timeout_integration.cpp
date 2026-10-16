#include "api/plugin.h"
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/syscall.h>

struct TimeoutMonitor {
    uint64_t completions = 0;
    detsim_event_v2 event{};
};

#ifdef DETSIM_TIMEOUT_PLUGIN
namespace {
bool addresses(const detsim_host_v2* host, uint64_t& marker, uint64_t& fail) {
    unsigned long long a = 0, b = 0;
    if (std::sscanf(host->configuration, "%llu,%llu", &a, &b) != 2) return false;
    marker = a; fail = b; return true;
}
detsim_verdict initialize(const detsim_host_v2*, const detsim_state_view*, const char*,
                          detsim_writer out, detsim_diagnostic) {
    const TimeoutMonitor zero{};
    return out.append(out.context, &zero, sizeof zero) ? DETSIM_ERROR : DETSIM_PASS;
}
detsim_verdict advance(const detsim_host_v2* host, const detsim_state_view* before,
    const detsim_state_view* after, const detsim_event_v2* event, detsim_bytes previous,
    detsim_writer out, detsim_diagnostic) {
    if (previous.size != sizeof(TimeoutMonitor)) return DETSIM_ERROR;
    TimeoutMonitor next{};
    std::memcpy(&next, previous.data, sizeof next);
    if (event->choice == DETSIM_CHOICE_TIMEOUT) {
        uint64_t marker, fail;
        long old_marker = -1, new_marker = -2;
        if (!addresses(host, marker, fail) ||
            host->read(before, event->process, marker, &old_marker, sizeof old_marker) ||
            host->read(after, event->process, marker, &new_marker, sizeof new_marker))
            return DETSIM_UNAVAILABLE;
        if (old_marker != new_marker || event->phase != DETSIM_SYSCALL_COMPLETED ||
            !event->result_valid || event->result != (event->syscall_number == SYS_futex ? -ETIMEDOUT : 0))
            return DETSIM_ERROR;
        ++next.completions;
        next.event = *event;
    }
    return out.append(out.context, &next, sizeof next) ? DETSIM_ERROR : DETSIM_PASS;
}
detsim_verdict edge(const detsim_host_v2* host, const detsim_state_view*,
    const detsim_state_view* after, const detsim_event_v2* event, detsim_diagnostic) {
    if (event->choice != DETSIM_CHOICE_TIMEOUT) return DETSIM_PASS;
    uint64_t marker, fail;
    int violate = 0;
    if (!addresses(host, marker, fail) || host->read(after, event->process, fail, &violate, sizeof violate))
        return DETSIM_UNAVAILABLE;
    return (violate & 1) ? DETSIM_VIOLATION : DETSIM_PASS;
}
detsim_verdict predicate(const detsim_host_v2* host, const detsim_state_view* view, detsim_diagnostic) {
    uint64_t marker, fail;
    int violate = 0;
    if (!addresses(host, marker, fail) || host->read(view, 0, fail, &violate, sizeof violate))
        return DETSIM_UNAVAILABLE;
    return (violate & 2) ? DETSIM_VIOLATION : DETSIM_PASS;
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2),
    "test.pending-timeout", 1, predicate, edge, initialize, advance, nullptr, nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2() { return &descriptor; }
#else
#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "io.h"
#include "pending_io.h"
#include "plugins.h"
#include "state/state_transition.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include "engine/thread_pool.h"
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <sstream>
#include "cereal/archives/binary.hpp"
#include <stdexcept>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
extern TSS state_tree;
static volatile long marker = 0;
static int violate = 0;
static int word = 7, epfd = -1;
static int pipefd[2];
static char payload = 'x';
static epoll_event events[2];
static epoll_event interest{EPOLLIN, {.u64 = 99}};
static timespec request{0, 1234};
static timespec select_request{0, 1234};
static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
static sys_state capture() {
    syscall_info calls[NP][MAX_THREADS_PER_PROCESS]{};
    for (int node = 0; node < NP; ++node)
        std::memcpy(calls[node], ptmc_state.running_state.child[node].si, sizeof calls[node]);
    return sys_state(calls, &ptmc_state.running_state);
}
static sys_state step(const sys_state& source, int nr) {
    auto result = state_transition(source, 0, 0);
    require(!result.property_violation && result.code == CKPT_YES &&
            result.new_state.child[0].si[0].nr == nr, "unexpected normal boundary");
    return std::move(result.new_state);
}
static TimeoutMonitor monitor(const sys_state& state) {
    require(state.monitor_states.size() == 1 &&
            state.monitor_states.begin()->second.size() == sizeof(TimeoutMonitor), "missing monitor bytes");
    TimeoutMonitor result{};
    std::memcpy(&result, state.monitor_states.begin()->second.data(), sizeof result);
    return result;
}
static std::string live_semantics() {
    std::ostringstream bytes;
    cereal::BinaryOutputArchive archive(bytes);
    archive(ptmc_state.running_state.child[0], ptmc_state.fs_states[0],
            ptmc_state.sock_states[0], *ptmc_state.fd_managers[0]);
    for (int slot = 0; slot < MAX_THREADS_PER_PROCESS; ++slot)
        archive(ptmc_state.thread_blocked[0][slot]);
    return bytes.str();
}
static void reject_without_mutation(const sys_state& source, const char* why) {
    user_regs_struct before{}, after{};
    require(ptrace(PTRACE_GETREGS, ptmc_state.pids[0], nullptr, &before) == 0, "read registers before reject");
    const auto time = ptmc_state.time[0];
    const auto runtime_hash = ptmc_state.running_state.ss_hash;
    const auto semantics = live_semantics();
    const auto tree_size = state_tree.size();
    bool rejected = false;
    try { state_transition(source, 0, 0, TransitionAction::Timeout); }
    catch (const ActionRejected&) { rejected = true; }
    require(rejected, why);
    require(ptrace(PTRACE_GETREGS, ptmc_state.pids[0], nullptr, &after) == 0 &&
            !std::memcmp(&before, &after, sizeof before) &&
            ptmc_state.time[0].tv_sec == time.tv_sec && ptmc_state.time[0].tv_usec == time.tv_usec &&
            ptmc_state.running_state.ss_hash == runtime_hash && state_tree.size() == tree_size &&
            live_semantics() == semantics,
            "rejected action changed registers, time, pending/futex state, resource queues or history");
}
static sys_state timeout_and_replay(const sys_state& source, long expected) {
    const auto& pending = *source.child[0].threads[0].pending_syscall;
    const auto before = monitor(source);
    const auto deadline = *pending.deadline;
    int64_t sec = deadline.sec, usec = (static_cast<int64_t>(deadline.nsec) + 999) / 1000;
    if (usec == 1000000) { ++sec; usec = 0; }
    const auto original_time = source.child[0].tv;
    if (original_time.tv_sec > sec || (original_time.tv_sec == sec && original_time.tv_usec > usec)) {
        sec = original_time.tv_sec; usec = original_time.tv_usec;
    }
    hash_type hash = 0;
    sys_state completed;
    for (int replay = 0; replay < 3; ++replay) {
        auto disk_source = sys_state(source.ss_hash);
        auto result = state_transition(disk_source, 0, 0, TransitionAction::Timeout);
        completed = std::move(result.new_state);
        require(result.code == CKPT_YES && !result.property_violation && !completed.property_failed,
                "timeout did not publish a successful transition");
        const auto& actor = completed.child[0].threads[0];
        require(!actor.pending_syscall && actor.regs.rip == source.child[0].threads[0].regs.rip &&
                static_cast<long>(actor.regs.rax) == expected &&
                static_cast<long>(completed.child[0].si[0].rval) == expected,
                "timeout resumed guest code or lost its syscall return");
        require(completed.child[0].tv.tv_sec == sec && completed.child[0].tv.tv_usec == usec,
                "timeout clock did not ceil deadline monotonically");
        const auto record = monitor(completed);
        require(record.completions == before.completions + 1 &&
                record.event.process == 0 && record.event.thread == 0 &&
                record.event.choice == DETSIM_CHOICE_TIMEOUT && record.event.syscall_number == pending.nr &&
                record.event.result_valid && record.event.result == expected &&
                record.event.phase == DETSIM_SYSCALL_COMPLETED &&
                !std::memcmp(record.event.args, pending.args, sizeof pending.args),
                "public event/monitor did not preserve timeout actor, syscall, arguments and result");
        const auto edge = state_tree.at(completed.ss_hash);
        require(std::get<0>(edge) == source.ss_hash && std::get<1>(edge) == 0 &&
                std::get<2>(edge) == DETSIM_CHOICE_TIMEOUT && std::get<3>(edge) == 0,
                "published TSS timeout edge lost action or actor");
        require(monitor(sys_state(source.ss_hash)).completions == before.completions,
                "monitor output mutated predecessor branch");
        require(monitor(sys_state(completed.ss_hash)).completions == record.completions,
                "disk successor lost monitor bytes");
        if (!replay) hash = completed.ss_hash;
        else require(completed.ss_hash == hash, "timeout replay changed successor identity");
    }
    return completed;
}
static void clock_case(const sys_state& source, timeval clock, int64_t sec, int32_t ns, const char* name) {
    source.recover_running_state();
    ptmc_state.time[0] = clock;
    auto& deadline = *ptmc_state.running_state.child[0].threads[0].pending_syscall->deadline;
    deadline.sec = sec; deadline.nsec = ns;
    auto edited = capture();
    timeout_and_replay(edited, -ETIMEDOUT);
    std::printf("PASS %s\n", name);
}
int main(int argc, char** argv) {
    pid_t children[NP]{};
    char directory[] = "/tmp/detsim-pending-timeout-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    auto cleanup = [&] {
        for (pid_t child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, __WALL); }
        detsim::plugins::reset();
        StateStore::instance().shutdown(); SysStateStore::instance().shutdown();
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
    };
    try {
        require(argc == 2 || argc == 3, "usage: pending_timeout_integration PLUGIN [--tiny-cache]");
        const auto plugin = std::filesystem::absolute(argv[1]);
        loglevel = 4;
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        for (int node = 0; node < NP; ++node) {
            children[node] = fork(); require(children[node] >= 0, "fork");
            if (!children[node]) {
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (__rseq_size && syscall(SYS_rseq,
                    static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                    32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(2);
                if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(3);
                if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(4);
                auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
                code[0] = 0x0f; code[1] = 0x05;
                if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(5);
                asm volatile("int3");
                if (!node) {
                    syscall(SYS_futex, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 7, &request, 0, 0);
                    marker = 1; syscall(SYS_getpid);
                    syscall(SYS_nanosleep, &request, nullptr);
                    marker = 2; syscall(SYS_getpid);
                    syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &request, nullptr);
                    marker = 3; syscall(SYS_getpid);
                    syscall(SYS_pselect6, 0, nullptr, nullptr, nullptr, &select_request, nullptr);
                    marker = 4; syscall(SYS_getpid);
                    syscall(SYS_epoll_wait, epfd, events, 2, 1);
                    marker = 5; syscall(SYS_getpid);
                    syscall(SYS_epoll_pwait, epfd, events, 2, 1, nullptr, 8);
                    marker = 6; syscall(SYS_getpid);
                    syscall(SYS_epoll_pwait2, epfd, events, 2, &request, nullptr, 8);
                    marker = 7; syscall(SYS_getpid);
                }
                for (;;) syscall(SYS_getpid);
            }
            int status;
            require(waitpid(children[node], &status, 0) == children[node] && WIFSTOPPED(status), "initial stop");
            require(ptrace(PTRACE_SETOPTIONS, children[node], nullptr,
                    PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
            ptmc_state.pids[node] = children[node];
            ptmc_state.fd_managers[node] = std::make_shared<FdManager>();
            ptmc_state.fs_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
            ptmc_state.sock_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
            adopt_clear_child_tid(node, children[node]);
        }
        ptmc_state.cursor = 0;
        epfd = ptmc_state.sock_states[0].do_epoll_create1(0);
        require(epfd >= 0 && guest_write_exact(children[0], &epfd, &epfd, sizeof epfd), "install epoll descriptor");
        require(ptmc_state.fs_states[0].do_pipe(pipefd, 0) == 0, "create readiness pipe");
        require(ptmc_state.sock_states[0].do_epoll_ctl(epfd, EPOLL_CTL_ADD, pipefd[0], &interest) == 0,
                "register readiness pipe");
        StateStore::Config config;
        if (argc == 3 && std::string(argv[2]) == "--tiny-cache")
            config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        const auto configuration = std::to_string(reinterpret_cast<uintptr_t>(&marker)) + "," +
            std::to_string(reinterpret_cast<uintptr_t>(&violate));
        detsim::plugins::load(plugin.string(), configuration);
        auto_mode = 1;
        auto initial = capture();
        require(!detsim::plugins::check_initial(initial), "initialize monitor");
        initial.commit_metadata(); ptmc_state.running_state = initial;
        thread::init_all(ptmc_state); exec::init_all();
        auto waiting = step(initial, SYS_futex);
        require(waiting.child[0].threads[0].pending_syscall &&
                waiting.child[0].threads[0].pending_syscall->deadline, "futex did not suspend with deadline");
        reject_without_mutation(initial, "nonpending source accepted timeout");
        waiting.recover_running_state();
        ptmc_state.running_state.child[0].threads[0].pending_syscall->deadline.reset();
        auto infinite = capture();
        reject_without_mutation(infinite, "infinite futex accepted timeout");
        waiting.recover_running_state(); ready_syscall(0, 1, 0);
        auto ready = capture();
        reject_without_mutation(ready, "Ready futex accepted timeout");
        clock_case(waiting, {10, 0}, 10, 999999999, "nanosecond deadline carries into next second");
        clock_case(waiting, {10, 0}, 9223372036LL, 854775807, "KTIME_MAX rounds safely to microseconds");
        clock_case(waiting, {50, 1}, 1, 1, "overdue timeout never moves time backwards");
        auto completed = timeout_and_replay(waiting, -ETIMEDOUT);
        std::puts("PASS timed futex completion and replay");
        auto resumed = step(completed, SYS_getpid);
        long value;
        require(guest_read_exact(children[0], const_cast<long*>(&marker), &value, sizeof value) && value == 1,
                "guest failed to resume after timeout completion");
        for (int nr : {SYS_nanosleep, SYS_clock_nanosleep, SYS_pselect6,
                       SYS_epoll_wait, SYS_epoll_pwait, SYS_epoll_pwait2}) {
            auto timer = step(resumed, nr);
            require(timer.child[0].threads[0].pending_syscall &&
                    timer.child[0].threads[0].pending_syscall->phase == PendingPhase::Waiting &&
                    timer.child[0].threads[0].pending_syscall->deadline, "timer did not remain unfinished");
            if (nr == SYS_epoll_wait) {
                timer.recover_running_state();
                ptmc_state.running_state.child[0].threads[0].pending_syscall->phase = PendingPhase::Retry;
                auto retry = capture();
                reject_without_mutation(retry, "Retry epoll accepted timeout");
                timer.recover_running_state(); ptmc_state.cursor = 0;
                require(emu_write(pipefd[1], &payload, 1) == 1, "make competing readiness");
                auto data_ready = capture();
                require(io_retry_ready(data_ready.child[0], data_ready.child[0].si[0]), "pipe not readable");
                reject_without_mutation(data_ready, "Waiting epoll bypassed existing I/O readiness");
                // The live peer branch is ready, but restoring the original source must
                // restore the empty queue before deciding whether timeout is eligible.
            }
            auto done = timeout_and_replay(timer, 0);
            std::printf("PASS syscall %d timeout completion and replay\n", nr);
            resumed = step(done, SYS_getpid);
        }
        // State and edge predicates each receive the completion boundary. A
        // violating state retains its monitor event and refuses further actions.
        for (int fail : {1, 2}) {
            waiting.recover_running_state();
            require(guest_write_exact(children[0], &violate, &fail, sizeof fail), "seed property failure");
            ptmc_state.tracee_image_dirty[0] = true;
            auto failing_source = capture();
            auto failed = state_transition(failing_source, 0, 0, TransitionAction::Timeout);
            require(failed.property_violation && failed.new_state.property_failed &&
                    monitor(failed.new_state).event.choice == DETSIM_CHOICE_TIMEOUT,
                    "timeout completion did not publish terminal property and monitor");
            reject_without_mutation(sys_state(failed.new_state.ss_hash), "terminal timeout state accepted execution");
            std::printf("PASS timeout %s predicate publishes terminal state\n", fail == 1 ? "edge" : "state");
        }
        // One-byte caches intentionally bypass snapshots rather than evicting
        // resident entries. Drain asynchronous saves before proving cold reads.
        auto& store = StateStore::instance();
        if (argc == 3) store.wait_for_completion();
        const auto cold_before = store.get_stats();
        require(timeout_and_replay(waiting, -ETIMEDOUT).ss_hash == completed.ss_hash,
                "property failure contaminated healthy predecessor branch");
        if (argc == 3) {
            const auto cold_after = store.get_stats();
            require(cold_after.load_requests > cold_before.load_requests &&
                    cold_after.disk_reads > cold_before.disk_reads &&
                    cold_after.l1_hits == cold_before.l1_hits &&
                    cold_after.l2_hits == cold_before.l2_hits,
                    "tiny-cache bypass replay did not restore from cold disk");
            std::puts("PASS tiny-cache bypass replay reads persisted snapshots from cold disk");
        }
        cleanup();
        std::puts("PASS explicit timeout publication, rejection isolation and full persisted replay");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pending timeout: %s\n", error.what()); cleanup(); return 1;
    }
}
#endif
