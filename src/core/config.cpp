/*
 * config.cpp - Configuration parsing implementation using modern C++
 * (nlohmann/json)
 */

#include "config.h"
#include "common.h"
#include "debug.h"
#include "emu.h"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "state_store.h"
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <libgen.h>
#include <limits.h>
#include <nlohmann/json.hpp>
#include <wait.h>

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
    Assert(argv_json.is_array() && argv_json.size() <= 5,
           "Tracee %d: invalid arguments (max 5 supported)", i);

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

/* Compile .cpp to .so if needed, return path to .so */
static std::string compile_plugin_if_needed(const std::string &src_path,
                                            const std::string &default_name)
{
  // If already a .so file, use it directly
  if (src_path.size() > 3 && src_path.substr(src_path.size() - 3) == ".so")
  {
    LOG_INFO("Loading plugin %s", src_path.c_str());
    return src_path;
  }

  // Compile .cpp to .so
  fs::path src(src_path);
  std::string obj_name = "./" + src.stem().string() + ".so";

  LOG_INFO("Compiling %s -> %s", src_path.c_str(), obj_name.c_str());

  if (access(obj_name.c_str(), R_OK | X_OK) != 0)
  {
    int pid = vfork();
    if (pid == 0)
    {
      char arg_d[16];
      snprintf(arg_d, sizeof(arg_d), "-DNP=%d", NP);
      execlp("g++", "g++", "-O2", arg_d, "-fpic", "-shared", "-I.", "-Isrc",
             "-Isrc/core", "-Isrc/utils", "-Isrc/subsys", src_path.c_str(),
             "-o", obj_name.c_str(), nullptr);
      perror("exec");
      _exit(1);
    }
    else
    {
      waitpid(pid, nullptr, 0);
    }
  }

  return obj_name;
}

/* Parse and load user check plugin */
static void parse_user_check(const json &cfg, const fs::path &cfg_dir)
{
  if (!cfg.contains("UserCheck"))
    return;

  std::string src = cfg["UserCheck"].get<std::string>();
  std::string src_path = resolve_path(src, cfg_dir);
  std::string obj_path = compile_plugin_if_needed(src_path, "check");

  void *handle = dlopen(obj_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!handle)
  {
    panic("%s", dlerror());
  }

  auto *func = reinterpret_cast<int (*)()>(dlsym(handle, "check"));
  ptmc_state.user_checks.push_back(func);
  LOG_INFO("User check plugin loaded");
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
      choose_many[nr] = cp["choose"].get<int>();
    }
  }
}

/* Parse and load choose function plugin */
static void parse_choose_function(const json &cfg, const fs::path &cfg_dir)
{
  if (!cfg.contains("ChooseFunc"))
    return;

  std::string src = cfg["ChooseFunc"].get<std::string>();
  std::string src_path = resolve_path(src, cfg_dir);
  std::string obj_path = compile_plugin_if_needed(src_path, "choose");

  void *handle = dlopen(obj_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!handle)
  {
    panic("%s", dlerror());
  }

  for (int nr = 0; nr < 450; nr++)
  {
    if (syscalls[nr] == nullptr)
      continue;

    char funcname[64];
    snprintf(funcname, sizeof(funcname), "choose_%s", syscalls[nr]);
    auto *func = reinterpret_cast<choose_func>(dlsym(handle, funcname));
    choose_syswhat[nr] = func;
  }

  LOG_INFO("Choose function plugin loaded");
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
  if (ss.contains("enable_malloc_trim"))
  {
    ss_config.enable_malloc_trim = ss["enable_malloc_trim"].get<bool>();
  }
  if (ss.contains("malloc_trim_threshold_mb"))
  {
    ss_config.malloc_trim_threshold =
        ss["malloc_trim_threshold_mb"].get<size_t>() * 1024 * 1024;
  }
  if (ss.contains("packed_storage_path"))
  {
    ss_config.packed_storage_path =
        ss["packed_storage_path"].get<std::string>();
  }

  StateStore::instance().init(ss_config);
  LOG_INFO(
      "StateStore configured: hot=%zuMB, warm=%zuMB, prefetch=%zu, trim=%s",
      ss_config.hot_cache_size / (1024 * 1024),
      ss_config.warm_cache_size / (1024 * 1024), ss_config.prefetch_window,
      ss_config.enable_malloc_trim ? "on" : "off");
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
  parse_user_check(cfg, cfg_dir);
  parse_choose_points(cfg);
  parse_choose_function(cfg, cfg_dir);
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
    ptmc_state.tracee[i].argc = 0;
    ptmc_state.tracee[i].executable = nullptr;
  }
}
