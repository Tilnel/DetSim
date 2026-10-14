#include "sstate_reader.h"
#include <cstdio>
#include <string>

namespace detsim { namespace ui { class NCursesUI; } }
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

int main(int argc, char **argv) {
    std::string directory = "./sstate";
    bool verbose = false, directory_set = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "-v" || argument == "--verbose") verbose = true;
            else if (argument == "-h" || argument == "--help") {
                printf("Usage: %s [-v|--verbose] [sstate_dir]\n", argv[0]); return 0;
            } else if (!argument.empty() && argument[0] != '-' && !directory_set) {
                directory = argument; directory_set = true;
            } else throw std::runtime_error("invalid argument");
        }
        size_t count = 0;
        SysStateStore::inspect(directory, [&](hash_type hash, const SysStateEntry &, const std::vector<uint8_t> &data) {
            auto state = decode_metadata(hash, data);
            if (verbose) printf("%016lX error_bound=%d OK\n", hash, state.error_bound);
            ++count;
        });
        printf("Verified %zu entries (NP=%d)\n", count, NP);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "Verification failed: %s\n", error.what()); return 1;
    }
}
