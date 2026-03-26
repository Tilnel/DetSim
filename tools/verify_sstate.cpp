/*
 * verify_sstate.cpp - Verify integrity of sstate (SysStateStore) files
 * 
 * Stub for get_ncurses_ui - defined in main.cpp which we don't link
 */
extern "C" {
    namespace detsim { namespace ui { class NCursesUI; } }
    detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
}

/*
 * Usage: ./verify_sstate [sstate_dir]
 *   Default sstate_dir is "./sstate"
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

/* Statistics */
struct VerifyStats {
    size_t total_entries = 0;
    size_t valid_entries = 0;
    size_t corrupted_entries = 0;
    size_t missing_entries = 0;
    
    // Error details
    size_t magic_mismatch = 0;
    size_t hash_mismatch = 0;
    size_t size_mismatch = 0;
    size_t checksum_mismatch = 0;
    size_t read_errors = 0;
    size_t deserialize_errors = 0;
    size_t zero_tshash = 0;  // ts_hash = 0 for non-exited process
};

/* Compute checksum same as SysStateStore */
uint32_t compute_checksum(const std::vector<uint8_t>& data) {
    uint32_t sum = 0;
    for (auto b : data) {
        sum = (sum * 31) + b;
    }
    return sum;
}

/* Read index file into memory */
bool read_index(const std::string& index_path, 
                std::unordered_map<hash_type, SysStateEntry>& index,
                bool is_incremental = false) {
    FILE* fp = fopen(index_path.c_str(), "rb");
    if (!fp) {
        printf("  Warning: Cannot open %s\n", index_path.c_str());
        return false;
    }
    
    /* Read header */
    if (is_incremental) {
        IncrementalHeader inc_header;
        if (fread(&inc_header, sizeof(inc_header), 1, fp) != 1) {
            printf("  Warning: Cannot read incremental header\n");
            fclose(fp);
            return false;
        }
        if (inc_header.magic != INCREMENTAL_MAGIC) {
            printf("  Error: Invalid incremental magic: 0x%08X (expected 0x%08X)\n",
                   inc_header.magic, INCREMENTAL_MAGIC);
            fclose(fp);
            return false;
        }
    } else {
        IndexHeader header;
        if (fread(&header, sizeof(header), 1, fp) != 1) {
            printf("  Warning: Cannot read index header\n");
            fclose(fp);
            return false;
        }
        if (header.magic != INDEX_MAGIC) {
            printf("  Error: Invalid index magic: 0x%08X (expected 0x%08X)\n",
                   header.magic, INDEX_MAGIC);
            fclose(fp);
            return false;
        }
        printf("  Index version: %u, entries: %lu\n", 
               header.version, (unsigned long)header.entry_count);
    }
    
    /* Read entries */
    size_t count = 0;
    while (true) {
        hash_type hash;
        SysStateEntry entry;
        
        if (fread(&hash, sizeof(hash), 1, fp) != 1) break;
        if (fread(&entry, sizeof(entry), 1, fp) != 1) {
            printf("  Warning: Truncated entry for hash %016lX\n", hash);
            break;
        }
        
        index[hash] = entry;
        count++;
    }
    
    fclose(fp);
    printf("  Read %zu entries from %s\n", count, index_path.c_str());
    return true;
}

