#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include "guest.h"
#include "page_restore_stats.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static int protection(pid_t pid, uintptr_t address)
{
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    FILE* fp = fopen(path.c_str(), "r");
    require(fp, "open maps");
    std::vector<maps_item> maps;
    get_maps_item(maps, fp); fclose(fp);
    for (const auto& map : maps)
        if (map.start <= address && address < map.end)
            return (map.flags[0] == 'r' ? PROT_READ : 0) | (map.flags[1] == 'w' ? PROT_WRITE : 0) |
                   (map.flags[2] == 'x' ? PROT_EXEC : 0);
    return -1;
}
int main()
{
    pid_t child = -1;
    char directory[] = "/tmp/detsim-mapping-XXXXXX";
    auto cwd = std::filesystem::current_path();
    try {
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        FILE* maps_fixture = tmpfile();
        require(maps_fixture, "maps parser fixture");
        fputs("100000-102000 r--p 100000000 ab:cd 5000000000 /tmp/backing\n"
              "200000-201000 rw-p 00000000 00:00 0\n", maps_fixture);
        rewind(maps_fixture);
        std::vector<maps_item> parsed;
        get_maps_item(parsed, maps_fixture); fclose(maps_fixture);
        require(parsed.size() == 2 && parsed[0].offset == 0x100000000ULL &&
                uint64_t(parsed[0].inode) == 5000000000ULL && parsed[0].a == 0xab &&
                parsed[0].b == 0xcd && parsed[1].name[0] == 0,
                "maps parser truncates offsets/inodes, hex devices or stale names");
        auto* page = static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        require(page != MAP_FAILED, "mmap");
        page[0] = 11; page[4096] = 12;
        constexpr size_t sparse_size = 1024 * 1024;
        auto* sparse = static_cast<unsigned char*>(mmap(nullptr, sparse_size, PROT_READ | PROT_WRITE,
                                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        require(sparse != MAP_FAILED, "sparse mapping");
        memset(sparse, 37, sparse_size);
        FILE* file = fopen("backing", "w+");
        require(file && ftruncate(fileno(file), 4096) == 0, "backing file");
        void* backed = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fileno(file), 0);
        require(backed != MAP_FAILED, "file mapping");
        child = fork(); require(child >= 0, "fork");
        if (!child) {
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(2);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (mprotect(page, 8192, PROT_READ)) _exit(3);
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
            asm volatile("int3");
            _exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "child stop");
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD) == 0, "ptrace options");
        tracee_state saved;
        auto hash = saved.save_full_state_to_state_store(child);
        char* literal = static_cast<char*>(read_mem(child, hash, reinterpret_cast<uintptr_t>("read-only fixture"), 17));
        require(memcmp(literal, "read-only fixture", 17) == 0, "RO literal missing from snapshot"); free(literal);
        auto mutate = [&] {
            require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                     PROT_READ | PROT_WRITE, 0, 0, 0) == 0, "mutation mprotect");
            unsigned char value = 22; iovec local{&value, 1}, remote{page, 1};
            require(process_vm_writev(child, &local, 1, &remote, 1, 0) == 1, "mutation write");
            require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page) + 4096, 4096,
                                     PROT_READ, 0, 0, 0) == 0, "split mprotect");
        };
        auto restore = [&] {
            std::vector<maps_item> maps;
            saved.restore_memory_mappings(hash, child, maps);
            saved.recover_mem_reg_snapshot(maps, hash, child);
            unsigned char value = 0; iovec local{&value, 1}, remote{page, 1};
            require(process_vm_readv(child, &local, 1, &remote, 1, 0) == 1 && value == 11,
                    "read-only page value was not restored");
            require(protection(child, reinterpret_cast<uintptr_t>(page)) == PROT_READ &&
                    protection(child, reinterpret_cast<uintptr_t>(page) + 4096) == PROT_READ,
                    "saved read-only permissions were not restored");
        };
        // Two remote changes in a large region must restore only changed pages,
        // including the final byte; unchanged pages retain the exact baseline.
        unsigned char changed = 91;
        iovec sparse_local[2]{{&changed, 1}, {&changed, 1}};
        iovec sparse_remote[2]{{sparse, 1}, {sparse + sparse_size - 1, 1}};
        require(process_vm_writev(child, sparse_local, 2, sparse_remote, 2, 0) == 2,
                "sparse mutations");
        std::vector<maps_item> sparse_maps;
        saved.restore_memory_mappings(hash, child, sparse_maps);
        detsim::page_restore::reset();
        saved.recover_mem_reg_snapshot(sparse_maps, hash, child);
        auto sparse_stats = detsim::page_restore::stats();
        require(sparse_stats.pages_written >= 2 && sparse_stats.write_bytes < sparse_size &&
                sparse_stats.read_bytes >= sparse_size, "restore did not restrict writes to changed pages");
        std::vector<unsigned char> sparse_copy(sparse_size);
        iovec sparse_read_local{sparse_copy.data(), sparse_size}, sparse_read_remote{sparse, sparse_size};
        require(process_vm_readv(child, &sparse_read_local, 1, &sparse_read_remote, 1, 0) == ssize_t(sparse_size),
                "sparse restored read");
        for (auto byte : sparse_copy) require(byte == 37, "sparse region restore omitted bytes");
        printf("Sparse restore: read=%lu write=%lu compared=%lu written=%lu\n",
               sparse_stats.read_bytes, sparse_stats.write_bytes, sparse_stats.pages_compared, sparse_stats.pages_written);
        mutate(); restore(); mutate(); restore();
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_READ | PROT_WRITE, 0, 0, 0) == 0, "writable saved permission");
        const auto writable_hash = saved.save_full_state_to_state_store(child);
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_READ, 0, 0, 0) == 0, "read-only branch permission");
        std::vector<maps_item> writable_maps;
        saved.restore_memory_mappings(writable_hash, child, writable_maps);
        saved.recover_mem_reg_snapshot(writable_maps, writable_hash, child);
        require(protection(child, reinterpret_cast<uintptr_t>(page)) == (PROT_READ | PROT_WRITE),
                "saved writable permission was not restored");
        restore();
        require(tracee_do_syscall(child, SYS_munmap, reinterpret_cast<uintptr_t>(page), 8192, 0, 0, 0, 0) == 0,
                "remove anonymous map");
        restore();
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_NONE, 0, 0, 0) == 0, "PROT_NONE mutation");
        bool no_read_failed = false;
        try { saved.save_full_state_to_state_store(child); }
        catch (const std::runtime_error&) { no_read_failed = true; }
        require(no_read_failed, "unreadable mapping silently omitted from snapshot");
        restore();
        // Extending the reserved injection VMA must not hide the extra guest page.
        require(tracee_do_syscall(child, SYS_mremap, scratch_page, 4096, 8192, 0, 0, 0) == scratch_page,
                "extend scratch VMA");
        const auto saves_before_overlap = StateStore::instance().get_stats().save_calls;
        bool overlap_failed = false;
        try { saved.save_full_state_to_state_store(child); }
        catch (const std::runtime_error& error) {
            overlap_failed = std::string(error.what()).find("scratch") != std::string::npos;
        }
        require(overlap_failed, "expanded scratch VMA silently omitted guest memory");
        require(StateStore::instance().get_stats().save_calls == saves_before_overlap,
                "expanded scratch VMA published incomplete snapshot");
        bool overlap_restore_failed = false;
        try { std::vector<maps_item> maps; saved.restore_memory_mappings(hash, child, maps); }
        catch (const std::runtime_error& error) {
            overlap_restore_failed = std::string(error.what()).find("scratch") != std::string::npos;
        }
        require(overlap_restore_failed, "expanded scratch VMA silently accepted during restore");
        require(tracee_do_syscall(child, SYS_mremap, scratch_page, 8192, 4096, 0, 0, 0) == scratch_page,
                "shrink scratch VMA");
        restore();
        require(tracee_do_syscall(child, SYS_munmap, reinterpret_cast<uintptr_t>(backed), 4096, 0, 0, 0, 0) == 0,
                "remove file map");
        bool failed = false;
        try { std::vector<maps_item> maps; saved.restore_memory_mappings(hash, child, maps); }
        catch (const std::runtime_error&) { failed = true; }
        require(failed, "missing file-backed map silently replaced anonymously");
        require(tracee_do_syscall(child, SYS_mmap, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_SHARED,
                                 -1, 0) == reinterpret_cast<uintptr_t>(page), "shared mapping mutation");
        bool shared_failed = false;
        try { saved.save_full_state_to_state_store(child); }
        catch (const std::runtime_error&) { shared_failed = true; }
        require(shared_failed, "shared mapping accepted as isolated snapshot memory");
        kill(child, SIGKILL); waitpid(child, nullptr, 0); child = -1;
        StateStore::instance().shutdown();
        munmap(page, 8192); munmap(sparse, sparse_size); munmap(backed, 4096); fclose(file);
        std::filesystem::current_path(cwd); std::filesystem::remove_all(directory);
        puts("PASS mappings: RO data/literal capture, repeated restore, split/merge permissions, anonymous recreation, file-backed rejection");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory); return 1;
    }
}
