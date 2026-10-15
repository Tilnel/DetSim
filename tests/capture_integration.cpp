#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include "sysstate_store.h"
#include "guest.h"
#include "proc_status.h"
#include "page_snapshot.h"
#include "engine/exec_engine.h"
#include "state_transition.h"
#include <array>
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
        std::array<ProcessKernelState,NP> initial_process_states;
        for (int i=0;i<NP;++i) {
            ptmc_state.process_states[i].random_state^=uint64_t(i+123);
            ptmc_state.process_states[i].signal_actions[SIGUSR1]={uint64_t(SIG_IGN),0,0,0};
            initial_process_states[i]=ptmc_state.process_states[i];
        }
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
        for (int i=0;i<NP;++i)
            require(source.child[i].process_state==initial_process_states[i],"process model state absent from capture");
        ptmc_state.process_states[1].random_state^=0x1234;
        sys_state random_changed(info,&source);
        require(random_changed.ts_hash[1]!=source.ts_hash[1] && random_changed.ss_hash!=source.ss_hash,
                "RNG stream position absent from snapshot identity");
        require(source.child[1].process_state==initial_process_states[1],"RNG branch mutated source snapshot");
        compare_full(random_changed);
        ptmc_state.running_state=random_changed;
        source.recover_running_state();
        for (int i=0;i<NP;++i)
            require(ptmc_state.process_states[i]==initial_process_states[i],"restored RNG/signal state differs");
        ptmc_state.process_states[0].signal_actions[SIGUSR1].handler=0x12345678;
        sys_state action_changed(info,&source);
        require(action_changed.ts_hash[0]!=source.ts_hash[0] && action_changed.ss_hash!=source.ss_hash,
                "signal disposition absent from snapshot identity");
        require(source.child[0].process_state==initial_process_states[0],"signal branch mutated source snapshot");
        compare_full(action_changed);
        ptmc_state.running_state=action_changed;
        source.recover_running_state();
        for (int i=0;i<NP;++i)
            require(ptmc_state.process_states[i]==initial_process_states[i],"signal branch restore differs");
        sys_state process_replayed(info,&source);
        require(process_replayed.ss_hash==source.ss_hash,"RNG/signal branch restore identity drift");
        compare_full(process_replayed);
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
        // A virtual exit parks the OS task. Restoring its product-state image
        // must restore memory/resources even while that node cannot execute.
        source.recover_running_state();
        ExecutionEngine engine;
        syscall_info exit_info{};
        ptmc_state.cursor=1;
        require(engine.execute_syscall(children[1],exit_info)==2 && exit_info.nr==SYS_exit_group &&
                DISEXITED(ptmc_state.status[1]), "fixture must exit through the virtual lifecycle path");
        syscall_info dead_info[NP][MAX_THREADS_PER_PROCESS];
        memcpy(dead_info,info,sizeof(dead_info));dead_info[1][0]=exit_info;
        sys_state exited(dead_info,&source);
        ptmc_state.running_state=exited;
        source.recover_running_state();
        require(ptrace(PTRACE_POKEDATA,children[1],&guest_value,99)==0,"edit alternate live branch");
        ptmc_state.process_states[1].random_state^=0x5678;
        ptmc_state.fs_states[1].set_creation_mask(0077);
        ptmc_state.time[1].tv_sec+=7;
        sys_state alternate(info,&source);
        ptmc_state.running_state=alternate;
        // Decode persisted state too: this is not only an in-memory copy test.
        sys_state loaded_exit(exited.ss_hash);
        loaded_exit.recover_running_state();
        long exited_value=0;
        require(guest_read_exact(children[1],&guest_value,&exited_value,sizeof(exited_value)) &&
                exited_value==17,"exited node retained another branch's memory");
        require(ptmc_state.process_states[1]==exited.child[1].process_state &&
                ptmc_state.fs_states[1].creation_mask()==exited.child[1].fs_state.creation_mask() &&
                ptmc_state.time[1].tv_sec==exited.child[1].tv.tv_sec,
                "exited node retained another branch's modeled resources");
        ptmc_state.cursor=0;
        for (int actor : {-1,1,NP}) {
            bool rejected=false;
            try {(void)state_transition(loaded_exit,actor);}catch(const std::runtime_error&){rejected=true;}
            require(rejected && ptmc_state.cursor==0,"invalid or terminated actor reached transition execution");
        }
        sys_state recaptured_exit(dead_info,&loaded_exit);
        require(recaptured_exit.ss_hash==exited.ss_hash,"exited branch recapture identity drift");
        exec::init_all();
        const auto peer_step=state_transition(loaded_exit,0);
        require(peer_step.code==CKPT_EXIT && peer_step.new_state.ts_hash[1]==exited.ts_hash[1] &&
                peer_step.new_state.status[1]==exited.status[1],
                "stepping a live peer contaminated the parked terminal image");
        source.recover_running_state();
        sys_state resurrected(info,&source);
        require(resurrected.ss_hash==source.ss_hash && DISALIVE(ptmc_state.status[1]),
                "virtual exit predecessor cannot replay faithfully");
        // Fatal signal stops also park an image: exercise a real fault, not
        // merely a synthetic status flag, before switching branches again.
        const unsigned char fault_instruction[]={0x0f,0x0b}; // ud2 -> SIGILL
        require(guest_write_exact(children[1],reinterpret_cast<void*>(scratch_page+0x100),
                fault_instruction,sizeof(fault_instruction)),"write fault fixture");
        user_regs_struct fault_regs{};
        require(ptrace(PTRACE_GETREGS,children[1],nullptr,&fault_regs)==0,"read fault registers");
        fault_regs.rip=scratch_page+0x100;
        require(ptrace(PTRACE_SETREGS,children[1],nullptr,&fault_regs)==0,"arm native fault");
        ptmc_state.cursor=1;
        syscall_info fault_info{};
        require(engine.execute_syscall(children[1],fault_info)==3 && DISCRASHED(ptmc_state.status[1]),
                "fixture must crash at a real fatal signal stop");
        sys_state crashed(info,&source);
        ptmc_state.running_state=crashed;
        source.recover_running_state();
        require(ptrace(PTRACE_POKEDATA,children[1],&guest_value,123)==0,"edit alternate crash branch");
        ptmc_state.process_states[1].random_state^=0xabcd;
        sys_state crash_alternate(info,&source);
        ptmc_state.running_state=crash_alternate;
        sys_state loaded_crash(crashed.ss_hash);
        loaded_crash.recover_running_state();
        long crash_value=0;
        require(guest_read_exact(children[1],&guest_value,&crash_value,sizeof(crash_value)) &&
                crash_value==17 && ptmc_state.process_states[1]==crashed.child[1].process_state,
                "crashed node retained another branch's image");
        sys_state recaptured_crash(info,&loaded_crash);
        require(recaptured_crash.ss_hash==crashed.ss_hash,"crash branch recapture identity drift");
        ptmc_state.running_state=recaptured_crash;
        source.recover_running_state();
        sys_state crash_predecessor(info,&source);
        require(crash_predecessor.ss_hash==source.ss_hash,"crash predecessor restore identity drift");
        if (argc > 1) require(StateStore::instance().get_stats().disk_reads > 0, "oracle did not exercise cache eviction");
        // A later node's failure must be detected before any earlier image,
        // status, budget, or store record changes.
        require(ptrace(PTRACE_POKEDATA,children[0],&guest_value,456)==0,"edit preflight witness");
        ptmc_state.error_bound=3;
        sys_state before_death(info,&source);
        ptmc_state.running_state=before_death;
        kill(children[1],SIGKILL);waitpid(children[1],nullptr,0);children[1]=0;
        user_regs_struct before_failure{},after_failure{};
        require(ptrace(PTRACE_GETREGS,children[0],nullptr,&before_failure)==0,"preflight registers");
        bool vanished_rejected=false;
        try {source.recover_running_state();}catch(const std::runtime_error&){vanished_rejected=true;}
        long witness=0;
        require(vanished_rejected && ptmc_state.error_bound==3 &&
                ptmc_state.running_state.ss_hash==before_death.ss_hash &&
                guest_read_exact(children[0],&guest_value,&witness,sizeof(witness)) && witness==456 &&
                ptrace(PTRACE_GETREGS,children[0],nullptr,&after_failure)==0 &&
                !memcmp(&before_failure,&after_failure,sizeof(before_failure)),
                "failed all-node restore preflight mutated an earlier node");
        const auto save_calls=StateStore::instance().get_stats().save_calls;
        bool capture_rejected=false;
        try {sys_state refused(info,&source);}catch(const std::runtime_error&){capture_rejected=true;}
        require(capture_rejected && StateStore::instance().get_stats().save_calls==save_calls,
                "failed all-node capture preflight published an earlier node");
        // Clearing runtime provenance must not grant a disk-loaded snapshot
        // ownership of whatever physical process happens to occupy this slot.
        clear_snapshot_thread_bindings();
        bool foreign_rejected=false;
        try {(void)source.child[0].resolve_thread_bindings(0,source.ts_hash[0]);}
        catch(const std::runtime_error& error) {
            foreign_rejected=std::string(error.what()).find("no local thread binding provenance")!=std::string::npos;
        }
        require(foreign_rejected,"unregistered snapshot regained host thread ownership");
        StateStore::instance().shutdown();
        SysStateStore::instance().shutdown();
        for (pid_t &child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); child = 0; }
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
        puts("PASS capture: idle reuse, full byte oracle, branch restore, memory/socket updates, active capture, cache eviction");
        return 0;
    } catch (const std::exception& error) {
        for (pid_t child : children) if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory);
        return 1;
    }
}
