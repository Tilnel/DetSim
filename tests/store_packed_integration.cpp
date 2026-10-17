#include "ncurses_ui.h"
#include "state_store_packed.h"
#include <cerrno>
#include <array>
#include <cstdint>
#include <cstring>
#include <zstd.h>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static bool short_io = false, fail_write = false, fail_sync = false;
static unsigned sync_count = 0;
static bool bound_allocations = false;
static size_t largest_allocation = 0;
extern "C" void* __real__Znwm(size_t);
extern "C" void* __wrap__Znwm(size_t size)
{
  if (bound_allocations)
    largest_allocation = std::max(largest_allocation, size);
  return __real__Znwm(size);
}
static bool poison_arrays = false;
static size_t poison_expected_size = 0, poison_hits = 0;
static uint8_t poison_value = 0;
extern "C" void* __real__Znam(size_t);
extern "C" void* __wrap__Znam(size_t size)
{
  if (bound_allocations)
    largest_allocation = std::max(largest_allocation, size);
  void* result = __real__Znam(size);
  if (poison_arrays && size == poison_expected_size) {
    std::memset(result, poison_value, size);
    ++poison_hits;
  }
  return result;
}
struct CompressionArrayPoison {
  explicit CompressionArrayPoison(size_t bound, uint8_t value) {
    poison_expected_size = bound; poison_value = value; poison_hits = 0;
    poison_arrays = true;
  }
  ~CompressionArrayPoison() { poison_arrays = false; }
};
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
static std::vector<uint8_t> zero_filled_reference(const std::vector<uint8_t>& raw, int level)
{
  std::vector<uint8_t> output(ZSTD_compressBound(raw.size()), 0);
  auto* context = ZSTD_createCCtx();
  require(context != nullptr, "reference compression context");
  const size_t written = ZSTD_compressCCtx(context, output.data(), output.size(),
                                         raw.data(), raw.size(), level);
  ZSTD_freeCCtx(context);
  require(!ZSTD_isError(written), "reference compression failed");
  output.resize(written);
  return output;
}
static void compression_output_ignores_initial_array_contents(StateStorePacked& store, int level)
{
  std::vector<uint8_t> random(65537);
  uint32_t generator = 0x31415926;
  for (auto& byte : random) {
    generator ^= generator << 13; generator ^= generator >> 17; generator ^= generator << 5;
    byte = static_cast<uint8_t>(generator);
  }
  std::vector<uint8_t> sparse(1024 * 1024, 0);
  for (size_t position = 0; position < sparse.size(); position += 4096)
    sparse[position] = static_cast<uint8_t>(position / 4096);
  const std::array<std::vector<uint8_t>, 5> inputs{
      std::vector<uint8_t>{0x7f}, std::vector<uint8_t>{0, 1, 2, 3, 0xff, 4, 0, 5},
      random, std::vector<uint8_t>(1024 * 1024, 0), sparse};
  unsigned comparisons = 0;
  for (const auto& input : inputs) {
    const auto expected = zero_filled_reference(input, level);
    for (uint8_t poison : {uint8_t(0), uint8_t(0xa5), uint8_t(0x5a)}) {
      std::vector<uint8_t> compressed;
      {
        CompressionArrayPoison scoped(ZSTD_compressBound(input.size()), poison);
        compressed = store.compress(input);
      }
      require(compressed == expected, "poisoned output differs from complete zero-filled ZSTD reference");
      require(compressed.capacity() == compressed.size(), "poisoned output retains unaccounted capacity");
      require(store.decompress(compressed, input.size()) == input, "poisoned output lost raw bytes");
      require(poison_hits == 1, "compression poison did not hit exactly one bound-sized output array allocation");
      ++comparisons;
    }
  }
  for (uint8_t poison : {uint8_t(0), uint8_t(0xa5), uint8_t(0x5a)}) {
    std::vector<uint8_t> empty;
    {
      CompressionArrayPoison scoped(ZSTD_compressBound(0), poison);
      empty = store.compress({});
    }
    require(empty.empty() && empty.capacity() == 0 && poison_hits == 0,
            "empty input allocated or published a compression frame");
    rejects([&] { store.decompress(empty, 0); }, "empty compressed payload unexpectedly decodes");
  }
  std::cout << "compression output PASS: " << comparisons
            << " exact poisoned-array frame/roundtrip/capacity comparisons; 3 empty-input checks\n";
}
static void encoded_validation(StateStorePacked& store, int level)
{
  const std::vector<uint8_t> raw(8 * 1024 * 1024, 0x39);
  const auto frame = zero_filled_reference(raw, level);
  const auto entries = store.get_entry_count();
  auto invalid = [&](const std::vector<uint8_t>& bytes, size_t size) {
    rejects([&] { store.save(bytes.data(), bytes.size(), 900001, true, size); },
            "invalid encoded frame published");
    require(store.get_entry_count() == entries, "invalid encoded save changed index");
    std::vector<uint8_t> missing;
    require(store.load(900001, missing) == -1, "invalid encoded key visible");
  };
  auto truncated = frame; truncated.pop_back();
  invalid(truncated, raw.size());
  invalid(frame, raw.size() - 1);
  invalid(frame, raw.size() + 1);
  auto trailing = frame; trailing.push_back(0);
  invalid(trailing, raw.size());
  auto concatenated = frame;
  concatenated.insert(concatenated.end(), frame.begin(), frame.end());
  invalid(concatenated, raw.size());
  // Reserved block type preserves frame header and declared size, but is undecodable.
  auto malformed = frame;
  const size_t header = 4 + 1 + ((frame[4] & 0x20) ? 0 : 1) +
      ((frame[4] >> 6) == 0 ? ((frame[4] & 0x20) ? 1 : 0) :
       ((frame[4] >> 6) == 1 ? 2 : ((frame[4] >> 6) == 2 ? 4 : 8)));
  malformed[header] |= 6;
  require(ZSTD_getFrameContentSize(malformed.data(), malformed.size()) == raw.size(),
          "malformed fixture lost valid content-size header");
  invalid(malformed, raw.size());

  auto* checksum_context = ZSTD_createCCtx();
  require(checksum_context != nullptr, "checksum context");
  require(!ZSTD_isError(ZSTD_CCtx_setParameter(checksum_context, ZSTD_c_checksumFlag, 1)),
          "checksum parameter");
  std::vector<uint8_t> corrupt_checksum(ZSTD_compressBound(raw.size()));
  const size_t checksum_size = ZSTD_compress2(checksum_context, corrupt_checksum.data(),
      corrupt_checksum.size(), raw.data(), raw.size());
  ZSTD_freeCCtx(checksum_context);
  require(!ZSTD_isError(checksum_size), "checksum fixture compression");
  corrupt_checksum.resize(checksum_size);
  corrupt_checksum.back() ^= 1;
  require(ZSTD_findFrameCompressedSize(corrupt_checksum.data(), corrupt_checksum.size()) ==
              corrupt_checksum.size(), "checksum fixture no longer complete frame");
  invalid(corrupt_checksum, raw.size());

  largest_allocation = 0;
  bound_allocations = true;
  try {
    require(store.save(frame.data(), frame.size(), 900001, true, raw.size()) == 900001,
            "valid encoded save");
  } catch (...) { bound_allocations = false; throw; }
  bound_allocations = false;
  require(largest_allocation < 256 * 1024,
          "new encoded save allocates discarded snapshot-sized C++ buffer");
  std::vector<uint8_t> loaded;
  require(store.load(900001, loaded) == static_cast<ssize_t>(raw.size()) && loaded == raw,
          "encoded round trip");
  // Equal raw bytes encoded at another level must deduplicate; unequal bytes collide.
  const auto alternative = zero_filled_reference(raw, level == 1 ? 3 : 1);
  require(store.save(alternative.data(), alternative.size(), 900001, true, raw.size()) == 900001,
          "different encoding of equal bytes rejected");
  auto other = raw; other.back() ^= 1;
  const auto collision = zero_filled_reference(other, level);
  rejects([&] { store.save(collision.data(), collision.size(), 900001, true, raw.size()); },
          "encoded raw-byte collision accepted");
  require(store.load(900001, loaded) == static_cast<ssize_t>(raw.size()) && loaded == raw,
          "collision changed saved bytes");
  // Large advertised windows accepted by the one-shot API must stay accepted.
  for (uint8_t descriptor : {uint8_t(0x90), uint8_t(0xa8), uint8_t(0xa9), uint8_t(0xaf)}) {
    // 256 MiB, 2 GiB, 2.25 GiB and 3.75 GiB advertised windows.
    auto wide = frame;
    require(!(wide[4] & 3), "wide-window fixture has dictionary ID");
    if (wide[4] & 0x20) {
      wide[4] &= ~0x20;
      wide.insert(wide.begin() + 5, descriptor);
    } else {
      wide[5] = descriptor;
    }
    require(store.decompress(wide, raw.size()) == raw, "wide-window reference decode");
    const hash_type key = 900002 + descriptor;
    require(store.save(wide.data(), wide.size(), key, true, raw.size()) == key,
            "valid wide-window frame rejected");
    require(store.load(key, loaded) == static_cast<ssize_t>(raw.size()) && loaded == raw,
            "wide-window stored round trip");
    auto corrupt_wide = corrupt_checksum;
    if (corrupt_wide[4] & 0x20) {
      corrupt_wide[4] &= ~0x20;
      corrupt_wide.insert(corrupt_wide.begin() + 5, descriptor);
    } else {
      corrupt_wide[5] = descriptor;
    }
    const auto before = store.get_entry_count();
    rejects([&] { store.decompress(corrupt_wide, raw.size()); },
            "corrupt wide-window reference accepted");
    rejects([&] { store.save(corrupt_wide.data(), corrupt_wide.size(), 900003,
                             true, raw.size()); }, "corrupt wide-window save accepted");
    require(store.get_entry_count() == before && store.load(900003, loaded) == -1,
            "corrupt wide-window key published");
  }
  std::cout << "encoded validation PASS: bounded C++ allocation, malformed/truncated/size/trailing/"
               "concatenated rejection, exact collisions, context reuse, wide window\n";
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
    compression_output_ignores_initial_array_contents(s, c.compression_level);
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
    encoded_validation(s, c.compression_level);
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
