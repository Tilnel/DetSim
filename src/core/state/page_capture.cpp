#include "page_capture.h"
#include "snapshot_memory.h"
#include <sys/mman.h>
#include "soft_dirty_experiment.h"
#include <cstdlib>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/uio.h>

namespace {
struct SavedPage {
    PageRef ref;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
struct Scratch {
    uint64_t generation = 0;
    std::vector<uint8_t> memory;
    std::vector<SavedPage> previous;
};
// Only a byte-equality hint: every capture still reads every supported region.
// Fixed live tracees own at most one working image each; no search states pinned.
std::unordered_map<pid_t, Scratch> scratch;
struct Stats {
    uint64_t captures=0, pages=0, reused=0, saved=0, read_bytes=0, oracle_bytes=0;
} stats;

void read_regions(pid_t pid, const std::vector<detsim::experiment::Region>& ranges,
                  const std::vector<RegionInfo>& regions) {
    if (ranges.size() != regions.size()) throw std::runtime_error("Page capture region mismatch");
    detsim::GuardMemoryReader guard_memory(pid);
    for (size_t start=0; start<ranges.size(); start+=256) {
        iovec local[256], remote[256];
        const size_t count=std::min(size_t(256),ranges.size()-start);
        size_t expected=0, readable_count=0;
        for (size_t i=0;i<count;++i) {
            const auto& r=ranges[start+i];
            if (regions[start+i].prot == PROT_NONE) {
                guard_memory.read(r.address,r.data,r.size);
                continue;
            }
            if (r.size>size_t(SSIZE_MAX)-expected) throw std::runtime_error("Page capture transfer overflow");
            expected+=r.size;
            local[readable_count]={r.data,r.size};
            remote[readable_count]={reinterpret_cast<void*>(r.address),r.size};
            ++readable_count;
        }
        if (!readable_count) continue;
        const auto got=process_vm_readv(pid,local,readable_count,remote,readable_count,0);
        if (got<0 || size_t(got)!=expected) throw std::runtime_error("Page capture read failed or short");
    }
}

void normalize(std::vector<uint8_t>& bytes, const std::vector<RegionInfo>& regions,
               uintptr_t address, int length) {
    if (length<0) throw std::runtime_error("Invalid rseq size");
    size_t offset=0;
    for (const auto& r:regions) {
        if (address>=r.start && address<r.end) {
            if (size_t(length)>r.end-address) throw std::runtime_error("rseq extends beyond captured region");
            memset(bytes.data()+offset+address-r.start,0x55,size_t(length));
        }
        offset+=r.end-r.start;
    }
}
}

bool page_capture_enabled() {
    const char* mode=getenv("DETSIM_SNAPSHOT_BACKEND");
    if (!mode || !strcmp(mode,"full")) return false;
    if (strcmp(mode,"pages") && strcmp(mode,"verify-pages"))
        throw std::runtime_error("Invalid DETSIM_SNAPSHOT_BACKEND");
    return true;
}

hash_type save_paged_snapshot(pid_t pid, const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs, uintptr_t rseq_address, int rseq_length,
    hash_type reference_hash) {
    if (sysconf(_SC_PAGESIZE)!=4096) throw std::runtime_error("Page backend requires 4KiB host pages");
    auto& store=StateStore::instance();
    auto& work=scratch[pid];
    if (work.generation!=store.generation()) {work=Scratch{};work.generation=store.generation();}
    size_t total=0;
    for (const auto& r:regions) {
        if (r.end<=r.start || r.end-r.start>SIZE_MAX-total) throw std::runtime_error("Page capture extent overflow");
        total+=r.end-r.start;
    }
    work.memory.resize(total);
    std::vector<detsim::experiment::Region> reads;
    size_t offset=0;
    for (const auto& r:regions) {
        reads.push_back({uintptr_t(r.start),size_t(r.end-r.start),work.memory.data()+offset});
        offset+=r.end-r.start;
    }
    if (auto* experiment=detsim::experiment::configured_capture()) {
        for (const auto& region:regions)
            if (region.prot == PROT_NONE)
                throw std::runtime_error("Soft-dirty capture does not support PROT_NONE mappings");
        const auto before=experiment->stats().bytes_read;
        experiment->capture(pid,reads,std::string(maps));
        stats.read_bytes+=experiment->stats().bytes_read-before;
    } else {
        read_regions(pid,reads,regions);
        stats.read_bytes+=total;
    }
    normalize(work.memory,regions,rseq_address,rseq_length);

    std::vector<RegionInfo> descriptors=regions;
    std::vector<PageRef> refs;
    std::vector<SavedPage> next;
    offset=0;
    for (auto& r:descriptors) {
        r.offset=refs.size();
        for (uint64_t address=r.start;address<r.end;) {
            const auto length=uint32_t(std::min(r.end-address,4096-address%4096));
            const uint8_t* bytes=work.memory.data()+offset;
            const size_t index=refs.size();
            const bool same=index<work.previous.size() &&
                work.previous[index].ref.address==address && work.previous[index].ref.length==length &&
                !memcmp(work.previous[index].bytes->data(),bytes,length);
            SavedPage saved;
            if (same) {saved=work.previous[index];++stats.reused;}
            else {
                const auto key=store.save(bytes,length);
                auto owner=store.load_shared(key);
                if (!owner || owner->size()!=length) throw std::runtime_error("Cannot retain captured page");
                saved={{address,length,key},std::move(owner)};
                ++stats.saved;
            }
            refs.push_back(saved.ref);next.push_back(std::move(saved));
            address+=length;offset+=length;
        }
    }
    auto manifest=encode_page_manifest(descriptors,metadata,maps,regs,refs);
    const auto root=store.save(manifest.data(),manifest.size(),reference_hash);
    const char* mode=getenv("DETSIM_SNAPSHOT_BACKEND");
    if (mode && !strcmp(mode,"verify-pages")) {
        std::vector<uint8_t> oracle(total);
        offset=0;
        for (auto& read:reads) {read.data=oracle.data()+offset;offset+=read.size;}
        read_regions(pid,reads,regions);stats.oracle_bytes+=total;
        normalize(oracle,regions,rseq_address,rseq_length);
        const auto view=SnapshotView::open(root);
        if (view->maps_text()!=maps || view->metadata().size()!=metadata.size() ||
            memcmp(view->metadata().data(),metadata.data(),metadata.size()) ||
            memcmp(&view->main_regs(),&regs,sizeof(regs)))
            throw std::runtime_error("Page snapshot metadata disagrees with full capture");
        offset=0;
        std::vector<uint8_t> decoded;
        for (const auto& r:regions) {
            decoded.resize(r.end-r.start);
            view->read_into(r.start,decoded.data(),decoded.size());
            if (memcmp(decoded.data(),oracle.data()+offset,decoded.size()))
                throw std::runtime_error("Page snapshot disagrees with full memory oracle");
            offset+=decoded.size();
        }
    }
    work.previous=std::move(next);
    work.generation=store.generation();
    ++stats.captures;stats.pages+=refs.size();
    return root;
}

void reset_page_capture_stats() {stats={};}
void report_page_capture_stats() {
    fprintf(stdout,"PAGE_CAPTURE {\"captures\":%llu,\"pages\":%llu,\"reused\":%llu,\"saved\":%llu,\"read_bytes\":%llu,\"oracle_bytes\":%llu}\n",
        (unsigned long long)stats.captures,(unsigned long long)stats.pages,(unsigned long long)stats.reused,
        (unsigned long long)stats.saved,(unsigned long long)stats.read_bytes,(unsigned long long)stats.oracle_bytes);
}
