/*
 * state_store_benchmark.cpp - Benchmark for StateStore
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include "state_store.h"
#include "utils.h"
#include "debug.h"
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <numeric>
#include <algorithm>

extern int loglevel;

// --- Data Generation with Controlled Deduplication ---

// Template pool for generating duplicate states
struct TemplatePool {
    std::vector<std::vector<uint8_t>> templates;
    std::mt19937 rng;
    
    void generate_templates(size_t count, size_t min_size, size_t max_size) {
        std::uniform_int_distribution<size_t> size_dist(min_size, max_size);
        std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
        
        for (size_t i = 0; i < count; i++) {
            size_t len = size_dist(rng);
            std::vector<uint8_t> data(len);
            std::generate(data.begin(), data.end(), [&]() { return byte_dist(rng); });
            templates.push_back(std::move(data));
        }
    }
    
    // Get a template with small perturbations (simulates "almost same" states)
    std::vector<uint8_t> get_perturbed_template(size_t idx, int perturbation_percent = 5) {
        if (idx >= templates.size()) idx = idx % templates.size();
        std::vector<uint8_t> data = templates[idx];  // Copy
        
        // Perturb a small percentage of bytes (simulates real state changes)
        std::uniform_int_distribution<size_t> pos_dist(0, data.size() - 1);
        std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
        size_t num_perturb = data.size() * perturbation_percent / 100;
        
        for (size_t i = 0; i < num_perturb; i++) {
            data[pos_dist(rng)] = byte_dist(rng);
        }
        return data;
    }
};

// Function to generate a vector of random bytes
std::vector<uint8_t> generate_random_data(size_t len) {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::vector<uint8_t> data(len);
    std::generate(data.begin(), data.end(), [&]() { return dist(gen); });
    return data;
}

// Monitoring thread function
void monitor_thread() {
    auto& store = StateStore::instance();
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto now = std::chrono::steady_clock::now();
        auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        size_t l1_usage = store.get_l1_usage();
        size_t l2_usage = store.get_l2_usage();
        size_t total_usage = l1_usage + l2_usage;

        printf("\n--- Monitor [T=%zds] ---\n", elapsed_seconds);
        printf("Memory Usage:\n");
        printf("  L1 (Hot):   %8.2f MB (%zu entries)\n", l1_usage / (1024.0 * 1024.0), store.get_l1_entry_count());
        printf("  L2 (Warm):  %8.2f MB (%zu entries)\n", l2_usage / (1024.0 * 1024.0), store.get_l2_entry_count());
        printf("  Total:      %8.2f MB\n", total_usage / (1024.0 * 1024.0));
        printf("Queue Sizes:\n");
        printf("  I/O Queue:       %zu / %zu\n", store.get_io_queue_size(), store.get_io_queue_capacity());
        printf("  Prefetch Queue:  %zu\n", store.get_prefetch_queue_size());
        
        store.print_stats();
        store.reset_stats(); // Reset stats each interval for fresh readings
    }
}

int main() {
    // --- Configuration ---
    loglevel = 2;
    const size_t min_data_size = 4 * 1024;      // 4 KB
    const size_t max_data_size = 256 * 1024;    // 256 KB
    const size_t history_size = 2000;          // Keep track of the last 2000 saved hashes
    const int load_probability_percent = 40;   // 40% chance to perform a load operation
    
    // --- Deduplication Configuration ---
    const size_t num_templates = 100;           // Number of base templates
    const int dedup_probability_percent = 75;   // 75% chance to use a template (simulates high dedup)
    const int perturbation_percent = 3;         // 3% byte perturbation (small changes)

    // --- Initialization ---
    printf("Initializing StateStore Benchmark...\n");
    printf("Configuration:\n");
    printf("  Data size: %zu KB - %zu KB\n", min_data_size/1024, max_data_size/1024);
    printf("  Templates: %zu (dedup rate target: ~%d%%)\n", num_templates, dedup_probability_percent);
    printf("  Perturbation: %d%% (simulates small state changes)\n", perturbation_percent);
    
    // Generate template pool for deduplication
    printf("Generating template pool...\n");
    TemplatePool template_pool;
    template_pool.generate_templates(num_templates, min_data_size, max_data_size);
    printf("Generated %zu templates\n", template_pool.templates.size());
    
    StateStore::Config config;
    config.enable_malloc_trim = true;
    config.malloc_trim_threshold = 64 * 1024 * 1024;
    StateStore::instance().init(config);

    // --- Start Monitoring Thread ---
    std::thread monitor(monitor_thread);
    monitor.detach();

    // --- Benchmark Globals ---
    std::vector<hash_type> saved_hashes;
    saved_hashes.reserve(history_size);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> size_dist(min_data_size, max_data_size);
    std::uniform_int_distribution<int> load_chance_dist(0, 99);
    std::uniform_int_distribution<int> dedup_chance_dist(0, 99);
    std::uniform_int_distribution<size_t> template_idx_dist(0, num_templates - 1);

    printf("Benchmark starting. It will run indefinitely. Press Ctrl+C to stop.\n");

    // --- Main Benchmark Loop ---
    size_t total_saves = 0;
    size_t dedup_saves = 0;
    
    while (true) {
        // 1. Generate data (with deduplication simulation)
        std::vector<uint8_t> data;
        bool is_dedup = (dedup_chance_dist(rng) < dedup_probability_percent);
        
        if (is_dedup) {
            // Use a template with small perturbations (simulates "almost same" state)
            size_t template_idx = template_idx_dist(rng);
            data = template_pool.get_perturbed_template(template_idx, perturbation_percent);
            dedup_saves++;
        } else {
            // Generate completely new random data
            size_t data_size = size_dist(rng);
            data = generate_random_data(data_size);
        }
        total_saves++;
        
        // 2. Save the state
        hash_type hash = StateStore::instance().save(data.data(), data.size());

        // 2. Add to state queue to simulate a trace and drive prefetching
        StateStore::instance().queue_push_back(hash);
        
        // 3. Keep track of recent hashes
        if (saved_hashes.size() < history_size) {
            saved_hashes.push_back(hash);
        } else {
            // Overwrite a random old entry to keep memory usage of the benchmark itself stable
            static std::uniform_int_distribution<size_t> history_idx_dist(0, history_size - 1);
            saved_hashes[history_idx_dist(rng)] = hash;
        }

        // 4. Simulate processing the queue (drives prefetching)
        if (StateStore::instance().queue_size() > config.prefetch_window) {
             StateStore::instance().queue_pop_front();
        }

        // 5. Randomly decide to load a previous state
        if (!saved_hashes.empty() && load_chance_dist(rng) < load_probability_percent) {
            std::uniform_int_distribution<size_t> hash_idx_dist(0, saved_hashes.size() - 1);
            hash_type hash_to_load = saved_hashes[hash_idx_dist(rng)];
            
            std::vector<uint8_t> loaded_data;
            ssize_t result = StateStore::instance().load(hash_to_load, loaded_data);

            if (result < 0) {
                // This might happen if a state was evicted and its hash was overwritten in our history
                // LOG_WARN("Failed to load hash %08x (might have been evicted)", hash_to_load);
            }
        }

        // Optional: slow down loop slightly to avoid overwhelming CPU
        // std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // This part is unreachable due to the infinite loop, but good practice
    StateStore::instance().shutdown();
    return 0;
}
