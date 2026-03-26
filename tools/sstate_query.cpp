/*
 * sstate_query.cpp - Query and verify sstate (SysStateStore) 
 * 
 * Stub for get_ncurses_ui - defined in main.cpp which we don't link
 */
extern "C" {
    namespace detsim { namespace ui { class NCursesUI; } }
    detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
}

/*
 * Usage:
 *   ./sstate_query search <ts_hash> [sstate_dir]  - Find which ss_hash contains this ts_hash
 *   ./sstate_query info <ss_hash> [sstate_dir]    - Show info about ss_hash and its ts_hashes
 *   ./sstate_query verify [sstate_dir]            - Verify integrity of all entries
 *   ./sstate_query list [sstate_dir]              - List all ss_hash entries
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

// For deserializing sys_state
#include "cereal/archives/binary.hpp"
#include "core/state.h"

/* File paths (relative to sstate directory) */
static const char* DATA_FILE = "packed.dat";
static const char* INDEX_FILE = "packed.idx";
static const char* INCREMENTAL_FILE = "packed.inc";

/* Magic numbers */
#define INDEX_MAGIC 0x53535953      // "SYSS"
#define ENTRY_MAGIC 0x53534545      // "EESS"
#define INCREMENTAL_MAGIC 0x494E4458 // "INDX"

using hash_type = uint64_t;

/* Data entry header in packed.dat - packed to 20 bytes */
struct __attribute__((packed)) DataEntryHeader {
    uint32_t magic;
    hash_type hash;
    uint32_t size;
    uint32_t checksum;
};
static_assert(sizeof(DataEntryHeader) == 20, "DataEntryHeader size must be 20 bytes");

/* Index entry */
struct SysStateEntry {
    uint64_t offset;
    uint32_t size;
    uint32_t checksum;
};

/* Index file header */
struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t entry_count;
};

/* Incremental index header */
struct IncrementalHeader {
    uint32_t magic;
    uint32_t reserved;
};

/* Parsed sys_state info */
struct SysStateInfo {
    hash_type ss_hash;
    hash_type ts_hash[NP];
    int status[NP];
    bool valid;
};

/* Global state */
std::string g_sstate_dir = "./sstate";
std::unordered_map<hash_type, SysStateEntry> g_index;
int g_data_fd = -1;

/* Compute checksum */
uint32_t compute_checksum(const std::vector<uint8_t>& data) {
    uint32_t sum = 0;
    for (auto b : data) {
        sum = (sum * 31) + b;
    }
    return sum;
}

/* Parse hash from hex string */
bool parse_hash(const char* str, hash_type& hash) {
    char* endptr;
    hash = strtoull(str, &endptr, 16);
    if (*endptr != '\0') {
        // Try with 0x prefix
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            hash = strtoull(str + 2, &endptr, 16);
        }
    }
    return *endptr == '\0';
}

/* Format hash to hex string */
std::string format_hash(hash_type hash) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lX", hash);
    return std::string(buf);
}

/* Read index file */
bool read_index(const std::string& index_path, bool is_incremental = false) {
    FILE* fp = fopen(index_path.c_str(), "rb");
    if (!fp) {
        printf("Warning: Cannot open %s\n", index_path.c_str());
        return false;
    }
    
    if (is_incremental) {
        IncrementalHeader inc_header;
        if (fread(&inc_header, sizeof(inc_header), 1, fp) != 1) {
            printf("Warning: Cannot read incremental header\n");
            fclose(fp);
            return false;
        }
        if (inc_header.magic != INCREMENTAL_MAGIC) {
            printf("Error: Invalid incremental magic: 0x%08X\n", inc_header.magic);
            fclose(fp);
            return false;
        }
    } else {
        IndexHeader header;
        if (fread(&header, sizeof(header), 1, fp) != 1) {
            printf("Warning: Cannot read index header\n");
            fclose(fp);
            return false;
        }
        if (header.magic != INDEX_MAGIC) {
            printf("Error: Invalid index magic: 0x%08X (expected 0x%08X)\n",
                   header.magic, INDEX_MAGIC);
            fclose(fp);
            return false;
        }
    }
    
    size_t count = 0;
    while (true) {
        hash_type hash;
        SysStateEntry entry;
        
        if (fread(&hash, sizeof(hash), 1, fp) != 1) break;
        if (fread(&entry, sizeof(entry), 1, fp) != 1) {
            printf("Warning: Truncated entry for hash %016lX\n", hash);
            break;
        }
        
        g_index[hash] = entry;
        count++;
    }
    
    fclose(fp);
    if (!is_incremental) {
        printf("  Loaded %zu entries from main index\n", count);
    } else {
        printf("  Loaded %zu entries from incremental index\n", count);
    }
    return true;
}

