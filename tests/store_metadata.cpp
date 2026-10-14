#include "sysstate_store.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static void require(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
template<class F> static void rejects(F fn, const char *why) {
  bool rejected = false; try { fn(); } catch (const std::exception &) { rejected = true; }
  require(rejected, why);
}
static int write_fault = 0;
static bool sync_fault = false;
static size_t read_calls = 0;
extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void *data, size_t size, off_t offset) {
  ++read_calls;
  return __real_pread(fd, data, size, offset);
}
extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset) {
  if (write_fault == 1) { write_fault = 2; errno = EINTR; return -1; }
  if (write_fault == 2) { write_fault = 0; return __real_pwrite(fd, data, size > 3 ? 3 : size, offset); }
  if (write_fault == 4) { write_fault = 3; return __real_pwrite(fd, data, size > 3 ? 3 : size, offset); }
  if (write_fault == 3) { errno = ENOSPC; return -1; }
  return __real_pwrite(fd, data, size, offset);
}
extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd) {
  if (sync_fault) { errno = EIO; return -1; }
  return __real_fdatasync(fd);
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && strcmp(argv[1], "reload") == 0) {
      std::vector<uint8_t> value;
      require(SysStateStore::instance().load(2, value) && value == std::vector<uint8_t>({4,5,6}), "fresh process reload");
      SysStateStore::instance().shutdown();
      return 0;
    }
    if (argc == 2) { // Separate process must be unable to open an owned store.
      rejects([] { SysStateStore::instance(); }, "second process opened owned store");
      return 0;
    }
    auto executable = std::filesystem::canonical(argv[0]);
    char pattern[] = "/tmp/detsim-store-metadata-XXXXXX";
    require(mkdtemp(pattern), "mkdtemp");
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(pattern);
    auto &store = SysStateStore::instance();
    std::vector<uint8_t> a{1,2,3}, b{4,5,6}, result{99};
    require(store.save(0xabc0000000000000, a), "save");
    require(store.save(0xabc0000000000000, a), "duplicate");
    rejects([&] { store.save(0xabc0000000000000, b); }, "conflicting hash silently accepted");
    require(store.get_entry_count() == 1, "duplicate appended");
    require(!store.load(42, result) && result == std::vector<uint8_t>{99}, "missing load modified output");
    require(store.find_by_prefix("abc").size() == 1, "prefix");
    require(store.find_by_prefix("abc!").empty(), "malformed prefix accepted");
    require(store.find_by_prefix("abc00000000000000").empty(), "oversized prefix accepted");
    pid_t pid = fork(); require(pid >= 0, "fork");
    if (pid == 0) { execl(executable.c_str(), executable.c_str(), "locked", nullptr); _exit(127); }
    int status; require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "cross-process lock");
    rejects([&] { SysStateStore::inspect("sstate", [](auto, const auto &, const auto &) {}); }, "inspection raced active writer");
    write_fault = 1;
    require(store.save(2, b), "retry EINTR/short write");
    require(write_fault == 0, "write fault not exercised");
    store.flush_index(); store.shutdown();
    size_t inspected = 0;
    SysStateStore::inspect("sstate", [&](hash_type, const SysStateEntry &, const std::vector<uint8_t> &) { ++inspected; });
    require(inspected == 2, "read-only scanner missed records");
    rejects([&] { SysStateStore::inspect("missing", [](auto, const auto &, const auto &) {}); }, "missing inspection accepted");
    require(!std::filesystem::exists("missing"), "inspection created store");
    pid = fork(); require(pid >= 0, "reload fork");
    if (pid == 0) { execl(executable.c_str(), executable.c_str(), "reload", nullptr); _exit(127); }
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh process reload");
    // Historical indexes are only caches: stale/broken files must never hide data.
    std::ofstream("sstate/packed.idx", std::ios::binary) << "stale";
    std::ofstream("sstate/packed.inc", std::ios::binary) << "partial";
    store.init();
    require(store.get_entry_count() == 2 && store.load(2, result) && result == b, "reopen authoritative log");
    store.compact(); store.rebuild_index();
    require(store.load(0xabc0000000000000, result) && result == a, "maintenance preserved values");
    std::thread threads[4];
    for (int n=0; n<4; ++n) threads[n] = std::thread([&,n] {
      for (int i=0; i<100; ++i) {
        auto &same = SysStateStore::instance();
        require(same.save(100+n*100+i, a), "concurrent save");
        std::vector<uint8_t> value; require(same.load(2,value) && value==b, "concurrent load");
      }
    });
    for (auto &thread: threads) thread.join();
    require(store.get_entry_count() == 402, "concurrent count");
    // Both cache limits must retain disk equivalence after eviction.
    for (int i=0; i<17000; ++i) require(store.save(20000+i, a), "entry cache pressure");
    read_calls = 0;
    require(store.load(2, result) && result == b && read_calls > 0, "evicted entry disk load");
    read_calls = 0;
    require(store.load(2, result) && result == b && read_calls == 0, "hot cache read");
    std::vector<uint8_t> large(600000, 77);
    require(store.save(40000, large) && store.save(40001, large), "byte cache pressure");
    read_calls = 0;
    require(store.load(40000, result) && result == large && read_calls > 0, "byte eviction disk load");
    sync_fault = true;
    rejects([&] { store.flush_index(); }, "flush failure hidden");
    rejects([&] { store.save(10000, a); }, "failed store accepted write");
    sync_fault = false;
    try { store.shutdown(); } catch (const std::exception &) {}
    store.init();
    write_fault = 3;
    rejects([&] { store.save(10001, a); }, "write failure hidden");
    write_fault = 0;
    rejects([&] { store.save(10002, a); }, "failed append not sticky");
    try { store.shutdown(); } catch (const std::exception &) {}
    store.init();
    auto clean_size = std::filesystem::file_size("sstate/packed.dat");
    write_fault = 4;
    rejects([&] { store.save(10003, a); }, "partial failed write hidden");
    write_fault = 0;
    try { store.shutdown(); } catch (const std::exception &) {}
    rejects([&] { store.init(); }, "partial failed append accepted on reopen");
    require(std::filesystem::file_size("sstate/packed.dat") == clean_size + 3, "partial append silently removed");
    std::filesystem::resize_file("sstate/packed.dat", clean_size); // Test repairs its own deliberate corruption.
    store.init(); store.shutdown();
    auto original_size = std::filesystem::file_size("sstate/packed.dat");
    std::ofstream("sstate/packed.dat", std::ios::binary | std::ios::app).put('x');
    rejects([&] { store.init(); }, "partial tail silently ignored");
    require(std::filesystem::file_size("sstate/packed.dat") == original_size + 1, "corruption silently repaired");
    std::filesystem::resize_file("sstate/packed.dat", original_size);
    { std::fstream file("sstate/packed.dat", std::ios::binary | std::ios::in | std::ios::out); file.seekp(20); file.put('x'); }
    rejects([&] { store.init(); }, "checksum corruption accepted");
    std::filesystem::current_path(previous); std::filesystem::remove_all(pattern);
    std::puts("PASS metadata store: collision, exact IO, flush failure, lock, concurrency, legacy log, corruption, maintenance");
    return 0;
  } catch (const std::exception &error) { std::fprintf(stderr,"FAIL metadata store: %s\n",error.what()); return 1; }
}
