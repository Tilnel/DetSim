#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include "sysstate_store.h"
#include "guest.h"
#include "proc_status.h"
#include "page_snapshot.h"
#include <cstdio>
#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/rseq.h>
#include <sys/syscall.h>

static long guest_value = 17;
static_assert(NP == 2, "capture oracle requires two processes");
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int main(int argc, char**)
{
    pid_t children[NP]{};
    char directory[] = "/tmp/detsim-capture-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    try {
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        // Fork before the stores start writer threads. Tiny caches force disk reloads.
        for (int i = 0; i < NP; ++i) {
            children[i] = fork(); require(children[i] >= 0, "fork");
            if (!children[i]) {
                // Fork inherits libc's rseq area; injected syscalls otherwise
                // let the kernel change its CPU fields between oracle captures.
                if (__rseq_size && syscall(SYS_rseq,
                    static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                    32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(5);
                if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(2);
                auto *code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
                code[0] = 0x0f; code[1] = 0x05;
                if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(3);
                asm volatile("int3");
                _exit(0);
            }
            int status;
            require(waitpid(children[i], &status, 0) == children[i] && WIFSTOPPED(status), "child stop");
            require(ptrace(PTRACE_SETOPTIONS, children[i], nullptr,
                           PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
            ptmc_state.pids[i] = children[i];
            ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
            ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
            ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
        }
        StateStore::Config config;
        if (argc > 1) config.hot_cache_size = config.warm_cache_size = 1;
        StateStore::instance().init(config);
        syscall_info info[NP][MAX_THREADS_PER_PROCESS]{};
        ptmc_state.cursor = -1;
        auto_mode = 1;
        // Use actual allocated descriptors: resource queues may never exist
        // outside the captured process descriptor namespace.
        ptmc_state.addrs[0] = "127.0.0.1"; ptmc_state.addrs[1] = "127.0.0.2";
        ptmc_state.cursor = 1;
        const int udp_fd = ptmc_state.sock_states[1].do_socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        const int listener_fd = ptmc_state.sock_states[1].do_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        require(udp_fd >= 0 && listener_fd >= 0, "allocate fixture sockets");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET; endpoint.sin_port = htons(23117);
        require(inet_pton(AF_INET, "127.0.0.2", &endpoint.sin_addr) == 1, "listener address");
        void* guest_address = reinterpret_cast<void*>(scratch_page);
        require(guest_write_exact(children[1], guest_address, &endpoint, sizeof(endpoint)), "write listener address");
        require(ptmc_state.sock_states[1].do_bind(listener_fd, static_cast<sockaddr*>(guest_address), sizeof(endpoint)) == 0 &&
                ptmc_state.sock_states[1].do_listen(listener_fd, 2) == 0, "create listener");
        int pipe_ends[2];
        require(ptmc_state.fs_states[1].do_pipe(pipe_ends, O_NONBLOCK | O_CLOEXEC) == 0, "create saved pipe");
        const int epfd = ptmc_state.sock_states[1].do_epoll_create1(EPOLL_CLOEXEC);
        epoll_event watch{}; watch.events = EPOLLIN; watch.data.u64 = 0xfedcba9876543210ULL;
        require(guest_write_exact(children[1], guest_address, &watch, sizeof(watch)), "write epoll fixture");
        require(ptmc_state.sock_states[1].do_epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_ends[0], static_cast<epoll_event*>(guest_address)) == 0,
                "watch saved pipe");
        const char pipe_payload[] = "pipe branch";
        require(guest_write_exact(children[1], guest_address, pipe_payload, sizeof(pipe_payload)), "write pipe fixture");
        require(ptmc_state.fs_states[1].do_pipe_write(pipe_ends[1], guest_address, sizeof(pipe_payload)) == sizeof(pipe_payload),
                "buffer saved pipe");
        ptmc_state.cursor = 0;
        const int client_fd = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        require(guest_write_exact(children[0], guest_address, &endpoint, sizeof(endpoint)), "write client address");
        require(ptmc_state.sock_states[0].do_connect(client_fd, static_cast<sockaddr*>(guest_address), sizeof(endpoint)) == 0,
                "create preaccept connection");
        const char tcp_payload[] = "pending branch";
        require(guest_write_exact(children[0], guest_address, tcp_payload, sizeof(tcp_payload)), "write TCP fixture");
        require(ptmc_state.sock_states[0].do_send(client_fd, guest_address, sizeof(tcp_payload), 0) == sizeof(tcp_payload),
                "buffer preaccept TCP");
        ptmc_state.cursor = -1;
        sys_state source(info);
        ptmc_state.running_state = source;
        ptmc_state.cursor = 0;
        ptmc_state.running_state.monitor_states["other-branch"] = {9};
        source.recover_running_state();
        require(ptmc_state.running_state.monitor_states == source.monitor_states &&
                ptmc_state.running_state.ss_hash == source.ss_hash,
                "restore did not select the source monitor/product identity");
        const auto before = StateStore::instance().get_stats();
        sys_state reused(info, &source);
        require(StateStore::instance().get_stats().save_calls == before.save_calls + NP,
                "capture skipped ordinary store validation");
        if (argc == 1) require(StateStore::instance().get_stats().save_reference_hits == before.save_reference_hits + 1,
                "unchanged resident inactive snapshot did not bypass hashing");
        require(reused.ss_hash == source.ss_hash, "unchanged capture identity drift");
        auto compare_full = [&](const sys_state& optimized) {
            const char* backend = getenv("DETSIM_SNAPSHOT_BACKEND");
            const std::string selected = backend ? backend : "full";
            setenv("DETSIM_SNAPSHOT_BACKEND", "full", 1);
            setenv("DETSIM_CAPTURE_MODE", "full", 1);
            sys_state full(info, &source);
            unsetenv("DETSIM_CAPTURE_MODE");
            if (backend) setenv("DETSIM_SNAPSHOT_BACKEND", selected.c_str(), 1);
            else unsetenv("DETSIM_SNAPSHOT_BACKEND");
            if (selected == "full")
                require(full.ss_hash == optimized.ss_hash, "full capture system identity differs");
            for (int i = 0; i < NP; ++i) {
                const auto expected = SnapshotView::open(full.ts_hash[i]);
                const auto actual = SnapshotView::open(optimized.ts_hash[i]);
                require(expected->maps_text() == actual->maps_text() &&
                    expected->metadata().size() == actual->metadata().size() &&
                    !memcmp(expected->metadata().data(), actual->metadata().data(), expected->metadata().size()) &&
                    !memcmp(&expected->main_regs(), &actual->main_regs(), sizeof(user_regs_struct)),
                    "decoded snapshot metadata differs");
                require(expected->regions().size() == actual->regions().size(), "region count differs");
                for (size_t region = 0; region < expected->regions().size(); ++region) {
                    const auto& e = expected->regions()[region];
                    const auto& a = actual->regions()[region];
                    require(e.start == a.start && e.end == a.end && e.prot == a.prot &&
                        !memcmp(e.flags, a.flags, sizeof(e.flags)), "decoded regions differ");
                    std::vector<uint8_t> ebytes(e.end-e.start), abytes(ebytes.size());
                    expected->read_into(e.start, ebytes.data(), ebytes.size());
                    actual->read_into(a.start, abytes.data(), abytes.size());
                    require(ebytes == abytes, "decoded snapshot memory differs");
                }
            }
        };
        compare_full(reused);
        // Cross-node modeled delivery must invalidate the idle node's hash.
        ptmc_state.time[1].tv_sec++;
        sys_state changed(info, &source);
        require(changed.ts_hash[1] != source.ts_hash[1], "inactive modeled update lost");
        compare_full(changed);
        ptmc_state.running_state = changed;
        source.recover_running_state();
        sys_state restored(info, &source);
        require(restored.ss_hash == source.ss_hash, "branch restore reused stale running-state hash");
        compare_full(restored);
        setenv("DETSIM_CAPTURE_MODE", "verify", 1);
        sys_state verified(info, &source);
        require(verified.ss_hash == source.ss_hash, "verification mode identity differs");
        unsetenv("DETSIM_CAPTURE_MODE");
        // A stopped private process can still be changed by tracer expressions
        // or another process: metadata equality alone is not a valid fast path.
        require(ptrace(PTRACE_POKEDATA, children[1], &guest_value, 99) == 0, "inactive memory edit");
        sys_state memory_changed(info, &source);
        require(memory_changed.ts_hash[1] != source.ts_hash[1], "inactive memory edit lost");
        compare_full(memory_changed);
        ptmc_state.running_state = memory_changed;
        source.recover_running_state();
        UdpDatagram datagram{};
        datagram.content = "peer delivery";
        ptmc_state.sock_states[1].deliver_udp_datagram(udp_fd, datagram);
        sys_state socket_changed(info, &source);
        require(socket_changed.ts_hash[1] != source.ts_hash[1], "inactive socket update lost");
        compare_full(socket_changed);
        ptmc_state.running_state = socket_changed;
        source.recover_running_state();
        // Restoring reconstitutes queued data, descriptor flags and opaque
        // watches, independently of later branch mutations and accepted sockets.
        ptmc_state.cursor = 1;
        require(ptmc_state.fd_managers[1]->get_descriptor_flags(pipe_ends[0]) == FD_CLOEXEC &&
                (ptmc_state.fd_managers[1]->get_status_flags(pipe_ends[0]) & O_NONBLOCK), "restored pipe flags");
        require(ptmc_state.sock_states[1].epoll_instances().at(epfd).watched_fds.at(pipe_ends[0]).data == watch.data.u64,
                "restored epoll opaque data");
        const int accepted = ptmc_state.sock_states[1].do_accept(listener_fd, nullptr, nullptr);
        require(accepted >= 0 && ptmc_state.sock_states[1].do_recv(accepted, guest_address, sizeof(tcp_payload), 0) == sizeof(tcp_payload),
                "restored preaccept TCP data missing");
        char received[sizeof(tcp_payload)]{};
        require(guest_read_exact(children[1], guest_address, received, sizeof(received)) &&
                !memcmp(received, tcp_payload, sizeof(received)), "restored TCP payload changed");
        require(ptmc_state.fs_states[1].do_pipe_read(pipe_ends[0], guest_address, sizeof(pipe_payload)) == sizeof(pipe_payload),
                "restored pipe data missing");
        require(ptmc_state.fs_states[1].do_pipe_close(pipe_ends[1]) == 0, "close branch pipe end");
        ptmc_state.cursor = 0;
        sys_state consumed(info, &source);
        require(consumed.ts_hash[1] != source.ts_hash[1], "I/O branch mutation absent from identity");
        ptmc_state.running_state = consumed;
        source.recover_running_state();
        sys_state replayed(info, &source);
        require(replayed.ss_hash == source.ss_hash, "I/O branch restore identity drift");
        compare_full(replayed);
        // The selected actor is always recaptured even when modeled metadata agrees.
        user_regs_struct regs;
        require(ptrace(PTRACE_GETREGS, children[0], nullptr, &regs) == 0, "read actor registers");
        regs.rax ^= 1;
        require(ptrace(PTRACE_SETREGS, children[0], nullptr, &regs) == 0, "change actor registers");
        sys_state active_changed(info, &source);
        require(active_changed.ts_hash[0] != source.ts_hash[0], "active actor incorrectly reused");
        compare_full(active_changed);
        // Generic captures have no comparison hint and use the ordinary store path.
        const auto before_generic = StateStore::instance().get_stats().save_calls;
        sys_state generic(info);
        require(StateStore::instance().get_stats().save_calls == before_generic + NP,
                "unanchored capture bypassed the ordinary store path");
        if (argc > 1) require(StateStore::instance().get_stats().disk_reads > 0, "oracle did not exercise cache eviction");
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        for (pid_t &child : children) { kill(child, SIGKILL); waitpid(child, nullptr, 0); child = 0; }
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
        puts("PASS capture: idle reuse, full byte oracle, branch restore, memory/socket updates, active capture, cache eviction");
        return 0;
    } catch (const std::exception& error) {
        for (pid_t child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory);
        return 1;
    }
}