/* Load sys_state from data file */
SysStateInfo load_sys_state(hash_type ss_hash, const SysStateEntry& entry) {
    SysStateInfo info = {};
    info.ss_hash = ss_hash;
    info.valid = false;
    
    if (lseek(g_data_fd, entry.offset - sizeof(DataEntryHeader), SEEK_SET) < 0) {
        printf("Error: Cannot seek to offset %lu\n", 
               (unsigned long)(entry.offset - sizeof(DataEntryHeader)));
        return info;
    }
    
    DataEntryHeader header;
    ssize_t n = read(g_data_fd, &header, sizeof(header));
    if (n != sizeof(header)) {
        printf("Error: Cannot read header\n");
        return info;
    }
    
    if (header.magic != ENTRY_MAGIC) {
        printf("Error: Magic mismatch: 0x%08X\n", header.magic);
        return info;
    }
    
    if (header.hash != ss_hash) {
        printf("Error: Hash mismatch: stored %016lX\n", header.hash);
        return info;
    }
    
    std::vector<uint8_t> data(header.size);
    n = read(g_data_fd, data.data(), header.size);
    if (n != (ssize_t)header.size) {
        printf("Error: Cannot read data (expected %u, got %zd)\n", header.size, n);
        return info;
    }
    
    /* Deserialize to get ts_hash and status */
    try {
        std::istringstream iss(std::string((char*)data.data(), data.size()));
        cereal::BinaryInputArchive ar(iss);
        
        // Format: ts_hash[NP], status[NP] (ss_hash is only in index, not in data)
        // Note: child[] array is also serialized but we don't need it for query
        for (int i = 0; i < NP; i++) {
            ar(info.ts_hash[i]);
        }
        for (int i = 0; i < NP; i++) {
            ar(info.status[i]);
        }
        
        info.valid = true;
    } catch (const std::exception& e) {
        printf("Error: Failed to deserialize sys_state: %s\n", e.what());
    }
    
    return info;
}

/* Initialize and load data */
bool init() {
    std::string data_path = g_sstate_dir + "/" + DATA_FILE;
    std::string index_path = g_sstate_dir + "/" + INDEX_FILE;
    std::string inc_path = g_sstate_dir + "/" + INCREMENTAL_FILE;
    
    struct stat st;
    if (stat(data_path.c_str(), &st) != 0) {
        printf("Error: Data file not found: %s\n", data_path.c_str());
        return false;
    }
    
    g_data_fd = open(data_path.c_str(), O_RDONLY);
    if (g_data_fd < 0) {
        printf("Error: Cannot open data file: %s\n", data_path.c_str());
        return false;
    }
    
    printf("Loading index files...\n");
    bool has_main = read_index(index_path, false);
    
    struct stat st_inc;
    if (stat(inc_path.c_str(), &st_inc) == 0 && st_inc.st_size > 0) {
        read_index(inc_path, true);
    }
    
    if (g_index.empty()) {
        printf("Error: No index entries loaded\n");
        close(g_data_fd);
        g_data_fd = -1;
        return false;
    }
    
    printf("Total entries: %zu\n\n", g_index.size());
    return true;
}

/* Cleanup */
void cleanup() {
    if (g_data_fd >= 0) {
        close(g_data_fd);
        g_data_fd = -1;
    }
    g_index.clear();
}

