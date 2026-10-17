#include "tools/sstate_reader.h"
#include "core/state/page_snapshot.h"
#include "core/state/state_store.h"
#include "proc_status.h"
#include <sys/stat.h>
#include <nlohmann/json.hpp>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace detsim { namespace ui { class NCursesUI; } }
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
using Json = nlohmann::json;
namespace fs = std::filesystem;
static std::string hex(hash_type value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}
static std::ofstream output(const fs::path& path) {
  if (fs::exists(path)) throw std::runtime_error("refusing to overwrite dump file: " + path.string());
  std::ofstream file(path, std::ios::binary);
  file.exceptions(std::ios::badbit | std::ios::failbit);
  return file;
}
static void write_bytes(const fs::path& path, const void* data, size_t size) {
  auto file = output(path);
  file.write(static_cast<const char*>(data), size);
  file.close();
}
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: dump <sys_state_hash_hex> <existing_output_dir>; cwd must be closed session");
    std::string_view text(argv[1]);
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    hash_type hash = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), hash, 16);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !hash)
      throw std::runtime_error("invalid nonzero hexadecimal sys-state hash");
    const fs::path directory(argv[2]);
    if (!fs::is_directory(directory)) throw std::runtime_error("output directory must already exist");
    std::optional<sys_state> state;
    SysStateStore::inspect("sstate", [&](hash_type key, const SysStateEntry&, const std::vector<uint8_t>& data) {
      if (key == hash) state = decode_metadata(key, data);
    });
    if (!state) throw std::runtime_error("sys-state hash not found");
    static_assert(NP == 1, "diagnostic requires NP=1");
    StateStore::Config config;
    config.read_only = true;
    config.packed_storage_path = "memory";
    config.hot_cache_size = 1024 * 1024;
    config.warm_cache_size = 1024 * 1024;
    config.prefetch_threads = 0;
    config.prefetch_window = 0;
    auto& store = StateStore::instance();
    store.init(config);
    const hash_type child = state->ts_hash[0];
    const auto view = SnapshotView::open(child);
    const tracee_state tracee(child);
    const auto metadata = view->metadata();
    write_bytes(directory / "tracee-metadata.bin", metadata.data(), metadata.size());
    const auto& registers = view->main_regs();
    write_bytes(directory / "main-registers.bin", &registers, sizeof(registers));
    const auto maps = view->maps_text();
    write_bytes(directory / "maps.txt", maps.data(), maps.size());
    Json summary{{"ss_hash", hex(hash)}, {"ts_hash", hex(child)},
      {"format", view->version() == PAGE_SNAPSHOT_VERSION ? "pages" : "full"},
      {"format_version", view->version()}, {"model_identity", state->model_identity},
      {"clock", {{"tv_sec", tracee.tv.tv_sec}, {"tv_usec", tracee.tv.tv_usec}}},
      {"regions", Json::array()}, {"registers", Json::object()}};
#define REG(name) summary["registers"][#name] = registers.name
    REG(r15); REG(r14); REG(r13); REG(r12); REG(rbp); REG(rbx);
    REG(r11); REG(r10); REG(r9); REG(r8); REG(rax); REG(rcx); REG(rdx);
    REG(rsi); REG(rdi); REG(orig_rax); REG(rip); REG(cs); REG(eflags);
    REG(rsp); REG(ss); REG(fs_base); REG(gs_base); REG(ds); REG(es); REG(fs); REG(gs);
#undef REG
    std::array<uint8_t, 128 * 1024> buffer;
    size_t index = 0;
    for (const auto& region : view->regions()) {
      const std::string name = "region-" + std::to_string(index++) + ".bin";
      auto file = output(directory / name);
      uint64_t address = region.start;
      while (address < region.end) {
        const size_t length = std::min<uint64_t>(buffer.size(), region.end - address);
        view->read_into(address, buffer.data(), length);
        file.write(reinterpret_cast<const char*>(buffer.data()), length);
        address += length;
      }
      file.close();
      summary["regions"].push_back({{"start", region.start}, {"end", region.end},
        {"prot", region.prot}, {"flags", std::string(region.flags, strnlen(region.flags, sizeof(region.flags)))},
        {"file", name}});
    }
    // Process exit closes descriptors, but preserves the captured stream node.
    // Read its saved bytes; absence is an error, never an assumed empty stream.
    std::string stdout_key(1, '\0');
    stdout_key += "stdio1";
    const auto node = tracee.fs_state.filesystem.find(stdout_key);
    if (node == tracee.fs_state.filesystem.end() || !S_ISCHR(node->second.metadata.st_mode))
      throw std::runtime_error("saved captured stdout node missing or invalid");
    const auto description = tracee.fs_state.open_files.find(1);
    if (tracee.fd_manager_state.is_allocated(1)) {
      if (tracee.fd_manager_state.get_fd_type(1) != FdType::STDIO ||
          description == tracee.fs_state.open_files.end() || description->second.path != stdout_key)
        throw std::runtime_error("saved stdout descriptor does not name captured stream");
    } else if (!DISEXITED(state->status[0]) || description != tracee.fs_state.open_files.end()) {
      throw std::runtime_error("saved stdout descriptor absent before process exit");
    }
    const auto& standard_output = node->second.content;
    if (node->second.metadata.st_size < 0 ||
        uint64_t(node->second.metadata.st_size) != standard_output.size())
      throw std::runtime_error("saved stdout size mismatch");
    write_bytes(directory / "stdout.bin", standard_output.data(), standard_output.size());
    summary["stdout_bytes"] = standard_output.size();
    summary["stdout_descriptor_open"] = tracee.fd_manager_state.is_allocated(1);
    summary["status"] = state->status[0];
    const std::string json = summary.dump(2) + "\n";
    write_bytes(directory / "summary.json", json.data(), json.size());
    store.shutdown();
    std::cout << "dumped " << index << " saved regions for " << hex(hash) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "snapshot dump: " << error.what() << '\n';
    return 1;
  }
}
