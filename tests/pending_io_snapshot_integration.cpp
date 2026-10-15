#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "io.h"
#include "utils.h"
#include "pending_io.h"
#include "state/state_transition.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "engine/exec_engine.h"
#include "engine/thread_manager.h"
#include "engine/thread_pool.h"
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static int endpoint;
static char output[8] = "payload";
alignas(4096) static char input[4096];
static long observed = -77;
static int sender_exit_call = SYS_exit;
static sockaddr_in address;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static sys_state step(const sys_state& source, int node, int nr) {
    auto next = state_transition(source, node, 0);
    require(!next.property_violation && next.code != CKPT_STOP && next.code != CKPT_DISCARD,
            "transition rejected");
    require(next.new_state.child[node].si[0].nr == nr, "wrong syscall boundary");
    return std::move(next.new_state);
}
static sys_state capture() {
    syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
    for (int node = 0; node < NP; ++node)
        std::memcpy(info[node], ptmc_state.running_state.child[node].si, sizeof info[node]);
    return sys_state(info);
}
int main(int argc, char** argv) {
    pid_t children[NP]{};
    char directory[] = "/tmp/detsim-pending-io-snapshot-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    auto cleanup = [&] {
        for (pid_t child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, __WALL); }
        StateStore::instance().shutdown(); SysStateStore::instance().shutdown();
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
    };
    try {
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
                if (node == 0) {
                    observed = syscall(SYS_read, endpoint, input, 7);
                    syscall(SYS_getpid);
                    syscall(SYS_read, endpoint, input, 7);
                } else {
                    syscall(SYS_write, endpoint, output, 7);
                    syscall(SYS_getpid);
                    syscall(sender_exit_call, 0);
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
            ptmc_state.addrs[node] = node ? "192.168.0.2" : "192.168.0.1";
            adopt_clear_child_tid(node, children[node]);
        }
        ptmc_state.cursor = 0;
        auto& server = ptmc_state.sock_states[0];
        const int listener = server.do_socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(9010);
        inet_pton(AF_INET, "192.168.0.1", &addr.sin_addr);
        for (auto child : children)
            require(guest_write_exact(child, &address, &addr, sizeof addr), "write address");
        require(server.do_bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                server.do_listen(listener, 8) == 0, "listen");
        ptmc_state.cursor = 1;
        auto& client = ptmc_state.sock_states[1];
        const int client_fd = client.do_socket(AF_INET, SOCK_STREAM, 0);
        require(client.do_connect(client_fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "connect");
        ptmc_state.cursor = 0;
        const int server_fd = emu_accept4(listener, nullptr, nullptr, 0);
        require(server_fd >= 0 && guest_write_exact(children[0], &endpoint, &server_fd, sizeof server_fd) &&
                guest_write_exact(children[1], &endpoint, &client_fd, sizeof client_fd), "install endpoints");
        StateStore::Config config;
        if (argc == 2 && std::string(argv[1]) == "--tiny-cache")
            config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        auto_mode = 1;
        auto initial = capture(); ptmc_state.running_state = initial;
        thread::init_all(ptmc_state); exec::init_all();
        auto waiting = step(initial, 0, SYS_read);
        require(waiting.child[0].threads[0].pending_syscall &&
                waiting.child[0].threads[0].pending_syscall->phase == PendingPhase::Waiting &&
                static_cast<long>(waiting.child[0].si[0].rval) == -ENOSYS, "read must remain unfinished");
        auto ready = step(waiting, 1, SYS_write);
        require(ready.child[0].threads[0].pending_syscall->phase == PendingPhase::Retry &&
                ready.child[1].si[0].rval == 7, "sender did not enable read retry");
        auto completed = step(ready, 0, SYS_read);
        require(!completed.child[0].threads[0].pending_syscall && completed.child[0].si[0].rval == 7 &&
                completed.child[0].threads[0].regs.rip == ready.child[0].threads[0].regs.rip,
                "read completion ran user instructions");
        long value; char bytes[8]{};
        require(guest_read_exact(children[0], &observed, &value, sizeof value) && value == -77 &&
                guest_read_exact(children[0], input, bytes, 7) && std::memcmp(bytes, "payload", 7) == 0,
                "completion output or user execution barrier incorrect");
        for (int replay = 0; replay < 3; ++replay) {
            auto sent = step(sys_state(waiting.ss_hash), 1, SYS_write);
            require(sent.ss_hash == ready.ss_hash, "waiting restore changed send successor");
            auto received = step(sys_state(ready.ss_hash), 0, SYS_read);
            require(received.ss_hash == completed.ss_hash, "retry restore consumed payload differently");
            auto resumed = step(received, 0, SYS_getpid);
            require(guest_read_exact(children[0], &observed, &value, sizeof value) && value == 7,
                    "guest did not observe completed result");
            auto next_wait = step(resumed, 0, SYS_read);
            require(next_wait.child[0].threads[0].pending_syscall &&
                    next_wait.child[0].threads[0].pending_syscall->phase == PendingPhase::Waiting,
                    "read consumed the same payload twice");
        }
        // Last-thread exit and exit_group close the sender's endpoint. EOF is
        // a completion opportunity for the blocked peer, not a fake read return.
        auto drained = step(completed, 0, SYS_getpid);
        auto eof_waiting = step(drained, 0, SYS_read);
        auto sender_boundary = step(eof_waiting, 1, SYS_getpid);
        for (int exit_call : {SYS_exit, SYS_exit_group}) {
            sender_boundary.recover_running_state();
            require(guest_write_exact(children[1], &sender_exit_call, &exit_call, sizeof exit_call),
                    "select sender exit syscall");
            ptmc_state.tracee_image_dirty[1] = true;
            auto exit_source = capture();
            auto exited = step(exit_source, 1, exit_call);
            require(!exited.child[1].threads[0].alive &&
                    exited.child[0].threads[0].pending_syscall->phase == PendingPhase::Retry &&
                    static_cast<long>(exited.child[0].si[0].rval) == -ENOSYS,
                    "sender exit did not enable unfinished peer read");
            auto eof = step(exited, 0, SYS_read);
            require(!eof.child[0].threads[0].pending_syscall && eof.child[0].si[0].rval == 0 &&
                    eof.child[0].threads[0].regs.rip == exited.child[0].threads[0].regs.rip,
                    "EOF completion ran user instructions or returned nonzero");
            for (int replay = 0; replay < 3; ++replay) {
                auto replay_exit = step(sys_state(exit_source.ss_hash), 1, exit_call);
                require(replay_exit.ss_hash == exited.ss_hash,
                        "restored sender exit changed peer EOF state");
                require(step(sys_state(exited.ss_hash), 0, SYS_read).ss_hash == eof.ss_hash,
                        "restored peer EOF completion changed identity");
            }
        }
        sys_state(waiting.ss_hash).recover_running_state();
        require(tracee_do_syscall(children[0], SYS_mprotect, reinterpret_cast<uintptr_t>(input),
                sizeof input, PROT_NONE, 0, 0, 0) == 0, "protect pending output buffer");
        ptmc_state.tracee_image_dirty[0] = true;
        auto fault_waiting = capture();
        auto fault_ready = step(fault_waiting, 1, SYS_write);
        auto fault_done = step(fault_ready, 0, SYS_read);
        require(static_cast<long>(fault_done.child[0].si[0].rval) == -EFAULT &&
                !fault_done.child[0].threads[0].pending_syscall, "faulting completion did not return EFAULT");
        ptmc_state.cursor = 0;
        require(tracee_do_syscall(children[0], SYS_mprotect, reinterpret_cast<uintptr_t>(input),
                sizeof input, PROT_READ | PROT_WRITE, 0, 0, 0) == 0 &&
                emu_read(server_fd, input, 7) == 7, "faulting completion consumed queued payload");
        require(step(step(sys_state(waiting.ss_hash), 1, SYS_write), 0, SYS_read).ss_hash == completed.ss_hash,
                "original waiting branch changed after faulting completion");
        // A competing read may consume readiness before this actor is selected.
        // Exercise completion directly so the stale Retry itself is processed.
        sys_state(ready.ss_hash).recover_running_state(); ptmc_state.cursor = 0;
        require(emu_read(server_fd, input, 7) == 7, "competing read");
        syscall_info retried{};
        require(complete_pending_syscall(0, children[0], retried) && static_cast<long>(retried.rval) == -ENOSYS,
                "lost readiness returned a spurious result");
        ptmc_state.running_state.child[0].si[0] = retried;
        auto rearmed = capture();
        require(rearmed.child[0].threads[0].pending_syscall->phase == PendingPhase::Waiting,
                "lost readiness did not rearm");
        sys_state(rearmed.ss_hash).recover_running_state();
        require(ptmc_state.thread_blocked[0][0], "persisted rearmed read became runnable");
        cleanup();
        puts("PASS pending TCP Waiting/Retry/completion, whole-system persisted replay, process-exit EOF and competing read");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "pending IO snapshot: %s\n", error.what()); cleanup(); return 1;
    }
}
