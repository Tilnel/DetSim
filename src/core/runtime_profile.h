#ifndef DETSIM_RUNTIME_PROFILE_H
#define DETSIM_RUNTIME_PROFILE_H

#include <nlohmann/json.hpp>
#include <set>
#include <vector>
#include <cstddef>
#include <string>

namespace detsim {
// The prelude runs outside exploration. Its endpoint and platform mutations are
// configuration, never inferred from the language or target's private structs.
struct RuntimeProfile {
  int startup_syscall;
  std::vector<int> startup_syscalls;
  size_t startup_syscall_limit;
  bool deterministic_at_random;
  bool disable_vdso;
  std::set<int> ignored_signals;
};
const RuntimeProfile& runtime_profile();
std::string runtime_profile_identity();
int startup_syscall_for_node(size_t node);
void configure_runtime_profile(const nlohmann::json& settings);
}
#endif
