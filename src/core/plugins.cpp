#include "plugins.h"
#include "state/state.h"
#include "state/state_view.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <xxhash.h>

struct detsim_state_view { const detsim::StateView *view; };
namespace detsim::plugins {
namespace {
constexpr size_t max_blob = 64 * 1024 * 1024;
struct Module {
  void *handle;
  const detsim_plugin_v2 *api;
  std::string configuration, key;
  ~Module() { dlclose(handle); }
};
std::vector<std::unique_ptr<Module>> modules;
size_t process_count(const detsim_state_view *v) { return v->view->process_count(); }
int read(const detsim_state_view *v, uint32_t n, uint64_t a, void *p, size_t l) {
  try { v->view->read(n, a, p, l); return 0; } catch (...) { return -1; }
}
int evaluate(const detsim_state_view *v, uint32_t n, const char *e, int64_t *p) {
  if (!e || !p) return -1;
  try { *p = v->view->evaluate(n, e); return 0; } catch (...) { return -1; }
}
int get_time(const detsim_state_view *v, uint32_t n, detsim_time *p) {
  if (!p) return -1;
  try { auto t = v->view->time(n); *p = {t.tv_sec, static_cast<int32_t>(t.tv_usec)}; return 0; }
  catch (...) { return -1; }
}
int inspect_json(const detsim_state_view* v, uint32_t node, detsim_writer writer) {
  if (!writer.append) return -1;
  try { const auto json=v->view->inspect_json(node); return writer.append(writer.context,json.data(),json.size()) ? -1 : 0; }
  catch (...) { return -1; }
}
int read_buffer(const detsim_state_view* v, uint32_t node, const detsim_buffer_selector* selector,
                void* output, size_t length, size_t* copied) {
  if (!selector || !copied) return -1;
  *copied=0;
  try {
    *copied=v->view->read_buffer(node,static_cast<detsim_buffer_kind>(selector->kind),selector->fd,
      selector->item,selector->message,selector->offset,output,length);
    return 0;
  } catch (...) { return -1; }
}
const detsim_host_v2 host{DETSIM_PLUGIN_ABI_V2, process_count, read, evaluate, get_time, nullptr, inspect_json, read_buffer};
struct Output {
  std::vector<uint8_t> bytes;
  bool failed = false;
  static int append(void *context, const void *p, size_t size) {
    auto &out = *static_cast<Output *>(context);
    if ((size && !p) || size > max_blob - out.bytes.size()) { out.failed = true; return -1; }
    try {
      if (size) out.bytes.insert(out.bytes.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + size);
      return 0;
    } catch (...) { out.failed = true; return -1; }
  }
  detsim_writer writer() { return {this, append}; }
};
struct Diagnostic {
  std::array<char, 1024> bytes{};
  detsim_diagnostic argument() { return {bytes.data(), bytes.size()}; }
};
bool require_pass(const Module &m, detsim_verdict v, const Diagnostic &d) {
  if (v == DETSIM_PASS) return false;
  if (v == DETSIM_VIOLATION) {
    std::cerr << m.api->id << ": property violation: " << std::string(d.bytes.data(), strnlen(d.bytes.data(), d.bytes.size())) << "\n";
    return true;
  }
  const char *kind = v == DETSIM_VIOLATION ? "property violation" : v == DETSIM_UNAVAILABLE ? "inspection unavailable" : "plugin error";
  throw std::runtime_error(m.api->id + std::string(": ") + kind + ": " + std::string(d.bytes.data(), strnlen(d.bytes.data(), d.bytes.size())));
}
bool semantic(const Module &m) {
  return m.api->check_state || m.api->check_transition || m.api->init_monitor || m.api->choose_time;
}
void validate_context(const sys_state &state) {
  const size_t count = std::count_if(modules.begin(), modules.end(), [](const auto &m) { return semantic(*m); });
  if (state.monitor_states.size() != count)
    throw std::runtime_error("snapshot plugin context differs from loaded plugins");
  for (const auto &m : modules)
    if (semantic(*m) && !state.monitor_states.count(m->key))
      throw std::runtime_error("snapshot plugin binary/configuration/schema mismatch: " + std::string(m->api->id));
}
std::string fingerprint(const std::string &path, const std::string &configuration) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read plugin: " + path);
  XXH64_state_t *raw = XXH64_createState();
  if (!raw) throw std::bad_alloc();
  std::unique_ptr<XXH64_state_t, decltype(&XXH64_freeState)> hash(raw, XXH64_freeState);
  XXH64_reset(hash.get(), 0);
  std::array<char, 16384> buffer;
  while (input) {
    input.read(buffer.data(), buffer.size());
    XXH64_update(hash.get(), buffer.data(), static_cast<size_t>(input.gcount()));
  }
  if (!input.eof()) throw std::runtime_error("cannot read plugin: " + path);
  /* Delimit binary and configuration; neither path nor host process ID is semantic. */
  auto binary_hash = XXH64_digest(hash.get());
  XXH64_reset(hash.get(), binary_hash);
  XXH64_update(hash.get(), configuration.data(), configuration.size());
  std::ostringstream out;
  out << std::hex << XXH64_digest(hash.get());
  return out.str();
}
}
void load(const std::string &path, const std::string &configuration) {
  if (path.size() < 3 || path.substr(path.size() - 3) != ".so")
    throw std::runtime_error("plugin must be an explicitly built .so exporting detsim_plugin_entry_v2; compile-on-load/legacy check() is unsupported: " + path);
  auto hash = fingerprint(path, configuration);
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) throw std::runtime_error("cannot load plugin " + path + ": " + dlerror());
  auto close_handle = [](void *p) { dlclose(p); };
  auto close_on_error = std::unique_ptr<void, decltype(close_handle)>(handle, close_handle);
  dlerror();
  auto entry = reinterpret_cast<detsim_plugin_entry_v2_fn>(dlsym(handle, "detsim_plugin_entry_v2"));
  const char *error = dlerror();
  if (error || !entry) throw std::runtime_error("plugin lacks detsim_plugin_entry_v2 (legacy plugins must be rebuilt): " + path);
  const auto *api = entry();
  if (!api || api->abi_version != DETSIM_PLUGIN_ABI_V2 || api->struct_size != sizeof(detsim_plugin_v2) || !api->id || !*api->id || strlen(api->id) > 128)
    throw std::runtime_error("invalid plugin ABI/descriptor: " + path);
  if (bool(api->init_monitor) != bool(api->advance_monitor))
    throw std::runtime_error("plugin requires both init_monitor and advance_monitor: " + path);
  if (!api->check_state && !api->check_transition && !api->init_monitor && !api->choose_time && !api->describe_bytes)
    throw std::runtime_error("plugin has no callbacks: " + path);
  std::string key = std::string(api->id) + ":" + std::to_string(api->monitor_schema) + ":" + hash;
  for (const auto &m : modules) {
    if (m->key == key) return;
    if (std::strcmp(m->api->id, api->id) == 0)
      throw std::runtime_error("duplicate plugin ID with differing binary/configuration: " + std::string(api->id));
    if (m->api->choose_time && api->choose_time)
      throw std::runtime_error("only one time-choice plugin may be loaded");
  }
  auto module = std::make_unique<Module>();
  module->handle = close_on_error.release(); module->api = api;
  module->configuration = configuration; module->key = std::move(key);
  modules.push_back(std::move(module));
}
void reset() { modules.clear(); }
bool has_time_choice() {
  return std::any_of(modules.begin(), modules.end(), [](const auto &m) { return m->api->choose_time; });
}
std::string context_key() {
  std::string out;
  for (const auto &m : modules) if (semantic(*m)) out += std::to_string(m->key.size()) + ":" + m->key;
  return out;
}
bool check_initial(sys_state &state) {
  if (state.property_failed) return true;
  bool violation = false;
  const bool initialized = !state.monitor_states.empty();
  if (initialized) validate_context(state);
  if (modules.empty()) return false;
  StateView view(state); detsim_state_view v{&view};
  decltype(state.monitor_states) next;
  for (const auto &m : modules) {
    if (!semantic(*m)) continue;
    auto module_host = host; module_host.configuration = m->configuration.c_str();
    Diagnostic d;
    if (m->api->check_state) violation |= require_pass(*m, m->api->check_state(&module_host, &v, d.argument()), d);
    if (initialized) continue; // predicates rerun; history remains exactly as loaded
    Output out;
    if (m->api->init_monitor) {
      const auto verdict = m->api->init_monitor(&module_host, &v, m->configuration.c_str(), out.writer(), d.argument());
      violation |= require_pass(*m, verdict, d);
      if (verdict == DETSIM_VIOLATION) { out.bytes.clear(); out.failed = false; }
    }
    if (out.failed) throw std::runtime_error("plugin monitor output exceeds limit: " + std::string(m->api->id));
    next.emplace(m->key, std::move(out.bytes));
  }
  if (!initialized) state.monitor_states = std::move(next);
  return violation;
}
bool check_transition(const sys_state &before, sys_state &after, const detsim_event_v2 &event) {
  if (before.property_failed) throw std::runtime_error("cannot advance a terminal property failure");
  bool violation = false;
  validate_context(before);
  if (modules.empty()) { after.monitor_states.clear(); return false; }
  StateView a(before), b(after); detsim_state_view va{&a}, vb{&b};
  decltype(after.monitor_states) next;
  for (const auto &m : modules) {
    if (!semantic(*m)) continue;
    auto module_host = host; module_host.configuration = m->configuration.c_str();
    Diagnostic d;
    if (m->api->check_state) violation |= require_pass(*m, m->api->check_state(&module_host, &vb, d.argument()), d);
    if (m->api->check_transition) violation |= require_pass(*m, m->api->check_transition(&module_host, &va, &vb, &event, d.argument()), d);
    Output out;
    if (m->api->advance_monitor) {
      const auto &bytes = before.monitor_states.at(m->key);
      const auto verdict = m->api->advance_monitor(&module_host, &va, &vb, &event, {bytes.data(), bytes.size()}, out.writer(), d.argument());
      violation |= require_pass(*m, verdict, d);
      if (verdict == DETSIM_VIOLATION) { out.bytes = bytes; out.failed = false; }
    }
    if (out.failed) throw std::runtime_error("plugin monitor output exceeds limit: " + std::string(m->api->id));
    next.emplace(m->key, std::move(out.bytes));
  }
  after.monitor_states = std::move(next);
  return violation;
}
detsim_time_choice_v2 choose_time(const StateView &view, unsigned node, int choice, timeval now, unsigned budget) {
  if (node >= view.process_count() || choice < 0) throw std::runtime_error("invalid time-choice process/choice");
  detsim_state_view v{&view};
  for (const auto &m : modules) if (m->api->choose_time) {
    auto module_host = host; module_host.configuration = m->configuration.c_str();
    Diagnostic d; detsim_time_choice_v2 result{};
    if (require_pass(*m, m->api->choose_time(&module_host, &v, node, choice, {now.tv_sec, static_cast<int32_t>(now.tv_usec)}, budget, &result, d.argument()), d))
      throw std::runtime_error("time-choice policy returned a property violation");
    if (result.value.seconds < now.tv_sec || result.value.microseconds < 0 || result.value.microseconds >= 1000000 ||
        (result.value.seconds == now.tv_sec && result.value.microseconds < now.tv_usec) || result.budget_cost > budget ||
        result.value.seconds > std::numeric_limits<time_t>::max())
      throw std::runtime_error("invalid time-choice value/cost from " + std::string(m->api->id));
    return result;
  }
  throw std::runtime_error("gettimeofday choice requested without a time-choice plugin");
}
std::string describe_bytes(const void *data, size_t length) {
  for (const auto &m : modules) if (m->api->describe_bytes) {
    try {
      Output out;
      int result = m->api->describe_bytes({static_cast<const uint8_t *>(data), length}, out.writer());
      if (result == 1 && !out.failed) return std::string(out.bytes.begin(), out.bytes.end());
      if (result != 0 || out.failed)
        std::cerr << m->api->id << ": decoder failed; using raw display\n";
    } catch (const std::exception &error) {
      std::cerr << m->api->id << ": decoder exception; using raw display: " << error.what() << "\n";
    } catch (...) {
      std::cerr << m->api->id << ": decoder exception; using raw display\n";
    }
  }
  return {};
}
}
