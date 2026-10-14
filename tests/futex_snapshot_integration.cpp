#include "config.h"
#include "monitor.h"
#include "futexstate.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "state_store.h"
#include "proc_status.h"
#include "cereal/archives/binary.hpp"
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void wait_on(FutexState& state, pid_t tid, uint64_t address)
{
    uint64_t value = 7;
    require(state.handle_futex(tid, address, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 7, 0, 0, 0, &value) == 0,
            "enqueue waiter");
}
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--reload") {
        try {
            StateStore::instance().init();
            tracee_state disk(static_cast<hash_type>(std::stoull(argv[2])));
            require(disk.futex_state && disk.futex_state->get_wait_address(1) == 0x1000,
                    "fresh-process disk reload lost waiter");
            require(StateStore::instance().get_stats().disk_reads > 0, "reload did not read disk");
            StateStore::instance().shutdown();
            return 0;
        } catch (const std::exception& error) {
            fprintf(stderr, "FAIL disk reload: %s\n", error.what()); return 1;
        }
    }
    pid_t child = -1;
    char directory[] = "/tmp/detsim-futex-snapshot-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    try {
        auto& live = ptmc_state.running_state.child[0];
        live.futex_state.emplace();
        wait_on(*live.futex_state, 1, 0x1000);
        tracee_state captured(0, live.si);
        require(captured.futex_state && captured.futex_state->is_thread_waiting(1), "capture dropped live futex wait queues");
        tracee_state branch = captured;
        require(branch.metadata_equal(captured), "equal snapshot metadata differs");
        branch.futex_state->wake_thread(1);
        require(!branch.metadata_equal(captured), "metadata identity omitted futex queues");
        require(captured.futex_state->is_thread_waiting(1), "copied branch aliases saved futex queues");
        branch = captured;
        branch.futex_state->wake_thread(1);
        require(captured.futex_state->is_thread_waiting(1), "assigned branch aliases saved futex queues");
        std::stringstream archive;
        { cereal::BinaryOutputArchive out(archive); out(captured); }
        tracee_state decoded;
        { cereal::BinaryInputArchive in(archive); in(decoded); }
        require(decoded.futex_state && decoded.futex_state->get_wait_address(1) == 0x1000, "archive lost futex queue");
        tracee_state empty;
        std::stringstream empty_archive;
        { cereal::BinaryOutputArchive out(empty_archive); out(empty); }
        { cereal::BinaryInputArchive in(empty_archive); in(decoded); }
        require(!decoded.futex_state, "empty archive retained stale futex queue");
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        child = fork(); require(child >= 0, "fork");
        if (!child) {
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(2);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(3);
            asm volatile("int3");
            _exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "child stop");
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) == 0, "ptrace options");
        StateStore::instance().init();
        ptmc_state.pids[0] = child;
        ptmc_state.fd_managers[0] = std::make_shared<FdManager>();
        live.add_thread(child, 0, 0, 0, true);
        live.add_thread(child + 1, 0x1234, 0x5000, 1);
        live.remove_thread(2);
        tracee_state saved(0, live.si);
        const auto hash = saved.save(child);
        require(saved.threads.size() == 2 && saved.threads[1].tid == 2 && !saved.threads[1].physical_tid &&
                saved.thread_create_records.size() == 2, "capture renumbered or dropped exited virtual slots");
        tracee_state disk(hash);
        // Reject lifecycle restoration before any registers/memory are modified.
        tracee_state changed = disk;
        changed.threads[0].physical_tid = 0;
        user_regs_struct before{}, after{};
        require(ptrace(PTRACE_GETREGS, child, nullptr, &before) == 0, "read preflight registers");
        bool rejected = false;
        try { changed.recover_running_state(0, hash); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "changed thread set accepted");
        require(ptrace(PTRACE_GETREGS, child, nullptr, &after) == 0 && memcmp(&before, &after, sizeof(before)) == 0,
                "lifecycle rejection mutated tracee registers");
        require(disk.futex_state && disk.futex_state->get_wait_address(1) == 0x1000, "stored snapshot lost waiter");
        for (int replay = 0; replay < 3; ++replay) {
            live.futex_state->wake_thread(1);
            ptmc_state.thread_blocked[0][0] = false;
            ptmc_state.thread_blocked[0][17] = true;
            disk.recover_running_state(0, hash);
            require(live.futex_state && live.futex_state->is_thread_waiting(1), "restore lost queue after branch wake");
            require(ptmc_state.thread_exited[0][1], "restore lost exited virtual slot");
            require(ptmc_state.thread_blocked[0][0] && !ptmc_state.thread_blocked[0][17], "restore failed to reconstruct blocked flags");
            ThreadManager manager;
            manager.sync_from_tracee_state(disk);
            const auto* info = manager.get_thread_info(0);
            require(info && info->state == ThreadState::BLOCKED && info->blocked_on == 0x1000,
                    "scheduler sync made waiting thread runnable");
            require(saved.futex_state->is_thread_waiting(1) && disk.futex_state->is_thread_waiting(1), "restore aliases snapshots");
        }
        sys_state source;
        source.child[0] = disk;
        source.ts_hash[0] = hash;
        source.status[0] = dstatus_normal();
        ptmc_state.running_state.ts_hash[0] = hash;
        ptmc_state.status[0] = DIFEXITED;
        live.futex_state->wake_thread(1);
        ptmc_state.thread_blocked[0][0] = false;
        source.recover_running_state();
        require(DISALIVE(ptmc_state.status[0]), "same-hash restore retained virtual exit status");
        require(live.futex_state->is_thread_waiting(1) && ptmc_state.thread_blocked[0][0],
                "same-hash restore skipped runtime queues");
        StateStore::instance().wait_for_completion();
        StateStore::instance().shutdown();
        pid_t reload = fork(); require(reload >= 0, "reload fork");
        if (!reload) {
            const auto key = std::to_string(hash);
            execl("/proc/self/exe", "futex_snapshot_integration", "--reload", key.c_str(), nullptr);
            _exit(127);
        }
        require(waitpid(reload, &status, 0) == reload && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "persisted snapshot fresh-process round trip");
        kill(child, SIGKILL); waitpid(child, nullptr, __WALL); child = -1;
        StateStore::instance().shutdown();
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(directory);
        puts("PASS futex capture, branch isolation, archive, repeated ptrace restore and blocked reconstruction");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, __WALL); }
        StateStore::instance().shutdown();
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(directory);
        return 1;
    }
}
