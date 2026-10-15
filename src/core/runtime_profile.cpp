#include "runtime_profile.h"
#include <nlohmann/json.hpp>
#include <csignal>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/syscall.h>

namespace detsim {
static RuntimeProfile defaults() {
  return {SYS_munmap, {}, 100000, true, true, {}, 1000000};
}
static RuntimeProfile profile = defaults();
const RuntimeProfile& runtime_profile() { return profile; }
int startup_syscall_for_node(size_t node) {
  if (node >= NP) throw std::runtime_error("RuntimeProfile startup node index out of range");
  return profile.startup_syscalls.empty() ? profile.startup_syscall : profile.startup_syscalls.at(node);
}
std::string runtime_profile_identity() {
  nlohmann::json identity{
    {"profile", "linux-x86_64-syscall-prelude-v1"},
    {"kernel_model", "linux-x86_64-closed-syscalls-v20"},
    {"rseq", "unavailable-enosys"},
    {"altstack_min_size", 2048},
    {"initial_time_us", profile.initial_time_us},
    {"startup_syscall", profile.startup_syscall},
    {"startup_syscalls", profile.startup_syscalls},
    {"startup_syscall_limit", profile.startup_syscall_limit},
    {"deterministic_at_random", profile.deterministic_at_random},
    {"disable_vdso", profile.disable_vdso},
    {"ignored_signals", profile.ignored_signals}};
  return identity.dump();
}
void configure_runtime_profile(const nlohmann::json& settings) {
  if (!settings.is_object()) throw std::runtime_error("RuntimeProfile must be an object");
  static const std::set<std::string> keys{
    "startup_syscall", "startup_syscalls", "startup_syscall_limit", "deterministic_at_random",
    "disable_vdso", "ignored_signals", "initial_time_us"};
  for (const auto& item : settings.items())
    if (!keys.count(item.key())) throw std::runtime_error("Unknown RuntimeProfile key: " + item.key());
  for (const char* key : {"startup_syscall", "startup_syscall_limit"})
    if (settings.contains(key) && !settings[key].is_number_integer())
      throw std::runtime_error(std::string("RuntimeProfile ") + key + " must be an integer");
  for (const char* key : {"deterministic_at_random", "disable_vdso"})
    if (settings.contains(key) && !settings[key].is_boolean())
      throw std::runtime_error(std::string("RuntimeProfile ") + key + " must be a boolean");
  auto syscall_number = [](const nlohmann::json& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() ? value.get<uint64_t>() >= 450 :
         value.get<int64_t>() < 0 || value.get<int64_t>() >= 450))
      throw std::runtime_error("RuntimeProfile startup syscall must be an integer in [0, 450)");
    return value.get<int>();
  };
  RuntimeProfile next = defaults();
  if (settings.contains("initial_time_us")) {
    const auto& value = settings.at("initial_time_us");
    constexpr uint64_t maximum = std::numeric_limits<int64_t>::max() / 1000;
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() ? value.get<uint64_t>() > maximum :
         value.get<int64_t>() < 0 || static_cast<uint64_t>(value.get<int64_t>()) > maximum))
      throw std::runtime_error("RuntimeProfile initial_time_us exceeds modeled ktime range");
    next.initial_time_us = value.get<int64_t>();
  }
  if (settings.contains("startup_syscall"))
    next.startup_syscall = syscall_number(settings["startup_syscall"]);
  auto limit = settings.value("startup_syscall_limit", int64_t(next.startup_syscall_limit));
  if (next.startup_syscall < 0 || next.startup_syscall >= 450 || limit <= 0 || limit > 10000000)
    throw std::runtime_error("Invalid RuntimeProfile startup syscall or syscall limit");
  next.startup_syscall_limit = size_t(limit);
  if (settings.contains("startup_syscalls")) {
    const auto& per_node = settings["startup_syscalls"];
    if (!per_node.is_array() || per_node.size() != NP)
      throw std::runtime_error("RuntimeProfile startup_syscalls must contain exactly " +
                               std::to_string(NP) + " syscall numbers (one per node)");
    for (const auto& value : per_node) {
      next.startup_syscalls.push_back(syscall_number(value));
    }
  }
  next.deterministic_at_random = settings.value("deterministic_at_random", true);
  next.disable_vdso = settings.value("disable_vdso", true);
  if (settings.contains("ignored_signals")) {
    if (!settings["ignored_signals"].is_array()) throw std::runtime_error("RuntimeProfile ignored_signals must be an array");
    for (const auto& item : settings["ignored_signals"]) {
      if (!item.is_number_integer())
        throw std::runtime_error("RuntimeProfile signal number must be an integer");
      if (item.is_number_unsigned() ? item.get<uint64_t>() >= NSIG :
          item.get<int64_t>() <= 0 || item.get<int64_t>() >= NSIG)
        throw std::runtime_error("RuntimeProfile signal number out of range");
      int signal = item.get<int>();
      if (signal <= 0 || signal >= NSIG || signal == SIGKILL || signal == SIGSTOP ||
          signal == SIGTRAP || signal == SIGSEGV || signal == SIGABRT ||
          signal == SIGILL || signal == SIGFPE || signal == SIGBUS)
        throw std::runtime_error("RuntimeProfile cannot ignore reserved or fatal signal " + std::to_string(signal));
      next.ignored_signals.insert(signal);
    }
  }
  profile = std::move(next);
}
}
