#include "sstate_reader.h"
#include <charconv>
#include <cstdio>
#include <string_view>

namespace detsim { namespace ui { class NCursesUI; } }
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void usage(const char *program) {
    printf("Usage: %s {search <ts_hash>|info <ss_hash>|verify|list} [sstate_dir]\n", program);
}
static hash_type parse_hash(std::string_view value) {
    if (value.starts_with("0x") || value.starts_with("0X")) value.remove_prefix(2);
    hash_type hash;
    auto result = std::from_chars(value.data(), value.data() + value.size(), hash, 16);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size())
        throw std::runtime_error("invalid hexadecimal hash");
    return hash;
}
int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") { usage(argv[0]); return 0; }
    try {
        const bool target_command = command == "search" || command == "info";
        if (!target_command && command != "verify" && command != "list")
            throw std::runtime_error("unknown command");
        const int next = target_command ? 3 : 2;
        if (argc < next || argc > next + 1) throw std::runtime_error("incorrect argument count");
        hash_type target = target_command ? parse_hash(argv[2]) : 0;
        std::string directory = argc > next ? argv[next] : "./sstate";
        size_t total = 0, matched = 0;
        SysStateStore::inspect(directory, [&](hash_type hash, const SysStateEntry &entry, const std::vector<uint8_t> &data) {
            auto state = decode_metadata(hash, data);
            ++total;
            bool match = !target_command || (command == "info" && hash == target);
            if (command == "search")
                for (hash_type child : state.ts_hash) if (child == target) match = true;
            if (!match) return;
            ++matched;
            if (command == "verify") return;
            printf("ss_hash=%016lX offset=%lu size=%u checksum=%08X\n", hash, entry.offset, entry.size, entry.checksum);
            if (command != "list") {
                for (int i = 0; i < NP; ++i)
                    printf("  pid[%d]: ts_hash=%016lX status=%d\n", i, state.ts_hash[i], state.status[i]);
                printf("  error_bound=%d\n", state.error_bound);
            }
        });
        printf("Verified %zu entries; matched %zu\n", total, matched);
        if (target_command && !matched) { puts("NOT FOUND"); return 1; }
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "Error: %s\n", error.what()); return 1;
    }
}
