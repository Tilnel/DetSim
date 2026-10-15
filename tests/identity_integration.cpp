#include "config.h"
#include "state/state_transition.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "proc_status.h"
#include "state/sysstate_store.h"
#include "state/page_snapshot.h"
#include "cereal/archives/binary.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

extern TSS state_tree;
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
    StateStore::instance().shutdown();
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
    StoreDirectory directory;
    StateStore::instance().init(StateStore::Config{});
    auto snapshot = [](uint64_t seed) {
      tracee_state saved;saved.process_state.random_state=seed;
      std::ostringstream output;{cereal::BinaryOutputArchive ar(output);ar(saved);}
      const auto bytes=output.str();
      const std::vector<uint8_t> metadata(bytes.begin(),bytes.end());
      const auto manifest=encode_page_manifest({},metadata,"",user_regs_struct{},{});
      return StateStore::instance().save(manifest.data(),manifest.size());
    };
    sys_state original;
    original.ts_hash[0] = snapshot(1);
    original.ts_hash[1] = snapshot(2);
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
    repeated.ts_hash[0] = repeated.ts_hash[1] = original.ts_hash[0];
    auto other = repeated;
    other.ts_hash[0] = other.ts_hash[1] = original.ts_hash[1];
    require(repeated.identity_hash() != other.identity_hash(), "equal node hashes cancelled out");
    require(original.identity_hash() == ordered, "identity is not deterministic");
    auto monitored = original;
    monitored.monitor_states["monitor:v1"] = {1,2,3};
    require(monitored.identity_hash() != ordered, "monitor state omitted from identity");
    auto branch = monitored;
    branch.monitor_states.at("monitor:v1")[0] = 9;
    require(monitored.monitor_states.at("monitor:v1")[0] == 1 &&
            branch.identity_hash() != monitored.identity_hash(), "monitor branches alias or merge");
    auto failed = original;
    failed.property_failed = true;
    require(failed.identity_hash() != ordered, "terminal verdict omitted from identity");
    sys_state parent_a = original, parent_b = original;
    parent_a.error_bound = 4; parent_b.error_bound = 3;
    parent_a.ss_hash = parent_a.identity_hash(); parent_b.ss_hash = parent_b.identity_hash();
    auto passed_target = original; passed_target.ss_hash = passed_target.identity_hash();
    failed.ss_hash = failed.identity_hash();
    state_tree.clear();
    state_tree_add(parent_a, passed_target, 0, 0);
    state_tree_add(parent_b, failed, 0, 0);
    require(std::get<0>(state_tree.at(failed.ss_hash)) == parent_b.ss_hash,
            "failing edge reused an earlier valid witness");
    auto failed_live=failed;failed_live.status[0]=dstatus_normal();
    bool terminal_rejected=false;
    try {(void)state_transition(failed_live,0);}
    catch(const std::runtime_error& error) {
      terminal_rejected=std::string(error.what()).find("terminal property violation")!=std::string::npos;
    }
    require(terminal_rejected,"terminal property verdict did not reject at entry");
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
    auto dangling=original;dangling.ts_hash[0]=0x1111;dangling.commit_metadata();
    rejects([&] {sys_state missing_image(dangling.ss_hash);},
            "terminated node with missing snapshot was accepted");
    monitored.commit_metadata();
    sys_state monitor_loaded(monitored.ss_hash);
    require(monitor_loaded.monitor_states == monitored.monitor_states, "monitor archive roundtrip");
    auto model = monitored;
    model.model_identity = "different model";
    require(model.identity_hash() != monitored.identity_hash(), "model context omitted from identity");
    failed.commit_metadata();
    require(sys_state(failed.ss_hash).property_failed, "terminal verdict lost on reload");
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
