#include "state_store.h"
#include "state_store_packed.h"
#include "debug.h"
#include "ncurses_ui.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool hold_compression = false, compression_entered = false;
static bool fail_compression = false;
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (fail_sync.load()) {errno = EIO; return -1;}
  return __real_fsync(fd);
}
template <typename F> static void rejects(F operation, const char *message) {
  bool failed = false; try {operation();} catch (const std::exception &) {failed = true;}
  require(failed, message);
}
extern "C" std::vector<uint8_t> real_compress(StateStorePacked *, const std::vector<uint8_t> &)
  asm("__real__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked *store, const std::vector<uint8_t> &data)
  asm("__wrap__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked *store, const std::vector<uint8_t> &data) {
  std::unique_lock lock(gate_mutex);
  compression_entered = true;
  gate_cv.notify_all();
  gate_cv.wait(lock, [] { return !hold_compression; });
  if (fail_compression) throw std::runtime_error("injected compression failure");
  lock.unlock();
  return real_compress(store, data);
}
static void release_compression() {
  std::lock_guard lock(gate_mutex);
  hold_compression = false;
  gate_cv.notify_all();
}
static void await_compression() {
  std::unique_lock lock(gate_mutex);
  require(gate_cv.wait_for(lock, std::chrono::seconds(3), [] {return compression_entered;}), "worker did not reach compression gate");
}
static std::vector<uint8_t> payload(unsigned seed) {
  std::vector<uint8_t> result(4096);
  for (auto &byte : result) { seed = seed * 1664525 + 1013904223; byte = seed >> 24; }
  return result;
}
static StateStore::Config config(size_t hot = 8192, size_t warm = 8192) {
  StateStore::Config result;
  result.hot_cache_size = hot; result.warm_cache_size = warm;
  result.prefetch_threads = 0; result.prefetch_window = 0;
  result.packed_storage_path = "packed";
  return result;
}
static int run_child(const char *executable, const char *mode, const char *hash = nullptr) {
  const pid_t pid = fork();
  require(pid >= 0, "fork");
  if (!pid) { execl(executable, executable, mode, hash, nullptr); _exit(127); }
  int status; require(waitpid(pid, &status, 0) == pid, "waitpid");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
static void run_case(const char *mode, const char *executable, const char *hash_arg) {
  auto &store = StateStore::instance();
  if (!strcmp(mode, "reader")) {
    StateStorePacked::Config cfg; cfg.storage_path = "packed"; cfg.read_only = true;
    auto &packed = StateStorePacked::instance(); packed.init(cfg);
    std::vector<uint8_t> result;
    require(packed.load(std::stoull(hash_arg), result) == 4096 && result == payload(11), "fresh process cannot read completed write");
    packed.shutdown(); return;
  }
  if (!strcmp(mode, "barrier")) {
    store.init(config());
    hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size());
    await_compression();
    auto barrier = std::async(std::launch::async, [&] { store.wait_for_completion(); });
    bool blocked = barrier.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout;
    release_compression(); barrier.get();
    require(blocked, "completion barrier returned while worker held before persistence");
    auto encoded = std::to_string(hash);
    require(run_child(executable, "reader", encoded.c_str()) == 0, "fresh-process reload after wait failed");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "lifetime")) {
    store.init(config(4096, 0));
    auto data = payload(11); auto hash = store.save(data.data(), data.size());
    auto held = store.load_shared(hash);
    require(held && held.use_count() > 0, "load_shared returned a non-owning pointer");
    store.wait_for_completion();
    for (unsigned i = 20; i < 40; ++i) {auto other = payload(i); store.save(other.data(), other.size());}
    store.wait_for_completion();
    require(*held == data, "eviction mutated retained shared bytes");
    require(store.get_l1_usage() <= 4096 && store.get_l2_usage() == 0, "cache exceeded byte budget");
    store.shutdown(); require(*held == data, "shutdown invalidated retained shared bytes"); return;
  }
  if (!strcmp(mode, "errors")) {
    store.init(config()); fail_compression = true;
    auto data = payload(11); store.save(data.data(), data.size());
    bool failed = false; try {store.wait_for_completion();} catch (const std::exception &) {failed = true;}
    require(failed, "worker failure was not propagated");
    failed = false; try {store.save(data.data(), data.size());} catch (const std::exception &) {failed = true;}
    require(failed, "save accepted data after worker failure");
    failed = false; try {store.shutdown();} catch (const std::exception &) {failed = true;}
    require(failed, "shutdown concealed worker failure");
    fail_compression = false; store.init(config());
    auto hash = store.save(data.data(), data.size()); store.wait_for_completion();
    require(*store.load_shared(hash) == data, "reinit after failure did not recover"); store.shutdown(); return;
  }
  if (!strcmp(mode, "concurrent")) {
    store.init(config(0, 0)); hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size()); await_compression();
    std::vector<std::future<void>> work;
    for (unsigned i = 0; i < 8; ++i) work.push_back(std::async(std::launch::async, [&] {
      for (unsigned j = 0; j < 40; ++j) {
        require(store.save(data.data(), data.size()) == hash, "concurrent duplicate hash");
        require(*store.load_shared(hash) == data, "pending write not readable");
      }
    }));
    for (auto &task : work) task.get();
    require(store.get_stats().save_new == 1, "concurrent dedup queued multiple writes");
    release_compression(); store.wait_for_completion();
    require(*store.load_shared(hash) == data, "zero-cache disk read");
    require(store.get_l1_usage() == 0 && store.get_l2_usage() == 0, "zero cache retained data");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "shutdown")) {
    store.init(config(0, 0)); hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size()); await_compression();
    for (unsigned i = 1; i < 20; ++i) {auto other=payload(i); store.save(other.data(),other.size());}
    auto stopping = std::async(std::launch::async,[&]{store.shutdown();});
    require(stopping.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "shutdown did not wait for accepted writes");
    release_compression(); stopping.get();
    store.init(config(0, 0));
    require(*store.load_shared(hash) == data, "reinit lost first write");
    for (unsigned i = 1; i < 20; ++i) {auto other=payload(i); auto key=store.save(other.data(),other.size()); require(store.get_stats().save_new == 0, "shutdown dropped queued write"); require(*store.load_shared(key)==other,"reinit bytes differ");}
    store.shutdown(); return;
  }
  if (!strcmp(mode, "backpressure")) {
    auto cfg = config(0,0); cfg.max_pending_bytes = 4096; store.init(cfg);
    hold_compression = true;
    auto first = payload(11); store.save(first.data(), first.size()); await_compression();
    auto second = std::async(std::launch::async, [&] {auto bytes=payload(12); return store.save(bytes.data(),bytes.size());});
    require(second.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "pending-byte budget excluded active worker");
    release_compression(); auto second_hash = second.get(); store.wait_for_completion();
    require(*store.load_shared(second_hash) == payload(12), "backpressure lost write");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "multiworker")) {
    auto cfg = config(4096,4096); cfg.io_threads = 4; store.init(cfg);
    std::vector<std::future<void>> tasks;
    for (unsigned thread=0; thread<4; ++thread) tasks.push_back(std::async(std::launch::async, [&,thread] {
      for (unsigned i=1; i<=40; ++i) {
        auto bytes=payload(thread*100+i); auto hash=store.save(bytes.data(),bytes.size());
        auto held=store.load_shared(hash); require(held && *held==bytes,"multiworker read bytes");
      }
    }));
    for (auto &task : tasks) task.get();
    store.wait_for_completion();
    require(store.get_l1_usage()<=4096 && store.get_l2_usage()<=4096,"multiworker cache cap");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "prefetch")) {
    auto cfg=config(0,8192); cfg.prefetch_threads=2; cfg.prefetch_window=2;
    store.init(cfg);
    auto first=payload(11); auto hash=store.save(first.data(),first.size());
    store.wait_for_completion(); store.shutdown(); store.init(cfg);
    store.queue_push_back(1); store.queue_push_back(hash); store.queue_pop_front();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!store.in_l2(hash) && std::chrono::steady_clock::now()<deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(store.in_l2(hash),"prefetch did not populate L2");
    require(*store.load_shared(hash)==first,"prefetched bytes differ");
    require(store.get_stats().prefetch_issued==1,"prefetch issue accounting");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "sync_error")) {
    store.init(config()); auto data=payload(11); auto hash=store.save(data.data(),data.size());
    fail_sync=true;
    rejects([&]{store.wait_for_completion();},"sync error hidden"); fail_sync=false;
    rejects([&]{store.save(data.data(),data.size());},"cached save after failed barrier");
    rejects([&]{store.load_shared(hash);},"cached load after failed barrier");
    rejects([&]{store.wait_for_completion();},"repeat barrier after failure");
    rejects([&]{store.shutdown();},"shutdown hid barrier failure");
    return;
  }
  if (!strcmp(mode, "config")) {
    auto cfg=config(); cfg.io_threads=0;
    rejects([&]{store.init(cfg);},"zero workers accepted");
    cfg.io_threads=-1; rejects([&]{store.init(cfg);},"negative workers accepted");
    cfg.io_threads=1; cfg.max_pending_bytes=0;
    rejects([&]{store.init(cfg);},"zero pending budget accepted");
    store.init(config()); auto bytes=payload(11);
    rejects([&]{store.save(bytes.data(), size_t(UINT32_MAX)+1);},"oversized snapshot accepted");
    auto hash=state_store_save(bytes.data(),bytes.size());
    require(hash==store.save(bytes.data(),bytes.size()),"C interface truncated hash");
    std::vector<uint8_t> small(1);
    rejects([&]{state_store_load(hash,small.data(),small.size());},"C interface silently truncated state");
    store.shutdown();
    cfg=config(); cfg.read_only=true; store.init(cfg);
    require(*store.load_shared(hash)==bytes,"readonly reload");
    rejects([&]{store.save(bytes.data(),bytes.size());},"readonly save accepted");
    store.shutdown(); return;
  }
  throw std::runtime_error("unknown case");
}
int main(int argc, char **argv) {
  loglevel = 4;
  alarm(30);
  try {
    const std::string executable = std::filesystem::canonical("/proc/self/exe");
    if (argc > 1) {run_case(argv[1], executable.c_str(), argc > 2 ? argv[2] : nullptr); return 0;}
    for (auto mode : {"lifetime", "barrier", "concurrent", "shutdown", "errors", "backpressure", "multiworker", "prefetch", "sync_error", "config"}) {
      auto old = std::filesystem::current_path();
      char pattern[] = "/tmp/detsim-store-cache-XXXXXX";
      require(mkdtemp(pattern) != nullptr,"mkdtemp");
      std::filesystem::current_path(pattern);
      int status = run_child(executable.c_str(), mode);
      std::filesystem::current_path(old); std::filesystem::remove_all(pattern);
      require(status == 0, mode); std::printf("PASS store cache: %s\n", mode);
    }
  } catch (const std::exception &error) {std::fprintf(stderr,"FAIL store cache: %s\n",error.what()); return 1;}
}
