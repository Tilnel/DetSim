/*
 * sysstate_store_benchmark.cpp - Benchmark for SysStateStore
 * 
 * Tests sys_state storage with controlled deduplication rate
 * Monitors memory growth and disk usage over time
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#include "sysstate_store.h"
#include "utils.h"
#include "debug.h"
#include "monitor.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <numeric>
#include <algorithm>
#include <atomic>

extern int loglevel;

// --- Memory Monitoring ---

// Get current RSS memory in bytes
size_t get_current_rss() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;
    
    char line[256];
    size_t rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %zu", &rss);
            rss *= 1024; // Convert KB to bytes
            break;
        }
    }
    fclose(fp);
    return rss;
}

// Get current heap size from malloc_info (approximate)
size_t get_heap_size() {
#if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 33
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    return mi.uordblks + mi.fordblks; // Used + Free blocks
}

// --- SysState Data Generation ---

// Template for sys_state simulation
struct SysStateTemplate {
    hash_type ss_hash;
    hash_type ts_hashes[3];  // Support up to NP=3
    int exited[3];
    
    SysStateTemplate() {
        ss_hash = 0;
        for (int i = 0; i < 3; i++) {
            ts_hashes[i] = 0;
            exited[i] = 0;
        }
    }
};

class SysStateGenerator {
public:
    std::vector<SysStateTemplate> templates;
    std::mt19937 rng;
    
    void generate_templates(size_t count) {
        std::uniform_int_distribution<hash_type> hash_dist(1, 0xFFFFFFFF);
        std::uniform_int_distribution<int> exit_dist(0, 1);
        
        for (size_t i = 0; i < count; i++) {
            SysStateTemplate tmpl;
            tmpl.ss_hash = hash_dist(rng);
            for (int j = 0; j < 3; j++) {
                tmpl.ts_hashes[j] = hash_dist(rng);
                tmpl.exited[j] = (j == 2) ? 1 : 0; // Last process often exited
            }
            templates.push_back(tmpl);
        }
    }
    
    // Generate a sys_state based on template with perturbations
    std::vector<uint8_t> generate_state(const SysStateTemplate& tmpl, int perturbation_percent = 5) {
        // Simulate sys_state serialization (similar to cereal)
        // Format: [ss_hash:4][ts_hash0:4][ts_hash1:4][ts_hash2:4][exited0:1][exited1:1][exited2:1]
        std::vector<uint8_t> data;
        
        // Add ss_hash
        for (int i = 0; i < 4; i++) {
            data.push_back((tmpl.ss_hash >> (i * 8)) & 0xFF);
        }
        
        // Add ts_hashes with possible perturbation
        std::uniform_int_distribution<int> perturb_dist(0, 99);
        std::uniform_int_distribution<hash_type> hash_dist(1, 0xFFFFFFFF);
        
        for (int j = 0; j < 3; j++) {
            hash_type ts_hash = tmpl.ts_hashes[j];
            // Perturb with some probability
            if (perturb_dist(rng) < perturbation_percent) {
                ts_hash = hash_dist(rng); // Completely different
            }
            for (int i = 0; i < 4; i++) {
                data.push_back((ts_hash >> (i * 8)) & 0xFF);
            }
        }
        
        // Add exited flags
        for (int j = 0; j < 3; j++) {
            data.push_back(tmpl.exited[j]);
        }
        
        // Add some padding to simulate real sys_state size (~100-200 bytes)
        size_t padding_size = 100 + (rng() % 100);
        for (size_t i = 0; i < padding_size; i++) {
            data.push_back(rng() % 256);
        }
        
        return data;
    }
    
    // Generate completely random state
    std::vector<uint8_t> generate_random_state() {
        std::uniform_int_distribution<hash_type> hash_dist(1, 0xFFFFFFFF);
        SysStateTemplate tmpl;
        tmpl.ss_hash = hash_dist(rng);
        for (int j = 0; j < 3; j++) {
            tmpl.ts_hashes[j] = hash_dist(rng);
            tmpl.exited[j] = (rng() % 2);
        }
        return generate_state(tmpl, 100); // 100% perturbation = random
    }
};

// --- Monitoring Thread ---

struct MemorySample {
    double timestamp;
    size_t rss_mb;
    size_t heap_mb;
    size_t index_entries;
    size_t incremental_entries;
    size_t data_file_size_mb;
    size_t saves_total;
    size_t saves_new;
};

std::vector<MemorySample> memory_samples;
std::atomic<size_t> total_saves{0};
std::atomic<size_t> new_saves{0};

void monitor_thread() {
    auto& store = SysStateStore::instance();
    auto start_time = std::chrono::steady_clock::now();
    
    // Print CSV header
    printf("\n=== Memory Growth Monitor ===\n");
    printf("Time(s),RSS(MB),Heap(MB),IndexEntries,IncEntries,DataFile(MB),TotalSaves,NewSaves,SaveRate\n");
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();
        
        size_t rss = get_current_rss() / (1024 * 1024);
        size_t heap = get_heap_size() / (1024 * 1024);
        size_t index_entries = store.get_entry_count();
        size_t incremental_entries = store.get_incremental_count();
        size_t data_size = store.get_data_file_size() / (1024 * 1024);
        
        size_t total = total_saves.load();
        size_t new_count = new_saves.load();
        
        MemorySample sample;
        sample.timestamp = elapsed;
        sample.rss_mb = rss;
        sample.heap_mb = heap;
        sample.index_entries = index_entries;
        sample.incremental_entries = incremental_entries;
        sample.data_file_size_mb = data_size;
        sample.saves_total = total;
        sample.saves_new = new_count;
        
        memory_samples.push_back(sample);
        
        // Print CSV row
        printf("%.1f,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.2f\n",
               elapsed, rss, heap, index_entries, incremental_entries, data_size,
               total, new_count, total > 0 ? (100.0 * (total - new_count) / total) : 0);
        
        // Also print human readable summary every 10 seconds
        if ((int)elapsed % 10 == 0 && elapsed > 0) {
            printf("\n[Summary @ %.0fs]\n", elapsed);
            printf("  Memory: RSS=%zu MB, Heap=%zu MB\n", rss, heap);
            printf("  Store: %zu entries, %zu MB data file\n", index_entries, data_size);
            printf("  Saves: %zu total, %zu new (%.1f%% dedup)\n", 
                   total, new_count, total > 0 ? (100.0 * (total - new_count) / total) : 0);
            
            // Calculate growth rate
            if (memory_samples.size() >= 10) {
                auto& old = memory_samples[memory_samples.size() - 10];
                double dt = sample.timestamp - old.timestamp;
                if (dt > 0) {
                    double rss_growth = (double)(sample.rss_mb - old.rss_mb) / dt;
                    double data_growth = (double)(sample.data_file_size_mb - old.data_file_size_mb) / dt;
                    printf("  Growth Rate: RSS=%.2f MB/s, Data=%.2f MB/s\n", rss_growth, data_growth);
                }
            }
        }
    }
}

// --- Main Benchmark ---

int main() {
    // --- Configuration ---
    loglevel = 2;
    const size_t num_templates = 1000;          // Number of base sys_state templates
    const int dedup_probability_percent = 90;   // 90% chance to use template (high dedup)
    const int perturbation_percent = 2;         // 2% perturbation (small changes like real raft)
    const size_t target_runtime_seconds = 120;  // Run for 2 minutes
    
    printf("=== SysStateStore Benchmark ===\n");
    printf("Configuration:\n");
    printf("  Templates: %zu\n", num_templates);
    printf("  Dedup Probability: %d%%\n", dedup_probability_percent);
    printf("  Perturbation: %d%%\n", perturbation_percent);
    printf("  Target Runtime: %zu seconds\n", target_runtime_seconds);
    
    // --- Initialization ---
    printf("\nGenerating template pool...\n");
    SysStateGenerator generator;
    generator.generate_templates(num_templates);
    printf("Generated %zu templates\n", generator.templates.size());
    
    printf("\nInitializing SysStateStore...\n");
    SysStateStore::instance().init();
    
    // --- Start Monitoring ---
    std::thread monitor(monitor_thread);
    monitor.detach();
    
    // --- Benchmark Globals ---
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dedup_dist(0, 99);
    std::uniform_int_distribution<size_t> template_idx_dist(0, num_templates - 1);
    std::vector<hash_type> saved_hashes;
    saved_hashes.reserve(10000);
    
    printf("\nBenchmark starting...\n");
    auto start_time = std::chrono::steady_clock::now();
    
    // --- Main Loop ---
    size_t iteration = 0;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();
        
        if (elapsed >= target_runtime_seconds) {
            printf("\nTarget runtime reached. Stopping...\n");
            break;
        }
        
        // Generate sys_state data with controlled deduplication
        std::vector<uint8_t> data;
        hash_type hash;
        bool is_dedup = (dedup_dist(rng) < dedup_probability_percent);
        
        if (is_dedup) {
            // Use template index to create actual deduplication
            // Same template index -> Same hash -> Deduplication
            size_t idx = template_idx_dist(rng);
            data = generator.generate_state(generator.templates[idx], perturbation_percent);
            // Deterministic hash based on template index
            hash = (hash_type)(idx * 1234567ULL + 1);
        } else {
            // Random state with content-based hash
            data = generator.generate_random_state();
            hash = XXHash64::hash(data.data(), data.size(), 0);
        }
        
        // Check if this would be a new save or dedup
        bool exists = SysStateStore::instance().exists(hash);
        
        // Save to SysStateStore
        bool saved = SysStateStore::instance().save(hash, data);
        
        if (saved) {
            total_saves++;
            if (!exists) {
                new_saves++;
            }
            saved_hashes.push_back(hash);
        }
        
        iteration++;
        
        // Throttle to avoid overwhelming CPU
        // Target: ~1000 saves per second
        if (iteration % 1000 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // --- Final Report ---
    printf("\n=== Final Report ===\n");
    printf("Total iterations: %zu\n", iteration);
    printf("Total saves: %zu\n", total_saves.load());
    printf("New saves: %zu\n", new_saves.load());
    printf("Dedup rate: %.2f%%\n", 
           100.0 * (total_saves.load() - new_saves.load()) / total_saves.load());
    
    // Calculate average growth rates
    if (memory_samples.size() >= 2) {
        auto& first = memory_samples.front();
        auto& last = memory_samples.back();
        double dt = last.timestamp - first.timestamp;
        
        if (dt > 0) {
            printf("\nAverage Growth Rates:\n");
            printf("  RSS: %.3f MB/s\n", (double)(last.rss_mb - first.rss_mb) / dt);
            printf("  Heap: %.3f MB/s\n", (double)(last.heap_mb - first.heap_mb) / dt);
            printf("  Data File: %.3f MB/s\n", (double)(last.data_file_size_mb - first.data_file_size_mb) / dt);
            printf("  Entries: %.1f entries/s\n", (double)(last.index_entries - first.index_entries) / dt);
        }
    }
    
    // Cleanup
    printf("\nShutting down...\n");
    SysStateStore::instance().shutdown();
    printf("Done.\n");
    
    return 0;
}
