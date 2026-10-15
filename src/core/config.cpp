/*
 * config.cpp - Configuration parsing implementation using modern C++
 * (nlohmann/json)
 */

#include "config.h"
#include "common.h"
#include "debug.h"
#include "net/emu.h"
#include "fs/fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "state/state_store.h"
#include "plugins.h"
#include "runtime_profile.h"
#include <filesystem>
#include <limits>
#include <new>
#include <fstream>
#include <array>
#include <algorithm>
#include <memory>
#include <xxhash.h>
#include <getopt.h>
#include <iostream>
#include <libgen.h>
#include <limits.h>
#include <nlohmann/json.hpp>
#include <sys/syscall.h>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

char *cfg_file = (char *)"config.json";
char *log_file = NULL;
int loglevel = 0;
FILE *log_fp = NULL;
int auto_mode = 0;
int use_ui = 1; // Default to using UI
char *batch_file = NULL;

bool should_log(int level) { return level >= loglevel; }

void init_log(const char *log_file)
{
  if (log_file == nullptr)
  {
    log_fp = stdout;
    return;
  }
  log_fp = fopen(log_file, "w");
  Assert(log_fp, "Can not open '%s'", log_file);
}

/* Get absolute path relative to config file directory */
static std::string resolve_path(const std::string &path,
                                const fs::path &cfg_dir)
{
  if (path.empty() || path[0] == '/')
  {
    return path;
  }
  return (cfg_dir / path).string();
}

/* Load JSON config file */
static json load_json_file(const char *cfg_file)
{
  std::ifstream f(cfg_file);
  Assert(f.good(), "Cannot open config file: %s", cfg_file);

  try
  {
    json cfg;
    f >> cfg;
    return cfg;
  }
  catch (const json::exception &e)
  {
    panic("JSON parse error in %s: %s", cfg_file, e.what());
    return {}; // unreachable
  }
}

