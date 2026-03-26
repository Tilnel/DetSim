/*
 * xxhash_benchmark.cpp - Benchmark XXH64 vs XXH32 throughput
 */

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <xxhash.h>

static inline uint64_t get_time_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

static std::string format_bytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = bytes;
    while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f%s", size, units[unit]); return buf;
}

static std::string format_speed(double bytes_per_sec) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    double speed = bytes_per_sec;
    while (speed >= 1024 && unit < 3) { speed /= 1024; unit++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f%s", speed, units[unit]); return buf;
}

int main() {
    printf("=== XXHash Benchmark ===\n\n");
    
    // Test data sizes (typical state sizes)
    std::vector<size_t> sizes = {64, 256, 512, 1024, 4096, 65536, 524288, 1048576};
    // Scale iterations inversely with size
    std::vector<int> iterations_list = {5000000, 2000000, 1000000, 500000, 100000, 10000, 2000, 2000};
    
    // Random data generator
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    
    std::vector<uint8_t> max_data(524288);
    for (auto& b : max_data) b = (uint8_t)dist(rng);
    
    printf("%-10s %-15s %-20s\n", 
           "Size", "Iterations", "XXH64");
    printf("%-10s %-15s %-20s\n", 
           "==========", "===============", "====================");
    
    for (size_t idx = 0; idx < sizes.size(); idx++) {
        size_t size = sizes[idx];
        int iterations = iterations_list[idx];
        
        std::vector<uint8_t> data(max_data.begin(), max_data.begin() + size);
        volatile uint64_t hash64 = 0;
        
        // Warmup
        for (int i = 0; i < std::min(10000, iterations/10); i++) {
            hash64 = XXH3_64bits(data.data(), size);
        }
        
        // Benchmark XXH64
        uint64_t start = get_time_ns();
        for (int i = 0; i < iterations; i++) {
            hash64 = XXH3_64bits(data.data(), size);
        }
        uint64_t elapsed64 = get_time_ns() - start;

        
        // Calculate throughputs
        double bytes_processed = (double)size * iterations;
        double speed64 = bytes_processed / (elapsed64 / 1e9);

        printf("%-10s %-15d %-20s\n",
               format_bytes(size).c_str(),
               iterations,
               format_speed(speed64).c_str());
        
        // Prevent optimization
        (void)hash64; 
    }
    
    printf("\n=== Summary ===\n");
    printf("\nFor StateStore use case (512KB states):\n");
    printf("- XXH64 provides better hash quality (64-bit space)\n");
    
    return 0;
}
