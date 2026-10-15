#include "plugins.h"
#include "state/state_view.h"
#include "page_snapshot.h"
#include "cereal/archives/binary.hpp"
#include <sstream>
#include "ncurses_ui.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F call, const char *message) {
  bool failed = false; try { call(); } catch (const std::exception &) { failed = true; }
  require(failed, message);
}
hash_type save_counter(int64_t value, bool pages) {
  auto &store = StateStore::instance();
  RegionInfo region{}; region.start = 0x1000; region.end = 0x1008;
  region.prot = PROT_READ | PROT_WRITE; std::memcpy(region.flags, "rw-p", 5);
  user_regs_struct regs{};
  tracee_state saved;
  saved.fs_state.filesystem.at(saved.fs_state.open_files.at(1).path).content={'a','b','c'};
  saved.fs_state.filesystem.at(saved.fs_state.open_files.at(1).path).metadata.st_size=3;
  std::ostringstream encoded; { cereal::BinaryOutputArchive archive(encoded); archive(saved); }
  const auto serialized=encoded.str(); std::vector<uint8_t> metadata(serialized.begin(),serialized.end());
  if (pages) {
    auto chunk = store.save(&value, sizeof(value));
    auto data = encode_page_manifest({region}, metadata, "", regs, {{region.start, 8, chunk}});
    return store.save(data.data(), data.size());
  }
  StateDataHeader header{}; header.magic = 0x4453544D; header.version = FLAT_SNAPSHOT_VERSION; header.num_regions = 1;
  header.tstate_offset = sizeof(header) + sizeof(region); header.tstate_size = metadata.size();
  header.maps_offset = header.tstate_offset + header.tstate_size;
  region.offset = header.maps_offset; header.regs_offset = region.offset + sizeof(value);
  std::vector<uint8_t> bytes(header.regs_offset + sizeof(regs));
  std::memcpy(bytes.data(), &header, sizeof(header)); std::memcpy(bytes.data() + sizeof(header), &region, sizeof(region));
  std::memcpy(bytes.data() + header.tstate_offset, metadata.data(), metadata.size());
  std::memcpy(bytes.data() + region.offset, &value, sizeof(value));
  return store.save(bytes.data(), bytes.size());
}
sys_state state(int64_t counter, bool pages) {
  sys_state result;
  for (unsigned i = 0; i < NP; ++i) result.ts_hash[i] = save_counter(counter, pages);
  return result;
}
}
int main(int argc, char **argv) {
  if (argc != 5) { std::fprintf(stderr, "usage: plugin_integration GOOD.so LEGACY.so BAD_ABI.so DECODER.so\n"); return 2; }
  char directory[] = "/tmp/detsim-plugin-XXXXXX";
  if (!mkdtemp(directory)) return 1;
  try {
    StateStore::Config config; config.packed_storage_path = std::string(directory) + "/packed";
    config.hot_cache_size = 1; config.warm_cache_size = 1; config.prefetch_threads = 0;
    StateStore::instance().init(config);
    rejects([&] { detsim::plugins::load("old.cpp"); }, "source plugin accepted");
    rejects([&] { detsim::plugins::load(argv[2]); }, "legacy plugin accepted");
    rejects([&] { detsim::plugins::load(argv[3]); }, "bad ABI accepted");
    for (bool pages : {false, true}) {
      detsim::plugins::reset(); detsim::plugins::load(argv[1]);
      auto a = state(10, pages);
      int64_t probe = 0; detsim::StateView(a).read(0, 0x1000, &probe, sizeof(probe)); require(probe == 10, "fixture memory");
      require(!detsim::plugins::check_initial(a), "initial violation");
      require(a.monitor_states.size() == 1 && a.monitor_states.begin()->second == std::vector<uint8_t>{0}, "monitor init");
      auto original = a.monitor_states;
      auto b = state(11, pages), c = state(12, !pages);
      detsim_event_v1 event{}; event.choice = 1;
      require(!detsim::plugins::check_transition(a, b, event), "forward violation");
      event.choice = 2; require(!detsim::plugins::check_transition(a, c, event), "branch violation");
      require(b.monitor_states.begin()->second == std::vector<uint8_t>{1} && c.monitor_states.begin()->second == std::vector<uint8_t>{2} && a.monitor_states == original, "monitor branches alias");
      auto replay = state(11, pages); event.choice = 1;
      detsim::plugins::check_transition(a, replay, event); require(replay.monitor_states == b.monitor_states, "branch replay differs");
      auto terminal_source = a; terminal_source.property_failed = true;
      auto forbidden_branch = state(11, pages);
      rejects([&] { detsim::plugins::check_transition(terminal_source, forbidden_branch, event); }, "terminal source with valid monitor accepted");
      auto partial = state(11, pages); event.choice = 99;
      require(detsim::plugins::check_transition(a, partial, event), "partial monitor violation lost");
      require(partial.monitor_states == a.monitor_states && a.monitor_states == original, "partial violating monitor was committed");
      event.choice = 1;
      auto backwards = state(9, pages);
      require(detsim::plugins::check_transition(a, backwards, event), "edge violation lost");
      auto reloaded = b;
      require(!detsim::plugins::check_initial(reloaded), "loaded state violation");
      require(reloaded.monitor_states == b.monitor_states, "loaded monitor reinitialized");
      reloaded.ts_hash[0] = save_counter(-1, pages);
      require(detsim::plugins::check_initial(reloaded), "loaded state predicate skipped");
      require(reloaded.monitor_states == b.monitor_states, "loaded monitor changed on recheck");
      auto negative = state(-1, pages);
      require(detsim::plugins::check_initial(negative), "state violation lost");
      auto absent = state(0, pages); absent.ts_hash[0] = 0;
      rejects([&] { detsim::plugins::check_initial(absent); }, "missing data became pass");
      auto choice = detsim::plugins::choose_time(detsim::StateView(a), 0, 1, {10, 123}, 5);
      require(choice.value.seconds == 11 && choice.value.microseconds == 123, "typed time choice");
      rejects([&] { detsim::plugins::choose_time(detsim::StateView(a), 0, 2, {10, 0}, 5); }, "overspent budget");
      rejects([&] { detsim::plugins::choose_time(detsim::StateView(a), 0, 3, {10, 0}, 5); }, "invalid timeval");
      require(detsim::plugins::describe_bytes("MSG", 3) == "fixture message", "decoder");
      require(detsim::plugins::describe_bytes("raw", 3).empty(), "decoder fallback");
      detsim::plugins::reset(); detsim::plugins::load(argv[1], "{\"changed\":true}");
      rejects([&] { detsim::plugins::check_transition(a, b, event); }, "configuration mismatch accepted");
      detsim::plugins::reset();
    }
    detsim::plugins::reset(); detsim::plugins::load(argv[1], "{\"partial_initial\":true}");
    auto initial_partial = state(10, true);
    require(detsim::plugins::check_initial(initial_partial), "initial partial monitor violation lost");
    require(initial_partial.monitor_states.size() == 1 && initial_partial.monitor_states.begin()->second.empty(), "partial initial monitor was committed");
    initial_partial.property_failed = true;
    initial_partial.ts_hash[0] = 0; // terminal marks survive inspection, no callbacks
    require(detsim::plugins::check_initial(initial_partial) && initial_partial.property_failed,
      "terminal property failure was cleared");
    auto forbidden_next = state(11, true); detsim_event_v1 terminal_event{};
    rejects([&] { detsim::plugins::check_transition(initial_partial, forbidden_next, terminal_event); },
      "terminal property source accepted");
    // Presentation plugins cannot change semantic context or monitor identity.
    detsim::plugins::reset();
    const auto empty_context = detsim::plugins::context_key();
    detsim::plugins::load(argv[4]);
    require(detsim::plugins::context_key() == empty_context, "decoder changed context");
    auto display_only = state(7, true);
    require(!detsim::plugins::check_initial(display_only) && display_only.monitor_states.empty(), "decoder created monitor");
    require(detsim::plugins::describe_bytes("MSG", 3) == "fixture message", "pure decoder not called");
    require(detsim::plugins::describe_bytes("BAD", 3).empty(), "decoder partial error did not fall back");
    require(detsim::plugins::describe_bytes("THR", 3).empty(), "decoder exception did not fall back");
    require(detsim::plugins::describe_bytes("OVF", 3).empty(), "decoder writer failure did not fall back");
    require(detsim::plugins::context_key() == empty_context && display_only.monitor_states.empty(),
      "decoder failure changed semantic state");
    detsim::plugins::load(argv[1]);
    const auto semantic_context = detsim::plugins::context_key();
    auto with_display = state(8, true); detsim::plugins::check_initial(with_display);
    detsim::plugins::reset(); detsim::plugins::load(argv[1]);
    require(detsim::plugins::context_key() == semantic_context, "decoder removal changed context");
    auto without_display = state(8, true); detsim::plugins::check_initial(without_display);
    require(with_display.monitor_states == without_display.monitor_states, "decoder changed monitor bytes");
    detsim::plugins::reset();
    StateStore::instance().wait_for_completion(); StateStore::instance().shutdown();
    std::filesystem::remove_all(directory);
    std::puts("PASS plugin ABI, readonly flat/pages, monitor branch/replay, edge violations, failures, time-choice validation, decoder");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "plugin integration: %s\n", error.what());
    detsim::plugins::reset(); StateStore::instance().shutdown(); std::filesystem::remove_all(directory); return 1;
  }
}
