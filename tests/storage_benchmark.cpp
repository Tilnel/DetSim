#include "state_store.h"
#include "sysstate_store.h"
#include <filesystem>
#include <unistd.h>
#include "config.h"
#include "ncurses_ui.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
using Clock = std::chrono::steady_clock;
static std::vector<uint8_t> payload(unsigned n) {
    std::vector<uint8_t> v(128 * 1024);
    uint64_t x = 0x5f3759df + n;
    for (size_t i = 0; i < v.size(); ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        v[i] = (i % 4096 < 3072) ? uint8_t(n) : uint8_t(x);
    }
    return v;
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    loglevel = 5;
    try {
        if (std::string(argv[2]) == "metadata") {
            std::filesystem::create_directories(argv[1]);
            if (chdir(argv[1])) throw std::runtime_error("benchmark chdir");
            auto& metadata = SysStateStore::instance(); metadata.init();
            std::vector<uint8_t> v(32);
            auto begin = Clock::now();
            for (uint64_t i = 1; i <= 10000; ++i) {
                v[0] = i & 255; v[1] = (i >> 8) & 255;
                if (!metadata.save(i, v)) throw std::runtime_error("metadata save");
            }
            metadata.flush_index();
            auto saved = Clock::now();
            for (int round = 0; round < 4; ++round)
                for (uint64_t i = 1; i <= 10000; ++i) {
                    std::vector<uint8_t> out;
                    v[0] = i & 255; v[1] = (i >> 8) & 255;
                    if (!metadata.load(i, out) || out != v) throw std::runtime_error("metadata bytes differ");
                }
            auto loaded = Clock::now();
            metadata.shutdown();
            rusage usage{}; getrusage(RUSAGE_SELF, &usage);
            auto ms = [](auto a, auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
            printf("{\"label\":\"%s\",\"mode\":\"metadata\",\"states\":10000,\"save_barrier_ms\":%.3f,\"read_4x_ms\":%.3f,\"rss_kib\":%ld}\n",argv[3],ms(begin,saved),ms(saved,loaded),usage.ru_maxrss);
            return 0;
        }
        StateStore::Config c;
        c.packed_storage_path = argv[1];
        const bool pressure = std::string(argv[2]) == "pressure";
        c.hot_cache_size = pressure ? 256*1024 : 64*1024*1024;
        c.warm_cache_size = pressure ? 128*1024 : 64*1024*1024;
        c.prefetch_threads = 0; c.prefetch_window = 0;
        auto& s = StateStore::instance(); s.init(c);
        std::vector<std::vector<uint8_t>> data;
        std::vector<hash_type> hashes;
        for (unsigned i = 0; i < 128; ++i) data.push_back(payload(i));
        const auto begin = Clock::now();
        for (const auto& v : data) hashes.push_back(s.save(v.data(), v.size()));
        s.wait_for_completion();
        const auto saved = Clock::now();
        for (int round = 0; round < 4; ++round)
            for (size_t i = 0; i < data.size(); ++i) {
                auto v = s.load_shared(hashes[i]);
                if (!v || *v != data[i]) throw std::runtime_error("snapshot bytes differ");
            }
        const auto loaded = Clock::now();
        for (const auto& v : data) s.save(v.data(), v.size());
        s.wait_for_completion();
        const auto dedup = Clock::now();
        const auto stats = s.get_stats();
        const auto l1 = s.get_l1_usage(), l2 = s.get_l2_usage();
        s.shutdown();
        rusage usage{}; getrusage(RUSAGE_SELF, &usage);
        auto ms = [](auto a, auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
        printf("{\"label\":\"%s\",\"mode\":\"%s\",\"states\":128,\"state_bytes\":131072,\"save_barrier_ms\":%.3f,\"read_4x_ms\":%.3f,\"dedup_ms\":%.3f,\"rss_kib\":%ld,\"l1_bytes\":%zu,\"l2_bytes\":%zu,\"disk_reads\":%lu}\n", argv[3],argv[2],ms(begin,saved),ms(saved,loaded),ms(loaded,dedup),usage.ru_maxrss,l1,l2,stats.disk_reads);
    } catch (const std::exception& e) { fprintf(stderr,"benchmark failed: %s\n",e.what()); return 1; }
}
