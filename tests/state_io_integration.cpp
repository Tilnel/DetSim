#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "state_store.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
extern "C" ssize_t __real_process_vm_readv(pid_t, const iovec*, unsigned long, const iovec*, unsigned long, unsigned long);
extern "C" ssize_t __real_process_vm_writev(pid_t, const iovec*, unsigned long, const iovec*, unsigned long, unsigned long);
static int read_failure = 0, write_failure = 0;
static ssize_t injected_result(const iovec* io, unsigned long count, int mode)
{
    if (mode == 2) { errno = EFAULT; return -1; }
    size_t size = 0;
    for (unsigned long i = 0; i < count; ++i) size += io[i].iov_len;
    return size ? size - 1 : 0;
}
extern "C" ssize_t __wrap_process_vm_readv(pid_t pid, const iovec* local, unsigned long nl,
                                           const iovec* remote, unsigned long nr, unsigned long flags)
{
    return read_failure ? injected_result(local, nl, read_failure)
                        : __real_process_vm_readv(pid, local, nl, remote, nr, flags);
}
extern "C" ssize_t __wrap_process_vm_writev(pid_t pid, const iovec* local, unsigned long nl,
                                            const iovec* remote, unsigned long nr, unsigned long flags)
{
    return write_failure ? injected_result(local, nl, write_failure)
                         : __real_process_vm_writev(pid, local, nl, remote, nr, flags);
}
static void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
template<class F> static void must_fail(F call, const char* message)
{
    bool failed = false;
    try { call(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, message);
}
static std::vector<uint8_t> snapshot(uint64_t address)
{
    // One synthetic saved region: enough structural data for memory readers.
    // A bogus cereal payload deliberately tests that malformed archives fail.
    std::vector<uint8_t> data(sizeof(StateDataHeader) + sizeof(RegionInfo) + 1 + 8 + sizeof(user_regs_struct), 0);
    auto* h = reinterpret_cast<StateDataHeader*>(data.data());
    h->magic = 0x4453544D; h->version = 5; h->num_regions = 1;
    h->tstate_offset = sizeof(StateDataHeader) + sizeof(RegionInfo);
    h->tstate_size = 1; h->maps_offset = h->tstate_offset + 1; h->maps_size = 0;
    h->regs_offset = h->maps_offset + 8;
    auto* r = reinterpret_cast<RegionInfo*>(data.data() + sizeof(StateDataHeader));
    r->start = address; r->end = address + 8; r->offset = h->maps_offset;
    r->prot = PROT_READ | PROT_WRITE; memcpy(r->flags, "rw-p", 5);
    memcpy(data.data() + r->offset, "snapshot", 8);
    return data;
}
int main()
{
    pid_t child = -1;
    char directory[] = "/tmp/detsim-state-io-XXXXXX";
    const auto cwd = std::filesystem::current_path();
    try {
        require(mkdtemp(directory) && chdir(directory) == 0, "temporary directory");
        void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(page != MAP_FAILED, "mmap");
        memcpy(page, "live!!!!", 8);
        child = fork();
        require(child >= 0, "fork");
        if (!child) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            asm volatile("int3");
            _exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && WIFSTOPPED(status), "tracee stop");
        auto& store = StateStore::instance();
        tracee_state state;
        for (int mode : {1, 2}) {
            const auto saves = store.get_stats().save_calls;
            read_failure = mode;
            must_fail([&] { state.save_full_state_to_state_store(child); }, "failed capture was accepted");
            require(store.get_stats().save_calls == saves, "failed capture published a snapshot");
        }
        read_failure = 0;
        auto data = snapshot(reinterpret_cast<uint64_t>(page));
        auto save = [&] { return store.save(data.data(), data.size()); };
        const auto hash = save();
        auto* memory = static_cast<char*>(read_mem(child, hash, reinterpret_cast<uint64_t>(page), 8));
        require(memcmp(memory, "snapshot", 8) == 0, "snapshot read used live memory");
        free(memory);
        must_fail([&] { free(read_mem(child, hash, reinterpret_cast<uint64_t>(page), 9)); }, "partial range accepted");
        must_fail([&] { free(read_mem(child, hash, reinterpret_cast<uint64_t>(page) + 16, 1)); }, "missing region fell back to live memory");
        must_fail([&] { free(read_mem(child, hash, 0, -1)); }, "negative range accepted");
        must_fail([&] { free(read_mem(child, 0xdeadbeef, reinterpret_cast<uint64_t>(page), 1)); }, "missing snapshot fell back");
        must_fail([&] { tracee_state invalid(hash); }, "invalid cereal archive accepted");
        // Split the same data across two contiguous saved regions.
        auto adjacent = data;
        adjacent.insert(adjacent.begin() + sizeof(StateDataHeader) + sizeof(RegionInfo), sizeof(RegionInfo), 0);
        auto* adjacent_header = reinterpret_cast<StateDataHeader*>(adjacent.data());
        adjacent_header->num_regions = 2;
        adjacent_header->tstate_offset += sizeof(RegionInfo);
        adjacent_header->maps_offset += sizeof(RegionInfo);
        adjacent_header->regs_offset += sizeof(RegionInfo);
        auto* adjacent_regions = reinterpret_cast<RegionInfo*>(adjacent.data() + sizeof(StateDataHeader));
        adjacent_regions[0].offset += sizeof(RegionInfo);
        adjacent_regions[1] = adjacent_regions[0];
        adjacent_regions[0].end -= 4;
        adjacent_regions[1].start += 4;
        adjacent_regions[1].offset += 4;
        const auto adjacent_hash = store.save(adjacent.data(), adjacent.size());
        memory = static_cast<char*>(read_mem(child, adjacent_hash, reinterpret_cast<uint64_t>(page), 8));
        require(memcmp(memory, "snapshot", 8) == 0, "read across adjacent saved regions failed");
        free(memory);
        maps_item map{}; map.start = reinterpret_cast<uint64_t>(page); map.end = map.start + 4096;
        memcpy(map.flags, "rw-p", 5);
        std::vector<maps_item> maps{map};
        must_fail([&] { state.restore_memory_mappings(hash, child, maps); }, "missing mapping text accepted");
        for (int mode : {1, 2}) {
            write_failure = mode;
            must_fail([&] { state.recover_mem_reg_snapshot(maps, hash, child); }, "failed restore was accepted");
        }
        write_failure = 0;
        std::vector<maps_item> empty_maps;
        must_fail([&] { state.recover_mem_reg_snapshot(empty_maps, hash, child); }, "missing restore region accepted");
        auto* h = reinterpret_cast<StateDataHeader*>(data.data());
        h->version = 4;
        auto old_hash = save();
        must_fail([&] { tracee_state invalid(old_hash); }, "v4 archive accepted");
        must_fail([&] { free(read_mem(child, old_hash, map.start, 1)); }, "v4 read accepted");
        must_fail([&] { state.recover_mem_reg_snapshot(maps, old_hash, child); }, "v4 restore accepted");
        must_fail([&] { state.restore_memory_mappings(old_hash, child, maps); }, "v4 mappings accepted");
        h->version = 5;
        auto* r = reinterpret_cast<RegionInfo*>(data.data() + sizeof(StateDataHeader));
        r->offset = UINT64_MAX;
        auto bad_hash = save();
        must_fail([&] { free(read_mem(child, bad_hash, map.start, 1)); }, "overflow region accepted");
        kill(child, SIGKILL); waitpid(child, nullptr, 0); child = -1;
        store.shutdown();
        munmap(page, 4096);
        std::filesystem::current_path(cwd);
        std::filesystem::remove_all(directory);
        puts("PASS state I/O: short/error capture not published; short/error restore rejected; saved reads, missing ranges/states, corrupt archives/regions and all v4 readers");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL: %s (artifacts %s)\n", error.what(), directory);
        return 1;
    }
}