/* Command: search - Find which ss_hash contains this ts_hash */
int cmd_search(hash_type target_ts_hash) {
    printf("=== Search ts_hash: %016lX ===\n\n", target_ts_hash);
    
    std::vector<std::pair<hash_type, SysStateInfo>> found;
    
    for (const auto& [ss_hash, entry] : g_index) {
        SysStateInfo info = load_sys_state(ss_hash, entry);
        if (!info.valid) continue;
        
        for (int i = 0; i < NP; i++) {
            if (info.ts_hash[i] == target_ts_hash) {
                found.push_back({ss_hash, info});
                break;
            }
        }
    }
    
    if (found.empty()) {
        printf("NOT FOUND: ts_hash %016lX does not exist in any ss_hash\n", target_ts_hash);
        return 1;
    }
    
    printf("FOUND: ts_hash %016lX found in %zu ss_hash(s):\n\n", target_ts_hash, found.size());
    
    for (const auto& [ss_hash, info] : found) {
        printf("  ss_hash: %016lX\n", ss_hash);
        for (int i = 0; i < NP; i++) {
            if (info.ts_hash[i] == target_ts_hash) {
                const char* status_str = info.status[i] ? "exited" : "running";
                printf("    -> pid[%d]: ts_hash=%016lX, status=%s\n", 
                       i, info.ts_hash[i], status_str);
            }
        }
        printf("\n");
    }
    
    return 0;
}

/* Command: info - Show info about ss_hash */
int cmd_info(hash_type target_ss_hash) {
    printf("=== Info for ss_hash: %016lX ===\n\n", target_ss_hash);
    
    auto it = g_index.find(target_ss_hash);
    if (it == g_index.end()) {
        printf("NOT FOUND: ss_hash %016lX not found in index\n", target_ss_hash);
        return 1;
    }
    
    printf("Index Entry:\n");
    printf("  ss_hash:  %016lX\n", target_ss_hash);
    printf("  offset:   %lu\n", (unsigned long)it->second.offset);
    printf("  size:     %u bytes\n", it->second.size);
    printf("  checksum: 0x%08X\n", it->second.checksum);
    printf("\n");
    
    SysStateInfo info = load_sys_state(target_ss_hash, it->second);
    if (!info.valid) {
        printf("Error: Failed to load sys_state data\n");
        return 1;
    }
    
    printf("State Info:\n");
    printf("  ss_hash:  %016lX (from index)\n", target_ss_hash);
    printf("\n  Per-process state:\n");
    
    for (int i = 0; i < NP; i++) {
        const char* status_str = info.status[i] ? "exited" : "running";
        printf("    pid[%d]: ts_hash=%016lX, status=%s\n", 
               i, info.ts_hash[i], status_str);
    }
    printf("\n");
    
    printf("Integrity: OK\n");
    return 0;
}