/* Parse tracee configuration */
static void parse_tracees(const json &cfg, const fs::path &cfg_dir)
{
  auto tracees = cfg.value("Tracee", json::array());
  int nodes = cfg.value("Nodes", 0);

  Assert(nodes == NP,
         "Build doesn't support %d processes, please compile with 'NP=%d' and "
         "rebuild",
         nodes, nodes);

  Assert(static_cast<int>(tracees.size()) >= nodes,
         "Tracee array size (%zu) less than Nodes (%d)", tracees.size(), nodes);

  for (int i = 0; i < nodes; i++)
  {
    auto argv_json = tracees[i];
    if (!argv_json.is_array() || argv_json.empty() ||
        argv_json.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      throw std::runtime_error("Tracee " + std::to_string(i) + ": invalid arguments");

    ptmc_state.tracee[i].argv.assign(argv_json.size() + 1, nullptr);

    ptmc_state.tracee[i].argc = static_cast<int>(argv_json.size());

    for (size_t j = 0; j < argv_json.size(); j++)
    {
      std::string arg_str = argv_json[j].get<std::string>();

      // First argument (executable) is resolved relative to config dir
      if (j == 0)
      {
        arg_str = resolve_path(arg_str, cfg_dir);
      }

      // Store the string persistently
      char *stored = static_cast<char *>(malloc(arg_str.length() + 1));
      if (!stored) throw std::bad_alloc();
      strcpy(stored, arg_str.c_str());
      ptmc_state.tracee[i].argv[j] = stored;
    }

    ptmc_state.tracee[i].executable = ptmc_state.tracee[i].argv[0];
    ptmc_state.tracee[i].argv[ptmc_state.tracee[i].argc] = nullptr;
  }
}

/* Parse network addresses */
static void parse_addrs(const json &cfg)
{
  auto addrs = cfg.value("Addr", json::array());
  if (addrs.empty())
    return;

  Assert(static_cast<int>(addrs.size()) == NP,
         "Addr array size (%zu) doesn't match NP (%d)", addrs.size(), NP);

  for (int i = 0; i < NP; i++)
  {
    ptmc_state.addrs[i] = addrs[i].get<std::string>();
  }
}

/* Parse filesystem mappings */
static void parse_fsmap(const json &cfg, const fs::path &cfg_dir)
{
  std::string workdir = cfg.value("WorkingDir", "/");
  auto fsmap = cfg.value("FSMap", json::array());

  std::vector<std::pair<std::string, std::string>> mappings;
  for (const auto &item : fsmap)
  {
    if (item.contains("Host") && item.contains("Target"))
    {
      std::string host_path =
          resolve_path(item["Host"].get<std::string>(), cfg_dir);
      std::string target_path = item["Target"].get<std::string>();
      mappings.emplace_back(host_path, target_path);
    }
  }

  // Initialize per-node filesystem state
  for (int i = 0; i < NP; i++)
  {
    if (!mappings.empty())
    {
      ptmc_state.fs_states[i].init_from_mappings(mappings);
    }
    ptmc_state.fs_states[i].set_cwd(workdir);
  }
}

/* Parse assertions */
static void parse_assertions(const json &cfg)
{
  auto assertions = cfg.value("Assertions", json::array());
  LOG_DEBUG("Loading %zu assertions", assertions.size());

  for (const auto &assertion : assertions)
  {
    std::string assert_str = assertion.get<std::string>();
    LOG_DEBUG("  Assertion: %s", assert_str.c_str());
    ptmc_state.assertions.emplace(assert_str);
  }
}

/* Plugins are built explicitly and expose only the versioned public C ABI. */
static void parse_plugins(const json &cfg, const fs::path &cfg_dir)
{
  detsim::plugins::reset();
  for (const auto &entry : cfg.value("Plugins", json::array()))
  {
    std::string path = entry.is_string() ? entry.get<std::string>() : entry.at("Path").get<std::string>();
    std::string configuration = entry.is_object() ? entry.value("Configuration", json::object()).dump() : "{}";
    detsim::plugins::load(resolve_path(path, cfg_dir), configuration);
  }
  // Transitional configuration keys use the same ABI and never compile source.
  for (const char *key : {"UserCheck", "ChooseFunc"})
    if (cfg.contains(key)) detsim::plugins::load(resolve_path(cfg.at(key).get<std::string>(), cfg_dir));
}

/* Parse choose points */
static void parse_choose_points(const json &cfg)
{
  auto choose_points = cfg.value("ChoosePoint", json::array());

  for (const auto &cp : choose_points)
  {
    int nr = -1;

    if (cp.contains("syscall"))
    {
      if (cp["syscall"].is_number())
      {
        nr = cp["syscall"].get<int>();
      }
      else if (cp["syscall"].is_string())
      {
        std::string syscall_name = cp["syscall"].get<std::string>();
        for (int i = 0; i < 450; i++)
        {
          if (syscalls[i] && strcmp(syscalls[i], syscall_name.c_str()) == 0)
          {
            nr = i;
            break;
          }
        }
        if (nr == -1)
        {
          LOG_ERROR("Unknown syscall name: %s", syscall_name.c_str());
          continue;
        }
      }
    }

    if (nr >= 0 && cp.contains("choose"))
    {
      int count = cp["choose"].get<int>();
      if (nr != SYS_gettimeofday || count < 0 || count > 1024)
        throw std::runtime_error("ChoosePoint supports only gettimeofday with 0..1024 choices");
      if (count && !detsim::plugins::has_time_choice())
        throw std::runtime_error("ChoosePoint requires a time-choice plugin");
      choose_many[nr] = count;
    }
  }
}

/* Parse StateStore configuration */
static void parse_statestore_config(const json &cfg)
{
  if (!cfg.contains("StateStore"))
    return;

  auto ss = cfg["StateStore"];
  StateStore::Config ss_config;

  if (ss.contains("hot_cache_mb"))
  {
    ss_config.hot_cache_size = ss["hot_cache_mb"].get<size_t>() * 1024 * 1024;
  }
  if (ss.contains("warm_cache_mb"))
  {
    ss_config.warm_cache_size = ss["warm_cache_mb"].get<size_t>() * 1024 * 1024;
  }
  if (ss.contains("prefetch_window"))
  {
    ss_config.prefetch_window = ss["prefetch_window"].get<size_t>();
  }
  if (ss.contains("prefetch_threads"))
  {
    ss_config.prefetch_threads = ss["prefetch_threads"].get<size_t>();
  }
  if (ss.contains("compression_level"))
  {
    ss_config.compression_level = ss["compression_level"].get<int>();
  }
  if (ss.contains("enable_malloc_trim") || ss.contains("malloc_trim_threshold_mb"))
    LOG_WARN("StateStore malloc-trim options are obsolete and ignored");
  if (ss.contains("max_pending_bytes"))
    ss_config.max_pending_bytes = ss["max_pending_bytes"].get<size_t>();
  if (ss.contains("packed_storage_path"))
  {
    ss_config.packed_storage_path =
        ss["packed_storage_path"].get<std::string>();
  }

  StateStore::instance().init(ss_config);
  LOG_INFO(
      "StateStore configured: hot=%zuMB, warm=%zuMB, prefetch=%zu, pending_limit=%zu bytes",
      ss_config.hot_cache_size / (1024 * 1024),
      ss_config.warm_cache_size / (1024 * 1024), ss_config.prefetch_window,
      ss_config.max_pending_bytes);
}

void read_config(const char *cfg_file)
{
  assert(cfg_file);

  // Get config file directory
  fs::path cfg_dir = fs::path(cfg_file).parent_path();

  // Load and parse JSON
  json cfg = load_json_file(cfg_file);

  // Parse basic configuration
  loglevel = cfg.value("Loglevel", 0);

  // Parse all sections
  parse_tracees(cfg, cfg_dir);
  parse_addrs(cfg);
  parse_fsmap(cfg, cfg_dir);
  parse_assertions(cfg);
  detsim::configure_runtime_profile(cfg.value("RuntimeProfile", json::object()));
  parse_plugins(cfg, cfg_dir);
  parse_choose_points(cfg);

  // Search compatibility includes semantic configuration, not cache sizes or UI.
  // Resolve paths and fingerprint each executable once, outside exploration.
  json model = {{"Nodes", NP}, {"WorkingDir", cfg.value("WorkingDir", "/")}};
  std::map<std::string, uint64_t> binaries;
  model["Tracee"] = json::array();
  for (int node = 0; node < NP; ++node)
  {
    json arguments = json::array();
    for (int arg = 0; arg < ptmc_state.tracee[node].argc; ++arg)
      arguments.push_back(arg ? std::string(ptmc_state.tracee[node].argv[arg]) :
        fs::absolute(ptmc_state.tracee[node].argv[arg]).lexically_normal().string());
    std::string executable = arguments.at(0).get<std::string>();
    if (!binaries.count(executable))
    {
      std::ifstream binary(executable, std::ios::binary);
      if (!binary) throw std::runtime_error("Cannot fingerprint executable: " + executable);
      auto raw = XXH64_createState();
      if (!raw) throw std::bad_alloc();
      std::unique_ptr<XXH64_state_t, decltype(&XXH64_freeState)> hash(raw, XXH64_freeState);
      XXH64_reset(hash.get(), 0);
      std::array<char, 65536> buffer;
      while (binary)
      {
        binary.read(buffer.data(), buffer.size());
        XXH64_update(hash.get(), buffer.data(), static_cast<size_t>(binary.gcount()));
      }
      if (!binary.eof()) throw std::runtime_error("Cannot fingerprint executable: " + executable);
      binaries.emplace(executable, XXH64_digest(hash.get()));
    }
    model["Tracee"].push_back({{"argv", arguments}, {"binary", binaries.at(executable)}});
  }
  model["Addr"] = json::array();
  for (int node = 0; node < NP; ++node) model["Addr"].push_back(ptmc_state.addrs[node]);
  model["FSMap"] = cfg.value("FSMap", json::array());
  for (auto &mapping : model["FSMap"])
    if (mapping.contains("Host")) mapping["Host"] = fs::absolute(resolve_path(mapping.at("Host").get<std::string>(), cfg_dir)).lexically_normal().string();
  model["Choices"] = json::object();
  for (int nr = 0; nr < 450; ++nr)
    if (choose_many[nr]) model["Choices"][std::to_string(nr)] = choose_many[nr];
  std::vector<std::string> assertions(ptmc_state.assertions.begin(), ptmc_state.assertions.end());
  std::sort(assertions.begin(), assertions.end());
  model["Assertions"] = assertions;
  model["plugins"] = detsim::plugins::context_key();
  model["runtime"] = detsim::runtime_profile_identity();
  const auto canonical = model.dump();
  char identity[40];
  snprintf(identity, sizeof(identity), "detsim-model-v1:%016llx",
    static_cast<unsigned long long>(XXH64(canonical.data(), canonical.size(), 0)));
  ptmc_state.model_identity = identity;
  parse_statestore_config(cfg);
}

void parse_args(int argc, char *argv[])
{
  const struct option table[] = {
      {"auto", no_argument, nullptr, 'a'},
      {"no-ui", no_argument, nullptr, 'n'},
      {"batch", required_argument, nullptr, 'b'},
      {"log", required_argument, nullptr, 'l'},
      {"help", no_argument, nullptr, 'h'},
      {"cfg", required_argument, nullptr, 'c'},
      {0, 0, nullptr, 0},
  };

  int o;
  while ((o = getopt_long(argc, argv, "-ahnl:c:b:", table, nullptr)) != -1)
  {
    switch (o)
    {
      case 'a':
        auto_mode = 1;
        break;
      case 'n':
        use_ui = 0;
        break;
      case 'b':
        batch_file = optarg;
        break;
      case 'l':
        log_file = optarg;
        break;
      case 'c':
        cfg_file = optarg;
        break;
      default:
        printf("Usage: %s [OPTION...]\n\n", argv[0]);
        printf("\t-a,--auto               run with auto mode\n");
        printf("\t-n,--no-ui              run without TUI (CLI mode)\n");
        printf("\t-b,--batch=FILE         run commands from script file\n");
        printf("\t-l,--log=FILE           output log to FILE\n");
        printf("\t-c,--cfg=FILE           specify configuration FILE\n");
        printf("\n");
        exit(0);
    }
  }
}

int is_auto_mode() { return auto_mode; }

void cleanup_config()
{
  detsim::plugins::reset();
  for (int i = 0; i < NP; i++)
  {
    for (int j = 0; j < ptmc_state.tracee[i].argc; j++)
    {
      if (ptmc_state.tracee[i].argv[j])
      {
        free(ptmc_state.tracee[i].argv[j]);
        ptmc_state.tracee[i].argv[j] = nullptr;
      }
    }
    ptmc_state.tracee[i].argv.clear();
    ptmc_state.tracee[i].argc = 0;
    ptmc_state.tracee[i].executable = nullptr;
  }
}
