#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "proc_status.h"
#include "state/sysstate_store.h"
#include "cereal/archives/binary.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void require(bool ok, const char *message)
{
  if (!ok) throw std::runtime_error(message);
}

template <typename F> static void rejects(F operation, const char *message)
{
  bool rejected = false;
  try { operation(); }
  catch (const std::exception &) { rejected = true; }
  require(rejected, message);
}

struct StoreDirectory
{
  std::filesystem::path old = std::filesystem::current_path();
  std::filesystem::path path;
  StoreDirectory()
  {
    char pattern[] = "/tmp/detsim-identity-XXXXXX";
    require(mkdtemp(pattern) != nullptr, "temporary store directory");
    path = pattern;
    std::filesystem::current_path(path);
  }
  ~StoreDirectory()
  {
    SysStateStore::instance().shutdown();
    std::filesystem::current_path(old);
    std::filesystem::remove_all(path);
  }
};

int main()
{
  static_assert(NP == 2);
  loglevel = 4;
  try {
    sys_state original;
    original.ts_hash[0] = 0x1111;
    original.ts_hash[1] = 0x2222;
    original.status[0] = original.status[1] = dstatus_exit(0);
    original.error_bound = 5;
    auto ordered = original.identity_hash();
    auto swapped = original;
    std::swap(swapped.ts_hash[0], swapped.ts_hash[1]);
    require(swapped.identity_hash() != ordered, "node order disappeared from identity");
    auto exited = original;
    exited.status[0] = dstatus_exit(1);
    require(exited.identity_hash() != ordered, "exit status disappeared from identity");
    auto budget = original;
    budget.error_bound = 4;
    require(budget.identity_hash() != ordered, "remaining error budget disappeared from identity");
    auto repeated = original;
    repeated.ts_hash[0] = repeated.ts_hash[1] = 0x1111;
    auto other = repeated;
    other.ts_hash[0] = other.ts_hash[1] = 0x2222;
    require(repeated.identity_hash() != other.identity_hash(), "equal node hashes cancelled out");
    require(original.identity_hash() == ordered, "identity is not deterministic");
    StoreDirectory directory;
    original.ss_hash = ordered;
    require(original.save_metadata() == 1, "save original metadata");
    require(original.save_metadata() == 0, "identical metadata deduplicates");
    swapped.ss_hash = swapped.identity_hash();
    require(swapped.save_metadata() == 1, "swapped metadata not merged");
    sys_state loaded(original.ss_hash);
    require(loaded.ts_hash[0] == original.ts_hash[0] &&
            loaded.ts_hash[1] == original.ts_hash[1] &&
            loaded.error_bound == original.error_bound &&
            loaded.status[0] == original.status[0], "metadata round trip");
    auto wrong = original;
    wrong.error_bound--;
    rejects([&] { wrong.save_metadata(); }, "modified metadata reused stale identity");
    rejects([] { sys_state missing(hash_type{0}); }, "zero identity accepted");

    std::ostringstream encoded;
    { cereal::BinaryOutputArchive archive(encoded); archive(original); }
    const std::string bytes = encoded.str();
    // Inject a valid archive under a conflicting key, exercising the boundary
    // without relying on finding a probabilistic 64-bit hash collision.
    auto &store = SysStateStore::instance();
    require(store.save(budget.identity_hash(), {bytes.begin(), bytes.end()}), "inject metadata mismatch");
    rejects([&] { sys_state wrong_key(budget.identity_hash()); }, "wrong-key metadata accepted");
    budget.ss_hash = budget.identity_hash();
    rejects([&] { budget.save_metadata(); }, "conflicting metadata silently merged");
    std::puts("PASS system identity: ordered nodes, exit/budget, no XOR cancellation, metadata round trip, conflicts rejected");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL system identity: %s\n", error.what());
    return 1;
  }
}