/* Command: verify - Verify integrity of all entries */
int cmd_verify() {
    printf("=== Verify All Entries ===\n\n");
    
    size_t total = 0;
    size_t valid = 0;
    size_t corrupted = 0;
    
    // Statistics
    size_t magic_err = 0;
    size_t hash_err = 0;
    size_t size_err = 0;
    size_t checksum_err = 0;
    size_t read_err = 0;
    size_t deserialize_err = 0;
    size_t ss_hash_mismatch = 0;
    
    // ts_hash index integrity
    size_t ts_hash_zero_count = 0;
    size_t ts_hash_valid_count = 0;
    
    for (const auto& [ss_hash, entry] : g_index) {
        total++;
        bool entry_ok = true;
        
        // Seek and read header
        if (lseek(g_data_fd, entry.offset - sizeof(DataEntryHeader), SEEK_SET) < 0) {
            printf("[%016lX] ERROR: Cannot seek\n", ss_hash);
            read_err++;
            corrupted++;
            continue;
        }
        
        DataEntryHeader header;
        ssize_t n = read(g_data_fd, &header, sizeof(header));
        if (n != sizeof(header)) {
            printf("[%016lX] ERROR: Cannot read header\n", ss_hash);
            read_err++;
            corrupted++;
            continue;
        }
        
        // Verify magic
        if (header.magic != ENTRY_MAGIC) {
            printf("[%016lX] ERROR: Magic mismatch: 0x%08X\n", ss_hash, header.magic);
            magic_err++;
            corrupted++;
            continue;
        }
        
        // Verify hash
        if (header.hash != ss_hash) {
            printf("[%016lX] ERROR: Hash mismatch: stored %016lX\n", ss_hash, header.hash);
            hash_err++;
            corrupted++;
            continue;
        }
        
        // Verify size
        if (header.size != entry.size) {
            printf("[%016lX] ERROR: Size mismatch: header=%u, index=%u\n",
                   ss_hash, header.size, entry.size);
            size_err++;
            corrupted++;
            continue;
        }
        
        // Read data
        std::vector<uint8_t> data(header.size);
        n = read(g_data_fd, data.data(), header.size);
        if (n != (ssize_t)header.size) {
            printf("[%016lX] ERROR: Cannot read data\n", ss_hash);
            read_err++;
            corrupted++;
            continue;
        }
        
        // Verify checksum
        uint32_t computed = compute_checksum(data);
        if (computed != header.checksum) {
            printf("[%016lX] ERROR: Checksum mismatch: computed=%08X, stored=%08X\n",
                   ss_hash, computed, header.checksum);
            checksum_err++;
            corrupted++;
            continue;
        }
        if (computed != entry.checksum) {
            printf("[%016lX] ERROR: Index checksum mismatch: computed=%08X, index=%08X\n",
                   ss_hash, computed, entry.checksum);
            checksum_err++;
            corrupted++;
            continue;
        }
        
        // Deserialize and verify ts_hash integrity
        try {
            std::istringstream iss(std::string((char*)data.data(), data.size()));
            cereal::BinaryInputArchive ar(iss);
            
            hash_type ts_hashes[NP];
            int statuses[NP];
            
            // Format: ts_hash[NP], status[NP] (ss_hash is only in index)
            for (int i = 0; i < NP; i++) ar(ts_hashes[i]);
            for (int i = 0; i < NP; i++) ar(statuses[i]);
            
            // Check ts_hash integrity
            for (int i = 0; i < NP; i++) {
                if (!statuses[i] && ts_hashes[i] == 0) {
                    printf("[%016lX] WARNING: ts_hash[%d]=0 for non-exited process\n", ss_hash, i);
                    ts_hash_zero_count++;
                } else {
                    ts_hash_valid_count++;
                }
            }
            
            valid++;
        } catch (const std::exception& e) {
            printf("[%016lX] ERROR: Deserialize failed: %s\n", ss_hash, e.what());
            deserialize_err++;
            corrupted++;
            continue;
        }
    }
    
    // Check for orphaned entries in data file
    printf("\nScanning for orphaned entries...\n");
    size_t orphaned = 0;
    
    if (lseek(g_data_fd, 0, SEEK_SET) < 0) {
        printf("  Warning: Cannot seek to beginning of data file\n");
    } else {
        size_t offset = 0;
        while (true) {
            DataEntryHeader header;
            ssize_t n = read(g_data_fd, &header, sizeof(header));
            if (n != sizeof(header)) break;
            
            if (header.magic == ENTRY_MAGIC) {
                auto it = g_index.find(header.hash);
                if (it == g_index.end()) {
                    if (orphaned < 10) {
                        printf("  Orphaned: offset=%zu, hash=%016lX, size=%u\n",
                               offset, header.hash, header.size);
                    }
                    orphaned++;
                }
                if (lseek(g_data_fd, header.size, SEEK_CUR) < 0) break;
                offset += sizeof(header) + header.size;
            } else {
                if (lseek(g_data_fd, -(off_t)(sizeof(header) - 1), SEEK_CUR) < 0) break;
                offset++;
            }
        }
    }
    
    // Print summary
    printf("\n=== Verification Summary ===\n");
    printf("Total entries:    %zu\n", total);
    printf("Valid:            %zu (%.1f%%)\n", valid, total > 0 ? 100.0 * valid / total : 0);
    printf("Corrupted:        %zu (%.1f%%)\n", corrupted, total > 0 ? 100.0 * corrupted / total : 0);
    printf("Orphaned:         %zu\n", orphaned);
    
    if (corrupted > 0) {
        printf("\nError breakdown:\n");
        printf("  Magic errors:         %zu\n", magic_err);
        printf("  Hash errors:          %zu\n", hash_err);
        printf("  Size errors:          %zu\n", size_err);
        printf("  Checksum errors:      %zu\n", checksum_err);
        printf("  Read errors:          %zu\n", read_err);
        printf("  Deserialize errors:   %zu\n", deserialize_err);
        printf("  ss_hash mismatches:   %zu\n", ss_hash_mismatch);
    }
    
    printf("\nts_hash integrity:\n");
    printf("  Valid ts_hashes:      %zu\n", ts_hash_valid_count);
    printf("  Zero ts_hashes:       %zu (in non-exited processes)\n", ts_hash_zero_count);
    
    return corrupted > 0 ? 1 : 0;
}