/* Verify a single entry */
bool verify_entry(int data_fd, hash_type hash, const SysStateEntry& entry,
                  VerifyStats& stats, bool verbose) {
    stats.total_entries++;
    
    /* Seek to entry offset */
    if (lseek(data_fd, entry.offset - sizeof(DataEntryHeader), SEEK_SET) < 0) {
        if (verbose) {
            printf("  [%016lX] ERROR: Cannot seek to offset %lu\n",
                   hash, (unsigned long)(entry.offset - sizeof(DataEntryHeader)));
        }
        stats.read_errors++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Read header */
    DataEntryHeader header;
    ssize_t n = read(data_fd, &header, sizeof(header));
    if (n != sizeof(header)) {
        if (verbose) {
            printf("  [%016lX] ERROR: Cannot read header (read %zd bytes)\n", hash, n);
        }
        stats.read_errors++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Verify magic */
    if (header.magic != ENTRY_MAGIC) {
        if (verbose) {
            printf("  [%016lX] ERROR: Magic mismatch: 0x%08X (expected 0x%08X)\n",
                   hash, header.magic, ENTRY_MAGIC);
        }
        stats.magic_mismatch++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Verify hash */
    printf("  [%016lX]\n", hash);
    if (header.hash != hash) {
        if (verbose) {
            printf("  [%016lX] ERROR: Hash mismatch: stored %016lX\n",
                   hash, header.hash);
        }
        stats.hash_mismatch++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Verify size */
    if (header.size != entry.size) {
        if (verbose) {
            printf("  [%016lX] ERROR: Size mismatch: header=%u, index=%u\n",
                   hash, header.size, entry.size);
        }
        stats.size_mismatch++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Read data */
    std::vector<uint8_t> data(header.size);
    n = read(data_fd, data.data(), header.size);
    if (n != (ssize_t)header.size) {
        if (verbose) {
            printf("  [%016lX] ERROR: Cannot read data (expected %u, got %zd)\n",
                   hash, header.size, n);
        }
        stats.read_errors++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Verify checksum */
    uint32_t computed_checksum = compute_checksum(data);
    if (computed_checksum != header.checksum) {
        if (verbose) {
            printf("  [%016lX] ERROR: Checksum mismatch: computed=%08X, stored=%08X\n",
                   hash, computed_checksum, header.checksum);
        }
        stats.checksum_mismatch++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Also verify index checksum matches */
    if (computed_checksum != entry.checksum) {
        if (verbose) {
            printf("  [%016lX] ERROR: Index checksum mismatch: computed=%08X, index=%08X\n",
                   hash, computed_checksum, entry.checksum);
        }
        stats.checksum_mismatch++;
        stats.corrupted_entries++;
        return false;
    }
    
    /* Deserialize sys_state and check for ts_hash = 0 */
    try {
        std::istringstream iss(std::string((char*)data.data(), data.size()));
        cereal::BinaryInputArchive ar(iss);
        
        // Read ts_hash array directly (sys_state serializes ts_hash and exited)
        // Format: ts_hash[NP], exited[NP]
        hash_type ts_hash[NP];
        int exited[NP];
        ar(ts_hash, exited);
        
        // Check for ts_hash = 0 in non-exited processes
        for (int i = 0; i < NP; i++) {
            if (!exited[i] && ts_hash[i] == 0) {
                if (verbose) {
                    printf("  [%016lX] ERROR: ts_hash[%d] = 0 for non-exited process\n",
                           hash, i);
                }
                stats.zero_tshash++;
                stats.corrupted_entries++;
                return false;
            }
        }
    } catch (...) {
        if (verbose) {
            printf("  [%016lX] ERROR: Failed to deserialize sys_state\n", hash);
        }
        stats.deserialize_errors++;
        stats.corrupted_entries++;
        return false;
    }
    
    stats.valid_entries++;
    return true;
}

/* Print usage */
void print_usage(const char* prog) {
    printf("Usage: %s [options] [sstate_dir]\n", prog);
    printf("\nOptions:\n");
    printf("  -v, --verbose    Show details for each entry\n");
    printf("  -h, --help       Show this help\n");
    printf("\nArguments:\n");
    printf("  sstate_dir       Directory containing sstate files (default: ./sstate)\n");
}

int main(int argc, char* argv[]) {
    std::string sstate_dir = "./sstate";
    bool verbose = false;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            sstate_dir = argv[i];
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("=== SysStateStore Integrity Verifier ===\n");
    printf("SState directory: %s\n\n", sstate_dir.c_str());
    
    /* Build file paths */
    std::string data_path = sstate_dir + "/" + DATA_FILE;
    std::string index_path = sstate_dir + "/" + INDEX_FILE;
    std::string inc_path = sstate_dir + "/" + INCREMENTAL_FILE;
    
    /* Check if files exist */
    struct stat st;
    if (stat(data_path.c_str(), &st) != 0) {
        printf("Error: Data file not found: %s\n", data_path.c_str());
        return 1;
    }
    printf("Data file: %s (%zu bytes)\n", data_path.c_str(), (size_t)st.st_size);
    
    /* Open data file */
    int data_fd = open(data_path.c_str(), O_RDONLY);
    if (data_fd < 0) {
        printf("Error: Cannot open data file: %s\n", data_path.c_str());
        return 1;
    }
    
    /* Read main index */
    std::unordered_map<hash_type, SysStateEntry> index;
    printf("\nLoading main index...\n");
    bool has_main_index = read_index(index_path, index, false);
    
    /* Read incremental index (may be empty after merge) */
    printf("\nLoading incremental index...\n");
    bool has_inc_index = false;
    if (stat(inc_path.c_str(), &st) == 0 && st.st_size > 0) {
        has_inc_index = read_index(inc_path, index, true);
        if (!has_inc_index) {
            printf("  Note: Incremental index exists but is invalid or empty (may have been merged)\n");
        }
    } else {
        printf("  No incremental index (or empty)\n");
    }
    
    if (!has_main_index && !has_inc_index) {
        printf("Error: No index files found\n");
        close(data_fd);
        return 1;
    }
    
    printf("\nTotal unique entries to verify: %zu\n", index.size());
    
    /* Verify all entries */
    printf("\nVerifying entries...\n");
    VerifyStats stats;
    
    for (const auto& [hash, entry] : index) {
        verify_entry(data_fd, hash, entry, stats, verbose);
    }
    
    /* Scan for orphaned entries in data file */
    printf("\nScanning for orphaned entries in data file...\n");
    size_t orphaned = 0;
    size_t data_file_offset = 0;
    
    if (lseek(data_fd, 0, SEEK_SET) < 0) {
        printf("  Warning: Cannot seek to beginning of data file\n");
    } else {
        while (true) {
            DataEntryHeader header;
            ssize_t n = read(data_fd, &header, sizeof(header));
            if (n != sizeof(header)) break;
            
            if (header.magic == ENTRY_MAGIC) {
                auto it = index.find(header.hash);
                if (it == index.end()) {
                    /* Orphaned entry - not in index */
                    if (verbose || orphaned < 10) {
                        printf("  Orphaned entry at offset %zu: hash=%016lX, size=%u\n",
                               data_file_offset, header.hash, header.size);
                    }
                    orphaned++;
                }
                
                /* Skip data */
                if (lseek(data_fd, header.size, SEEK_CUR) < 0) break;
                data_file_offset += sizeof(header) + header.size;
            } else {
                /* Invalid magic, skip one byte and try again */
                if (lseek(data_fd, -(off_t)(sizeof(header) - 1), SEEK_CUR) < 0) break;
                data_file_offset++;
            }
        }
        
        if (orphaned > 0) {
            printf("  Found %zu orphaned entries (data exists but not in index)\n", orphaned);
        } else {
            printf("  No orphaned entries found\n");
        }
    }
    
    close(data_fd);
    
    /* Print summary */
    printf("\n=== Verification Summary ===\n");
    printf("Total entries:    %zu\n", stats.total_entries);
    printf("Valid:            %zu (%.1f%%)\n", 
           stats.valid_entries, 
           stats.total_entries > 0 ? (100.0 * stats.valid_entries / stats.total_entries) : 0);
    printf("Corrupted:        %zu (%.1f%%)\n", 
           stats.corrupted_entries,
           stats.total_entries > 0 ? (100.0 * stats.corrupted_entries / stats.total_entries) : 0);
    
    if (stats.corrupted_entries > 0) {
        printf("\nError breakdown:\n");
        printf("  Magic mismatch:       %zu\n", stats.magic_mismatch);
        printf("  Hash mismatch:        %zu\n", stats.hash_mismatch);
        printf("  Size mismatch:        %zu\n", stats.size_mismatch);
        printf("  Checksum mismatch:    %zu\n", stats.checksum_mismatch);
        printf("  Read errors:          %zu\n", stats.read_errors);
        printf("  Deserialize errors:   %zu\n", stats.deserialize_errors);
        printf("  Zero ts_hash errors:  %zu\n", stats.zero_tshash);
    }
    
    return stats.corrupted_entries > 0 ? 1 : 0;
}
