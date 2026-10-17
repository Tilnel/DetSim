#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include "guest.h"
#include "dispatcher.h"
#include "page_restore_stats.h"
#include "page_snapshot.h"
#include <fcntl.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/rseq.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
static uintptr_t guard_address = 0;
static int guard_read_failure = 0;
extern "C" ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset)
{
    if (guard_read_failure && uint64_t(offset) == guard_address) {
        if (guard_read_failure == 2) { errno = EIO; return -1; }
        return size ? ssize_t(size - 1) : 0;
    }
    return __real_pread(fd, buffer, size, offset);
}
// Link wrapping keeps fault injection out of production restore code.
extern "C" uint64_t __real__Z17tracee_do_syscalliimmmmmm(int, int, uint64_t, uint64_t,
                                                        uint64_t, uint64_t, uint64_t, uint64_t);
static unsigned guard_discards = 0;
static int discard_failure = 0;
extern "C" uint64_t __wrap__Z17tracee_do_syscalliimmmmmm(int pid, int nr, uint64_t address,
        uint64_t size, uint64_t advice, uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (nr == SYS_madvise && address == guard_address && advice == MADV_DONTNEED) {
        ++guard_discards;
        if (discard_failure == 3) throw std::runtime_error("injected unexpected discard stop failure");
        if (discard_failure == 2) {
            // A negative syscall result must repair even a partially discarded range.
            auto result = __real__Z17tracee_do_syscalliimmmmmm(pid, nr, address, 4096, advice, a4, a5, a6);
            if (result) throw std::runtime_error("partial discard injection failed");
        }
        if (discard_failure) return uint64_t(-EINVAL);
    }
    return __real__Z17tracee_do_syscalliimmmmmm(pid, nr, address, size, advice, a4, a5, a6);
}
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
int main(int argc, char** argv)
{
    pid_t child = -1;
    char directory[] = "/tmp/detsim-mapping-XXXXXX";
    auto cwd = std::filesystem::current_path();
    try {
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        StateStore::Config store_config;
        if (argc == 2 && !strcmp(argv[1], "--tiny-cache"))
            store_config.hot_cache_size = store_config.warm_cache_size = 1;
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
            // This fixture owns raw memory snapshots, not the process rseq model.
            if (__rseq_size && syscall(SYS_rseq,
                    static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
                    32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(5);
            if (mmap(reinterpret_cast<void*>(scratch_page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(2);
            auto* code = reinterpret_cast<unsigned char*>(scratch_page + 0xff0);
            code[0] = 0x0f; code[1] = 0x05;
            if (mprotect(page, 8192, PROT_READ)) _exit(3);
            if (syscall(SYS_set_robust_list, nullptr, 24)) _exit(91);
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(4);
            asm volatile("int3");
            _exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "child stop");
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD) == 0, "ptrace options");
        StateStore::instance().init(store_config);
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
        // Physical THP eligibility is a hint, excluded from semantic identity.
        // A subrange can split the host VMA without changing modeled mappings.
        ptmc_state.cursor = 0;
        SyscallDispatcher dispatcher;
        const auto provenance = ptmc_state.fs_states[0].virtual_file_mappings;
        auto nohugepage = [&] {
            syscall_info advice{};
            advice.nr = SYS_madvise;
            advice.args[0] = reinterpret_cast<uintptr_t>(page) + 4096;
            advice.args[1] = 1;
            advice.args[2] = MADV_NOHUGEPAGE;
            require(dispatcher.on_enter(child, SYS_madvise, advice), "admit anonymous nohugepage hint");
            require(tracee_do_syscall(child, SYS_madvise, advice.args[0], advice.args[1],
                                     advice.args[2], 0, 0, 0) == 0, "execute anonymous nohugepage hint");
            require(protection(child, reinterpret_cast<uintptr_t>(page)) == PROT_READ &&
                    protection(child, reinterpret_cast<uintptr_t>(page) + 4096) == PROT_READ,
                    "nohugepage changed protections");
            require(ptmc_state.fs_states[0].virtual_file_mappings == provenance &&
                    saved.fs_state.virtual_file_mappings == provenance,
                    "nohugepage changed semantic provenance");
        };
        nohugepage();
        require(saved.save_full_state_to_state_store(child) == hash,
                "nohugepage hint changed saved semantic identity");
        mutate();
        const auto hinted_successor = saved.save_full_state_to_state_store(child);
        require(hinted_successor != hash, "nohugepage branch mutation missing from identity");
        restore(); nohugepage(); mutate();
        require(saved.save_full_state_to_state_store(child) == hinted_successor,
                "nohugepage branch replay changed successor identity");
        restore();
        require(tracee_do_syscall(child, SYS_munmap, reinterpret_cast<uintptr_t>(page), 8192,
                                 0, 0, 0, 0) == 0, "remove hinted mapping");
        require(tracee_do_syscall(child, SYS_mmap, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1, 0) == reinterpret_cast<uintptr_t>(page), "recreate unhinted mapping");
        restore(); mutate();
        require(saved.save_full_state_to_state_store(child) == hinted_successor,
                "nohugepage recreation replay changed successor identity");
        restore();
        puts("PASS nohugepage: subrange identity, mutation replay, permissions and recreated mapping");
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
        require(tracee_do_syscall(child,SYS_madvise,reinterpret_cast<uintptr_t>(page),8192,
                                 MADV_DONTNEED,0,0,0)==0,"discard anonymous readonly pages");
        unsigned char discarded=99;
        require(guest_read_exact(child,page,&discarded,1) && discarded==0 &&
                protection(child,reinterpret_cast<uintptr_t>(page))==PROT_READ,
                "discard did not zero bytes while retaining permission");
        restore();
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
        guard_address = reinterpret_cast<uintptr_t>(page);
        for (int failure : {1, 2}) {
            guard_read_failure = failure;
            const auto saves = StateStore::instance().get_stats().save_calls;
            bool failed_read = false;
            try { saved.save_full_state_to_state_store(child); }
            catch (const std::runtime_error&) { failed_read = true; }
            guard_read_failure = 0;
            require(failed_read, "guard short/failed read accepted");
            require(StateStore::instance().get_stats().save_calls == saves,
                    "guard read failure published snapshot");
            require(protection(child, guard_address) == PROT_NONE,
                    "failed guard capture changed protection");
        }
        const auto guard_hash = saved.save_full_state_to_state_store(child);
        require(protection(child, reinterpret_cast<uintptr_t>(page)) == PROT_NONE,
                "capture changed guard protection");
        unsigned char hidden = 0;
        require(!guest_read_exact(child, page, &hidden, 1), "guest read bypassed guard protection");
        auto* guard_bytes = static_cast<unsigned char*>(read_mem(child, guard_hash,
                reinterpret_cast<uintptr_t>(page), 8192));
        require(guard_bytes[0] == 11 && guard_bytes[4096] == 12,
                "guard snapshot omitted hidden bytes");
        free(guard_bytes);
        mutate();
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_NONE, 0, 0, 0) == 0, "hide mutated guard bytes");
        const auto changed_guard_hash = saved.save_full_state_to_state_store(child);
        require(changed_guard_hash != guard_hash, "hidden guard contents missing from identity");
        auto restore_guard = [&] {
            std::vector<maps_item> guard_maps;
            saved.restore_memory_mappings(guard_hash, child, guard_maps);
            saved.recover_mem_reg_snapshot(guard_maps, guard_hash, child);
            require(protection(child, reinterpret_cast<uintptr_t>(page)) == PROT_NONE &&
                    protection(child, reinterpret_cast<uintptr_t>(page) + 4096) == PROT_NONE,
                    "guard restore lost permissions");
            require(!guest_read_exact(child, page, &hidden, 1), "restored guard is guest-readable");
            require(saved.save_full_state_to_state_store(child) == guard_hash,
                    "guard restore changed snapshot identity");
            require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                     PROT_READ, 0, 0, 0) == 0, "unhide restored guard");
            require(guest_read_exact(child, page, &hidden, 1) && hidden == 11,
                    "guard restore lost hidden data");
        };
        StateStore::instance().shutdown();
        StateStore::instance().init(store_config);
        restore_guard();
        // No content mutation: this must repair permissions even without writes.
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(page), 8192,
                                 PROT_READ | PROT_WRITE, 0, 0, 0) == 0, "unprotect identical guard");
        restore_guard();
        mutate(); restore_guard();
        require(tracee_do_syscall(child, SYS_munmap, reinterpret_cast<uintptr_t>(page), 8192,
                                 0, 0, 0, 0) == 0, "remove guard backing");
        restore_guard();
        // The saved nonzero guard must never use discard, including after reopen.
        guard_discards = 0;
        restore_guard();
        require(guard_discards == 0, "nonzero saved guard was discarded");

        // Native oracle: Linux discards a protected anonymous range without opening it.
        require(tracee_do_syscall(child, SYS_mprotect, guard_address, 8192, PROT_NONE, 0, 0, 0) == 0,
                "native oracle guard protection");
        require(__real__Z17tracee_do_syscalliimmmmmm(child, SYS_madvise, guard_address, 8192,
                MADV_DONTNEED, 0, 0, 0) == 0, "native protected anonymous discard");
        auto protected_bytes = [&] {
            std::vector<unsigned char> bytes(8192);
            const auto path = "/proc/" + std::to_string(child) + "/mem";
            int fd = open(path.c_str(), O_RDONLY);
            require(fd >= 0, "open owned child protected memory");
            const auto count = __real_pread(fd, bytes.data(), bytes.size(), guard_address);
            close(fd);
            require(count == ssize_t(bytes.size()), "read complete protected memory");
            return bytes;
        };
        const std::vector<unsigned char> zeros(8192, 0), poison(8192, 0xa5);
        require(protected_bytes() == zeros && protection(child, guard_address) == PROT_NONE,
                "native protected discard did not retain zero bytes and protection");
        const auto zero_guard_hash = saved.save_full_state_to_state_store(child);
        auto poison_guard = [&](bool readable = false) {
            require(tracee_do_syscall(child, SYS_mprotect, guard_address, 8192,
                    PROT_READ | PROT_WRITE, 0, 0, 0) == 0, "open guard for poison");
            iovec local{const_cast<unsigned char*>(poison.data()), poison.size()}, remote{page, poison.size()};
            require(process_vm_writev(child, &local, 1, &remote, 1, 0) == ssize_t(poison.size()),
                    "poison entire guard including last page");
            if (!readable)
                require(tracee_do_syscall(child, SYS_mprotect, guard_address, 8192,
                        PROT_NONE, 0, 0, 0) == 0, "hide poisoned guard");
        };
        auto recover_zero = [&](const tracee_state& target, hash_type key) {
            std::vector<maps_item> maps;
            target.restore_memory_mappings(key, child, maps);
            guard_discards = 0;
            detsim::page_restore::reset();
            target.recover_mem_reg_snapshot(maps, key, child);
            return detsim::page_restore::stats();
        };
        auto check_zero = [&] {
            require(protected_bytes() == zeros, "zero guard restore omitted bytes");
            require(protection(child, guard_address) == PROT_NONE &&
                    protection(child, guard_address + 4096) == PROT_NONE,
                    "zero guard restore lost final protections");
            require(saved.save_full_state_to_state_store(child) == zero_guard_hash,
                    "zero guard restore differs from saved semantic identity");
        };
        uint64_t baseline_write = 0;
        for (bool baseline : {true, false}) {
            // Both implementations must successfully reload the saved bytes cold.
            StateStore::instance().shutdown();
            StateStore::instance().init(store_config);
            if (baseline) setenv("DETSIM_ZERO_GUARD_RESTORE", "write", 1);
            else unsetenv("DETSIM_ZERO_GUARD_RESTORE");
            for (int replay = 0; replay < 3; ++replay) {
                poison_guard();
                const auto stats = recover_zero(saved, zero_guard_hash);
                check_zero();
                if (baseline) {
                    require(guard_discards == 0, "forced-write guard used discard");
                    require(stats.zero_guard_bytes == 0 && stats.write_bytes >= 8192,
                            "forced-write guard did not use baseline writes");
                    baseline_write = stats.write_bytes;
                } else {
                    require(guard_discards == 1, "zero guard restore did not discard once");
                    require(stats.zero_guard_bytes == 8192 && stats.zero_guard_regions == 1 &&
                            stats.zero_guard_fallbacks == 0 && stats.write_bytes + 8192 == baseline_write,
                            "zero guard discard counters or baseline write savings differ");
                }
            }
        }
        poison_guard(true);
        recover_zero(saved, zero_guard_hash);
        require(guard_discards == 1, "readable-live zero guard did not discard");
        check_zero();

        // Nonzero data only in the last saved page must invalidate the entire candidate.
        poison_guard(true);
        std::vector<unsigned char> tail_nonzero(8192, 0);
        tail_nonzero.back() = 73;
        iovec tail_local{tail_nonzero.data(), tail_nonzero.size()}, tail_remote{page, tail_nonzero.size()};
        require(process_vm_writev(child, &tail_local, 1, &tail_remote, 1, 0) == 8192,
                "write nonzero saved tail");
        require(tracee_do_syscall(child, SYS_mprotect, guard_address, 8192, PROT_NONE, 0, 0, 0) == 0,
                "hide nonzero saved tail");
        const auto tail_hash = saved.save_full_state_to_state_store(child);
        poison_guard();
        const auto tail_stats = recover_zero(saved, tail_hash);
        require(guard_discards == 0 && tail_stats.zero_guard_bytes == 0 && protected_bytes() == tail_nonzero,
                "late nonzero saved page was discarded or restored incorrectly");
        require(protection(child, guard_address) == PROT_NONE &&
                saved.save_full_state_to_state_store(child) == tail_hash,
                "nonzero tail baseline changed protection or identity");

        // Native anonymous VMAs can carry virtual file mappings: use target provenance.
        auto file_origin = saved;
        file_origin.fs_state.set_fd_manager(std::make_shared<FdManager>());
        const int origin_fd = file_origin.fs_state.do_open("/zero-guard-origin", O_CREAT | O_RDWR, 0600);
        require(origin_fd >= 0, "create verified virtual file origin");
        file_origin.fs_state.add_virtual_file_mapping(guard_address + 4096, guard_address + 8192,
                                                     "/zero-guard-origin");
        for (int provenance : {0, 1, 2}) {
            auto target_origin = file_origin;
            // Each table independently excludes discard even if its peer is unavailable.
            if (provenance == 1) target_origin.fs_state.virtual_file_mapping_sources.clear();
            if (provenance == 2) target_origin.fs_state.virtual_file_mappings.clear();
            poison_guard();
            const auto origin_stats = recover_zero(target_origin, zero_guard_hash);
            require(guard_discards == 0 && origin_stats.zero_guard_bytes == 0,
                    "target virtual file origin allowed anonymous carrier discard");
            check_zero();
        }

        // Saved mapping metadata alone can remove proof of anonymity or alignment.
        for (int exclusion : {0, 1, 2, 3, 4}) {
            poison_guard();
            std::vector<maps_item> maps;
            saved.restore_memory_mappings(zero_guard_hash, child, maps);
            bool changed_map = false;
            for (auto& map : maps) if (map.start <= guard_address && guard_address + 8192 <= map.end) {
                if (exclusion == 0) map.inode = 1;
                if (exclusion == 1) strcpy(map.name, "[unproven-anonymous]");
                if (exclusion == 2) ++map.start;
                if (exclusion == 3) map.a = 1;
                if (exclusion == 4) map.flags[3] = 's';
                changed_map = true;
            }
            require(changed_map, "saved guard mapping exclusion fixture missing");
            guard_discards = 0;
            bool rejected = false;
            try { saved.recover_mem_reg_snapshot(maps, zero_guard_hash, child); }
            catch (const std::runtime_error&) { rejected = true; }
            require(guard_discards == 0, "unproven saved mapping used discard");
            if (exclusion == 2 || exclusion == 4)
                require(rejected, "incomplete/shared saved mapping coverage accepted");
            else check_zero();
        }
        // Fresh live proof must reject file backing despite anonymous saved metadata.
        std::vector<maps_item> file_live_maps;
        saved.restore_memory_mappings(zero_guard_hash, child, file_live_maps);
        require(ftruncate(fileno(file), 8192) == 0, "extend inherited live carrier backing");
        require(tracee_do_syscall(child, SYS_mmap, guard_address, 8192, PROT_NONE,
                MAP_FIXED | MAP_PRIVATE, fileno(file), 0) == guard_address,
                "replace live anonymous carrier with file backing");
        poison_guard();
        guard_discards = 0;
        detsim::page_restore::reset();
        saved.recover_mem_reg_snapshot(file_live_maps, zero_guard_hash, child);
        require(guard_discards == 0 && detsim::page_restore::stats().zero_guard_bytes == 0 &&
                protected_bytes() == zeros && protection(child, guard_address) == PROT_NONE,
                "file-backed live carrier allowed discard or changed restored bytes");
        // Ordinary mapping restoration must reconstruct the saved anonymous carrier.
        recover_zero(saved, zero_guard_hash);
        check_zero();

        // Invalid mode is a preflight error at both low-level public entry points.
        for (bool mapping_entry : {true, false}) {
            poison_guard();
            std::vector<maps_item> maps;
            saved.restore_memory_mappings(zero_guard_hash, child, maps);
            guard_discards = 0;
            setenv("DETSIM_ZERO_GUARD_RESTORE", "unknown-test-mode", 1);
            bool mode_failed = false;
            try {
                if (mapping_entry) saved.restore_memory_mappings(zero_guard_hash, child, maps);
                else saved.recover_mem_reg_snapshot(maps, zero_guard_hash, child);
            } catch (const std::runtime_error& error) {
                mode_failed = std::string(error.what()).find("DETSIM_ZERO_GUARD_RESTORE") != std::string::npos;
            }
            unsetenv("DETSIM_ZERO_GUARD_RESTORE");
            require(mode_failed && guard_discards == 0 && protected_bytes() == poison &&
                    protection(child, guard_address) == PROT_NONE,
                    "unknown restore mode did not reject before guard mutation");
        }
        for (int fault : {1, 2}) {
            poison_guard();
            discard_failure = fault;
            const auto stats = recover_zero(saved, zero_guard_hash);
            discard_failure = 0;
            require(guard_discards == 1 && stats.zero_guard_fallbacks == 1 &&
                    stats.zero_guard_bytes == 0 && stats.write_bytes >= 8192,
                    "negative/partial discard did not fall back to full saved writes");
            check_zero();
        }
        poison_guard();
        discard_failure = 3;
        bool unexpected_failed = false;
        try { recover_zero(saved, zero_guard_hash); }
        catch (const std::runtime_error& error) {
            unexpected_failed = std::string(error.what()).find("unexpected discard stop") != std::string::npos;
        }
        discard_failure = 0;
        require(unexpected_failed && guard_discards == 1,
                "unexpected discard exception manufactured restore success");
        require(protected_bytes() == poison && protection(child, guard_address) == PROT_NONE,
                "unexpected discard failure changed protected guard");

        // Clone a real manifest with only the last guard PageRef pointing to a missing key.
        // No store deletion or warm-reader shortcut: reopen before the failed page read.
        std::vector<uint8_t> manifest;
        require(StateStore::instance().load(zero_guard_hash, manifest) > 0, "load zero guard manifest");
        StateDataHeader header{};
        memcpy(&header, manifest.data(), sizeof(header));
        if (header.version == PAGE_SNAPSHOT_VERSION) {
            PageManifestHeader page_header{};
            memcpy(&page_header, manifest.data(), sizeof(page_header));
            bool replaced = false;
            for (uint32_t n = 0; n < page_header.page_count; ++n) {
                const auto offset = page_header.pages_offset + n * sizeof(PageRef);
                PageRef ref{};
                memcpy(&ref, manifest.data() + offset, sizeof(ref));
                if (ref.address != guard_address + 4096) continue;
                ref.key = 0x8badf00d12345678ULL;
                std::vector<uint8_t> missing;
                require(StateStore::instance().load(ref.key, missing) < 0, "missing page key unexpectedly exists");
                memcpy(manifest.data() + offset, &ref, sizeof(ref));
                replaced = true;
            }
            require(replaced, "late guard PageRef fixture missing");
            const auto bad_hash = StateStore::instance().save(manifest.data(), manifest.size());
            StateStore::instance().shutdown();
            StateStore::instance().init(store_config);
            poison_guard();
            bool missing_failed = false;
            try { recover_zero(saved, bad_hash); }
            catch (const std::runtime_error&) { missing_failed = true; }
            require(missing_failed && guard_discards == 0, "discard preceded late saved-page validation");
            require(protected_bytes() == poison && protection(child, guard_address) == PROT_NONE,
                    "late missing saved page changed guard before rejection");
        }
        recover_zero(saved, zero_guard_hash);
        check_zero();
        puts("PASS zero guards: native oracle, 3x baseline equivalence, cold replay, saved-tail proof, provenance, faults and saved-page integrity");
        restore();
        require(tracee_do_syscall(child, SYS_mprotect, reinterpret_cast<uintptr_t>(backed), 4096,
                                 PROT_NONE, 0, 0, 0) == 0, "file guard mutation");
        bool file_guard_failed = false;
        try { saved.save_full_state_to_state_store(child); }
        catch (const std::runtime_error&) { file_guard_failed = true; }
        require(file_guard_failed, "unreadable file mapping admitted by capture");
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
        puts("PASS mappings: RO/guard bytes, identity, persisted replay, read faults, permissions, anonymous recreation, unsupported backing");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory); return 1;
    }
}