/* Command: list - List all ss_hash entries */
int cmd_list() {
    printf("=== List All Entries ===\n\n");
    
    // Create sorted list
    std::vector<std::pair<hash_type, SysStateEntry>> entries;
    for (const auto& [hash, entry] : g_index) {
        entries.push_back({hash, entry});
    }
    std::sort(entries.begin(), entries.end(), 
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    printf("Total: %zu entries\n\n", entries.size());
    printf("%-20s %-12s %-10s %-10s\n", "ss_hash", "offset", "size", "checksum");
    printf("%-20s %-12s %-10s %-10s\n", "--------------------", "------------", "----------", "----------");
    
    for (const auto& [hash, entry] : entries) {
        printf("%016lX   %-12lu 0x%-8X 0x%-8X\n",
               hash, (unsigned long)entry.offset, entry.size, entry.checksum);
    }
    
    return 0;
}

/* Print usage */
void print_usage(const char* prog) {
    printf("sstate_query - Query and verify SysStateStore\n\n");
    printf("Usage:\n");
    printf("  %s search <ts_hash> [sstate_dir]  Find which ss_hash contains this ts_hash\n", prog);
    printf("  %s info <ss_hash> [sstate_dir]    Show info about ss_hash and its ts_hashes\n", prog);
    printf("  %s verify [sstate_dir]            Verify integrity of all entries\n", prog);
    printf("  %s list [sstate_dir]              List all ss_hash entries\n", prog);
    printf("\nExamples:\n");
    printf("  %s search 0x05d71d971c08ec\n", prog);
    printf("  %s info 0x057f711dd0f438cd\n", prog);
    printf("  %s verify ./sstate\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* cmd = argv[1];
    
    // Special case for help
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    // Parse command-specific arguments
    hash_type hash_val = 0;
    int arg_idx = 2;
    
    // Commands that require a hash argument
    if (strcmp(cmd, "search") == 0 || strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            printf("Error: %s command requires a hash argument\n", cmd);
            return 1;
        }
        if (!parse_hash(argv[2], hash_val)) {
            printf("Error: Invalid hash format: %s\n", argv[2]);
            return 1;
        }
        arg_idx = 3;
    }
    
    // Optional sstate_dir argument
    if (argc > arg_idx) {
        g_sstate_dir = argv[arg_idx];
    }
    
    printf("SState directory: %s\n\n", g_sstate_dir.c_str());
    
    // Initialize
    if (!init()) {
        return 1;
    }
    
    // Execute command
    int result = 0;
    if (strcmp(cmd, "search") == 0) {
        result = cmd_search(hash_val);
    } else if (strcmp(cmd, "info") == 0) {
        result = cmd_info(hash_val);
    } else if (strcmp(cmd, "verify") == 0) {
        result = cmd_verify();
    } else if (strcmp(cmd, "list") == 0) {
        result = cmd_list();
    } else {
        printf("Error: Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        result = 1;
    }
    
    cleanup();
    return result;
}
