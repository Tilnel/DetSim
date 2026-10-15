#include "ncurses_ui.h"
#include "state_store_packed.h"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static bool short_io = false, fail_write = false, fail_sync = false;
static unsigned sync_count = 0;
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void *p, size_t n)
{
  if (fail_write)
  {
    errno = ENOSPC;
    return -1;
  }
  return __real_write(fd, p, short_io ? std::min(n, size_t{3}) : n);
}
extern "C" ssize_t __wrap_pread(int fd, void *p, size_t n, off_t off)
{
  return __real_pread(fd, p, short_io ? std::min(n, size_t{7}) : n, off);
}
extern "C" int __wrap_fsync(int fd)
{
  ++sync_count;
  if (fail_sync)
  {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}
static void require(bool ok, const char *msg)
{
  if (!ok)
    throw std::runtime_error(msg);
}
template <class F>
static void rejects(F f, const char *m)
{
  bool rejected = false;
  try
  {
    f();
  }
  catch (const std::exception &)
  {
    rejected = true;
  }
  require(rejected, m);
}
static void run_child(const char *self, const std::string &dir)
{
  pid_t p = fork();
  require(p >= 0, "fork");
  if (!p)
  {
    execl(self, self, "read", dir.c_str(), nullptr);
    _exit(99);
  }
  int status;
  require(waitpid(p, &status, 0) == p && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "fresh process read while writer alive");
}
int main(int argc, char **argv)
{
  auto &s = StateStorePacked::instance();
  StateStorePacked::Config c;
  if (argc == 3)
  {
    c.storage_path = argv[2];
    c.read_only = true;
    s.init(c);
    std::vector<uint8_t> out;
    require(s.load(109, out) == 4096 && out[0] == 43, "fresh read");
    s.shutdown();
    return 0;
  }
  char dir[] = "/tmp/detsim-packed-XXXXXX";
  require(mkdtemp(dir), "mkdtemp");
  c.storage_path = dir;
  c.max_entries_per_segment = 2;
  try
  {
    s.init(c);
    {
      const std::vector<uint8_t> sparse(8 * 1024 * 1024, 0);
      const auto compressed = s.compress(sparse);
      require(compressed.capacity() == compressed.size(),
              "compressed buffers retain raw-sized allocation beyond cache accounting");
      require(s.decompress(compressed, sparse.size()) == sparse,
              "tight compressed buffer preserves every decoded byte");
    }
    std::vector<uint8_t> a(4096, 42), out;
    short_io = true;
    require(s.save(a.data(), a.size(), 101) == 101, "save raw");
    require(s.load(101, out) == 4096 && out == a,
            "raw round trip and short IO");
    short_io = false;
    a[0] = 43;
    rejects([&] { s.save(a.data(), a.size(), 101); }, "collision rejection");
    for (unsigned i = 102; i < 110; ++i)
      require(s.save(a.data(), a.size(), i) == i, "rotation save");
    require(s.get_segment_count() == 5, "rotation count");
    s.flush_index();
    run_child(argv[0], dir);
    s.shutdown();
    s.init(c);
    require(s.get_entry_count() == 9 && s.get_segment_count() == 5,
            "reopen rotation");
    for (unsigned i = 102; i < 110; ++i)
      require(s.load(i, out) == 4096 && out == a, "reopen data");
    s.compact();
    require(s.load(109, out) == 4096 && out == a, "compact preserves data");
    s.shutdown();
    std::filesystem::remove(std::string(dir) + "/global.idx");
    std::filesystem::remove(std::string(dir) + "/global.inc");
    s.init(c);
    require(s.get_entry_count() == 9 && s.load(109, out) == 4096,
            "rebuild missing indexes");
    s.save(a.data(), a.size(), 110);
    s.flush_index();
    run_child(argv[0], dir);
    s.shutdown();
    auto segment = std::string(dir) + "/segments/4.dat";
    int fd = open(segment.c_str(), O_RDWR);
    require(fd >= 0, "corrupt open");
    unsigned char byte = 0;
    require(pwrite(fd, &byte, 1, 0) == 1, "corrupt header");
    close(fd);
    s.init(c);
    rejects([&] { s.load_compressed(109, out); }, "prefetch checks header");
    s.shutdown();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    c.max_entries_per_segment = 10000;
    s.init(c);
    sync_count = 0;
    for (unsigned i = 1; i <= 1000; ++i)
      s.save(a.data(), a.size(), i);
    require(sync_count == 0, "no per-save fsync");
    s.flush_index();
    require(sync_count == 4, "constant barrier fsync count");
    fail_sync = true;
    rejects([&] { s.flush_index(); }, "fsync error reported");
    fail_sync = false;
    rejects([&] { s.save(a.data(), a.size(), 2000); }, "failed store sticky");
    rejects([&] { s.shutdown(); }, "shutdown reports failure");
    s.init(c);
    require(s.load(1000, out) == 4096, "reinit after error");
    s.shutdown();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    s.init(c);
    fail_write = true;
    rejects([&] { s.save(a.data(), a.size(), 1); }, "write error reported");
    fail_write = false;
    rejects([&] { s.shutdown(); }, "write failure shutdown");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    s.init(c);
    s.save(a.data(), a.size(), 1);
    s.shutdown();
    std::filesystem::resize_file(std::string(dir) + "/global.inc", 5);
    rejects([&] { s.init(c); }, "truncated journal rejected");
    std::filesystem::remove_all(dir);
    std::cout << "packed storage PASS: raw/compressed integrity, short IO, "
                 "collisions, rotation, process reopen, rebuild, compaction, "
                 "write/fsync errors, journal truncation; 1000 saves=0 syncs, "
                 "barrier=4 syncs\n";
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
