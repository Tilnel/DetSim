#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace detsim::page_restore {
struct Stats {
    uint64_t read_bytes, write_bytes, pages_compared, pages_written;
    uint64_t zero_guard_bytes, zero_guard_regions, zero_guard_fallbacks;
};
inline std::atomic<uint64_t> read_bytes{0}, write_bytes{0}, pages_compared{0}, pages_written{0};
inline std::atomic<uint64_t> zero_guard_bytes{0}, zero_guard_regions{0}, zero_guard_fallbacks{0};
inline Stats stats() {
    return {read_bytes.load(std::memory_order_relaxed), write_bytes.load(std::memory_order_relaxed),
            pages_compared.load(std::memory_order_relaxed), pages_written.load(std::memory_order_relaxed),
            zero_guard_bytes.load(std::memory_order_relaxed), zero_guard_regions.load(std::memory_order_relaxed),
            zero_guard_fallbacks.load(std::memory_order_relaxed)};
}
inline void reset() {
    read_bytes.store(0, std::memory_order_relaxed); write_bytes.store(0, std::memory_order_relaxed);
    pages_compared.store(0, std::memory_order_relaxed); pages_written.store(0, std::memory_order_relaxed);
    zero_guard_bytes.store(0, std::memory_order_relaxed); zero_guard_regions.store(0, std::memory_order_relaxed);
    zero_guard_fallbacks.store(0, std::memory_order_relaxed);
}
inline void report(FILE* output = stdout) {
    const auto s = stats();
    fprintf(output, "PAGE_RESTORE {\"read_bytes\":%llu,\"write_bytes\":%llu,\"pages_compared\":%llu,\"pages_written\":%llu,\"zero_guard_bytes\":%llu,\"zero_guard_regions\":%llu,\"zero_guard_fallbacks\":%llu}\n",
            static_cast<unsigned long long>(s.read_bytes), static_cast<unsigned long long>(s.write_bytes),
            static_cast<unsigned long long>(s.pages_compared), static_cast<unsigned long long>(s.pages_written),
            static_cast<unsigned long long>(s.zero_guard_bytes), static_cast<unsigned long long>(s.zero_guard_regions),
            static_cast<unsigned long long>(s.zero_guard_fallbacks));
}
} // namespace detsim::page_restore
